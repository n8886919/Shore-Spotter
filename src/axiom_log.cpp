#if defined(ROLE_STATION)
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <atomic>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_sntp.h>
#include <cJSON.h>
#include <sys/time.h>
#include "axiom_log.h"

namespace axiom_log {
namespace {
enum class State { Idle, Wifi, Clock, Memory, Waiting, Sending, Backoff, Rejected, Storage, Task };
const char *stateName(State s) {
  switch (s) {
    case State::Wifi: return "waiting_wifi";
    case State::Clock: return "waiting_time";
    case State::Memory: return "low_memory";
    case State::Waiting: return "waiting_batch";
    case State::Sending: return "uploading";
    case State::Backoff: return "backoff";
    case State::Rejected: return "rejected";
    case State::Storage: return "storage_error";
    case State::Task: return "task_error";
    default: return "idle";
  }
}
struct Shared {
  Config config, pending;
  bool saving = false, inFlight = false;
  uint32_t generation = 1, queued = 0, dropped = 0, sent = 0, failed = 0;
  uint32_t requests = 0, captureLastUs = 0, captureMaxUs = 0, bootId = 0;
  uint64_t bytes = 0;
  uint32_t lastSuccessMs = 0, requestMs = 0, retryAt = 0, saveId = 0;
  bool haveSuccess = false;
  int httpStatus = 0, transportError = 0;
  State state = State::Idle;
} shared;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
// Atomic flags provide the cheap disabled path on every pass through loop().
std::atomic<bool> collecting{false}, otaPaused{false};
std::atomic<uint32_t> generation{1};
QueueHandle_t samples = nullptr;
TaskHandle_t task = nullptr;
uint32_t nextSampleMs = 0, sampleId = 0;
void state(State value, uint32_t retryAt = 0) {
  portENTER_CRITICAL(&mux); shared.state = value; shared.retryAt = retryAt; portEXIT_CRITICAL(&mux);
}
void drop(uint32_t count = 1) {
  portENTER_CRITICAL(&mux); shared.dropped += count; portEXIT_CRITICAL(&mux);
}
bool persist(const Config &config) {
  // One blob gives all-or-nothing settings across power loss. This namespace and
  // Preferences object are owned exclusively by the worker after begin().
  Preferences nvs;
  if (!nvs.begin("axiom_log", false)) return false;
  Config readback;
  const bool ok = nvs.putBytes("config", &config, sizeof(config)) == sizeof(config) &&
      nvs.getBytes("config", &readback, sizeof(readback)) == sizeof(readback) &&
      memcmp(&config, &readback, sizeof(config)) == 0;
  nvs.end(); return ok;
}
struct Response {
  char body[1024]{};
  size_t used = 0;
  bool overflow = false;
  uint32_t retryAfter = 0;
};
esp_err_t receiveHttp(esp_http_client_event_t *event) {
  auto &r = *static_cast<Response *>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
    if (r.used + size_t(event->data_len) >= sizeof(r.body)) r.overflow = true;
    else { memcpy(r.body + r.used, event->data, event->data_len); r.used += event->data_len; r.body[r.used] = 0; }
  } else if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
             strcasecmp(event->header_key, "Retry-After") == 0) {
    char *end = nullptr;
    const unsigned long seconds = strtoul(event->header_value, &end, 10);
    if (end != event->header_value && !*end) r.retryAfter = seconds > 86400 ? 86400 : seconds;
    else {
      struct tm utc{};
      end = strptime(event->header_value, "%a, %d %b %Y %H:%M:%S GMT", &utc);
      if (end && !*end) {
        // configTime(0, 0, ...) above fixes the process timezone to UTC.
        const int64_t wait = static_cast<int64_t>(mktime(&utc)) - time(nullptr);
        if (wait > 0) r.retryAfter = wait > 86400 ? 86400 : wait;
      }
    }
  }
  return ESP_OK;
}
void discardQueued() {
  Sample discarded;
  while (xQueueReceive(samples, &discarded, 0) == pdTRUE) drop();
}
void worker(void *) {
  char *batch = nullptr;
  LogHistory history;
  bool ntpStarted = false, rejected = false;
  uint32_t failureStreak = 0, nextUpload = millis() + kFlushMs;
  for (;;) {
    Config config;
    bool save;
    portENTER_CRITICAL(&mux);
    save = shared.saving; config = save ? shared.pending : shared.config;
    portEXIT_CRITICAL(&mux);
    if (save) {
      discardQueued();
      const bool saved = persist(config);
      portENTER_CRITICAL(&mux);
      if (saved) shared.config = config;
      collecting.store(saved && config.enabled);
      shared.saving = false; shared.state = saved ? State::Idle : State::Storage;
      shared.retryAt = 0;
      portEXIT_CRITICAL(&mux);
      rejected = !saved; failureStreak = 0; nextUpload = millis() + kFlushMs;
      // A failed save leaves the runtime paused until a successful settings save.
    }
    if (!collecting.load() || otaPaused.load() || rejected) {
      history.valid = false;
      if (ntpStarted) { esp_sntp_stop(); ntpStarted = false; }
      discardQueued(); free(batch); batch = nullptr;
      vTaskDelay(pdMS_TO_TICKS(100)); continue;
    }
    if (WiFi.status() != WL_CONNECTED) { state(State::Wifi); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    if (!ntpStarted) { configTime(0, 0, "pool.ntp.org", "time.cloudflare.com"); ntpStarted = true; }
    if (time(nullptr) < 1735689600) { state(State::Clock); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    if (!loop_metrics::due(millis(), nextUpload)) {
      portENTER_CRITICAL(&mux);
      if (shared.state == State::Wifi || shared.state == State::Clock) {
        shared.state = failureStreak ? State::Backoff : State::Waiting;
        shared.retryAt = failureStreak ? nextUpload : 0;
      }
      portEXIT_CRITICAL(&mux);
      vTaskDelay(pdMS_TO_TICKS(100)); continue;
    }
    // Heap checks happen before batch allocation AND TLS. Wi-Fi and control must
    // retain headroom even if the free space is fragmented.
    if (ESP.getFreeHeap() < kMinUploadHeap + (batch ? 0 : kBatchBytes) ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 48 * 1024) {
      state(State::Memory); nextUpload = millis() + kFlushMs; vTaskDelay(pdMS_TO_TICKS(100)); continue;
    }
    if (!batch) batch = static_cast<char *>(malloc(kBatchBytes));
    if (!batch) { state(State::Memory); nextUpload = millis() + kFlushMs; continue; }
    const uint32_t batchGeneration = generation.load();
    uint32_t dropped, maxCapture;
    portENTER_CRITICAL(&mux); dropped = shared.dropped; maxCapture = shared.captureMaxUs; portEXIT_CRITICAL(&mux);
    size_t bytes = 0, count = 0;
    // Bound queue work even when samples arrive while encoding.
    for (size_t i = 0; i < kBatchSamples; ++i) {
      Sample sample;
      if (xQueueReceive(samples, &sample, 0) != pdTRUE) break;
      const uint32_t age = millis() - sample.ms;
      if (age > kMaxAgeMs || sample.generation != batchGeneration) { drop(); continue; }
      struct timeval now{}; gettimeofday(&now, nullptr);
      const int64_t observedMs = int64_t(now.tv_sec) * 1000 + now.tv_usec / 1000 - age;
      size_t records = 0;
      const size_t length = encodeSample(batch + bytes, kBatchBytes - bytes, sample, observedMs, dropped, maxCapture,
                                        &history, &records, true); // Preserve the deployed Axiom schema 2 field paths.
      if (!length) { drop(); continue; }
      bytes += length; count += records;
    }
    nextUpload = millis() + kFlushMs;
    if (!count) { state(State::Waiting); continue; }
    if (!collecting.load() || otaPaused.load() || batchGeneration != generation.load()) { drop(count); continue; }
    char url[160], auth[sizeof(config.token) + 8];
    snprintf(url, sizeof(url), "https://%s/v1/ingest/%s", host(config.region), config.dataset);
    snprintf(auth, sizeof(auth), "Bearer %s", config.token);
    Response response;
    esp_http_client_config_t options{};
    options.url = url; options.method = HTTP_METHOD_POST;
    options.timeout_ms = 3000;
    options.crt_bundle_attach = esp_crt_bundle_attach;
    options.disable_auto_redirect = true; // Never forward the token to another host.
    options.event_handler = receiveHttp; options.user_data = &response;
    options.buffer_size = 1024; options.buffer_size_tx = 1024;
    auto client = esp_http_client_init(&options);
    esp_err_t result = ESP_ERR_NO_MEM;
    int code = 0;
    const uint32_t started = millis();
    portENTER_CRITICAL(&mux); shared.inFlight = true; shared.state = State::Sending; ++shared.requests; portEXIT_CRITICAL(&mux);
    if (client) {
      result = esp_http_client_set_header(client, "Authorization", auth);
      if (result == ESP_OK) result = esp_http_client_set_header(client, "Content-Type", "application/x-ndjson");
      if (result == ESP_OK) result = esp_http_client_set_post_field(client, batch, bytes);
      if (result == ESP_OK && collecting.load() && !otaPaused.load() && batchGeneration == generation.load())
        result = esp_http_client_perform(client);
      else if (result == ESP_OK) result = ESP_ERR_INVALID_STATE;
      code = esp_http_client_get_status_code(client);
      esp_http_client_cleanup(client);
    }
    // Bounded parsing; response bodies are never logged (may echo submitted data).
    uint32_t ingested = 0;
    bool success = false;
    if (result == ESP_OK && code == 200 && !response.overflow) {
      cJSON *body = cJSON_Parse(response.body);
      const cJSON *accepted = cJSON_GetObjectItemCaseSensitive(body, "ingested");
      const cJSON *failed = cJSON_GetObjectItemCaseSensitive(body, "failed");
      if (cJSON_IsNumber(accepted) && cJSON_IsNumber(failed) &&
          accepted->valuedouble >= 0 && accepted->valuedouble <= count &&
          accepted->valuedouble == accepted->valueint && failed->valuedouble >= 0 &&
          accepted->valuedouble + failed->valuedouble == count) {
        ingested = accepted->valueint; success = ingested == count && failed->valuedouble == 0;
      }
      cJSON_Delete(body);
    }
    // Failed or ambiguous batches are discarded, never replayed. Counters and
    // (node_id, boot_id, sample_id) make gaps/ambiguous delivery distinguishable.
    portENTER_CRITICAL(&mux);
    shared.inFlight = false; shared.httpStatus = code; shared.transportError = result;
    shared.requestMs = millis() - started; shared.sent += ingested;
    shared.failed += count - ingested; shared.dropped += count - ingested;
    if (success) { shared.bytes += bytes; shared.haveSuccess = true; shared.lastSuccessMs = millis(); }
    portEXIT_CRITICAL(&mux);
    if (success) { failureStreak = 0; state(State::Waiting); }
    else if (code == 400 || code == 401 || code == 403 || code == 404 || code == 413 || code == 422) {
      rejected = true; collecting.store(false); state(State::Rejected);
    } else {
      if (failureStreak < 32) ++failureStreak;
      nextUpload = millis() + backoffMs(failureStreak, response.retryAfter);
      state(State::Backoff, nextUpload);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
bool startTask() {
  if (task) return true;
  samples = xQueueCreate(kQueueSamples, sizeof(Sample));
  if (!samples) { state(State::Task); return false; }
  // ESP32-S3: Wi-Fi's higher-priority tasks share core 0, loop runs on core 1.
  const BaseType_t otherCore = CONFIG_ARDUINO_RUNNING_CORE == 1 ? 0 : 1;
  if (xTaskCreatePinnedToCore(worker, "axiom", 12288, nullptr, 1, &task, otherCore) != pdPASS) {
    vQueueDelete(samples); samples = nullptr; task = nullptr; state(State::Task); return false;
  }
  return true;
}
} // namespace

void begin(uint32_t bootId) {
  Preferences nvs; Config config;
  if (nvs.begin("axiom_log", true)) {
    if (nvs.getBytesLength("config") != sizeof(config) ||
        nvs.getBytes("config", &config, sizeof(config)) != sizeof(config) || !validConfig(config)) config = Config{};
    nvs.end();
  }
  shared.config = config; shared.bootId = bootId;
  if (config.enabled && startTask()) collecting.store(true);
}
bool getConfig(Config &out) {
  portENTER_CRITICAL(&mux); out = shared.config; const bool ready = !shared.saving; portEXIT_CRITICAL(&mux);
  return ready;
}
bool configure(const Config &config) {
  if (!validConfig(config)) return false;
  portENTER_CRITICAL(&mux); const bool busy = shared.saving; portEXIT_CRITICAL(&mux);
  if (busy || !startTask()) return false;
  collecting.store(false);
  nextSampleMs = millis(); // Re-enable after a long disabled period, including millis half-range.
  const uint32_t next = generation.fetch_add(1) + 1;
  portENTER_CRITICAL(&mux);
  shared.pending = config; shared.saving = true; shared.generation = next; ++shared.saveId;
  portEXIT_CRITICAL(&mux);
  return true;
}
bool captureDue(uint32_t now, bool busy) {
  if (!collecting.load() || otaPaused.load()) return false;
  if (!loop_metrics::due(now, nextSampleMs)) return false;
  nextSampleMs = now + kSampleMs;
  if (busy || ESP.getFreeHeap() < kMinCaptureHeap || uxQueueSpacesAvailable(samples) == 0) { drop(); return false; }
  return true;
}
void submit(Sample &sample, uint32_t startedUs) {
  sample.generation = generation.load(); sample.id = ++sampleId;
  const bool accepted = collecting.load() && !otaPaused.load() && xQueueSend(samples, &sample, 0) == pdTRUE;
  const uint32_t elapsed = micros() - startedUs;
  portENTER_CRITICAL(&mux);
  if (accepted) ++shared.queued; else ++shared.dropped;
  shared.captureLastUs = elapsed;
  if (elapsed > shared.captureMaxUs) shared.captureMaxUs = elapsed;
  portEXIT_CRITICAL(&mux);
}
void pauseForOta(bool paused) { otaPaused.store(paused); }
String statusJson() {
  // Snapshot includes a secret internally; format only an explicit public allowlist.
  Shared s;
  portENTER_CRITICAL(&mux); s = shared; portEXIT_CRITICAL(&mux);
  const uint32_t now = millis();
  const char *status = s.saving ? "saving" : otaPaused.load() ? "ota_paused" :
      !s.config.enabled && s.state != State::Storage && s.state != State::Task ? "disabled" : stateName(s.state);
  String js; js.reserve(1000);
  js = "{\"ok\":true,\"enabled\":"; js += s.config.enabled ? "true" : "false";
  js += ",\"dataset\":\""; js += s.config.dataset;
  js += "\",\"region\":\""; js += s.config.region ? "eu" : "us";
  js += "\",\"token_set\":"; js += s.config.token[0] ? "true" : "false";
  js += ",\"state\":\""; js += status; js += '"';
  js += ",\"saving\":"; js += s.saving ? "true" : "false";
  js += ",\"in_flight\":"; js += s.inFlight ? "true" : "false";
  auto number = [&](const char *name, uint32_t value) { js += ",\""; js += name; js += "\":"; js += String(value); };
  number("boot_id", s.bootId); number("save_id", s.saveId); number("queue_depth", samples ? uxQueueMessagesWaiting(samples) : 0);
  number("queue_capacity", kQueueSamples); number("sample_ms", kSampleMs); number("flush_ms", kFlushMs);
  number("sample_bytes", sizeof(Sample));
  number("sent_samples", s.sent); number("dropped_samples", s.dropped); number("failed_samples", s.failed);
  js += ",\"sent_bytes\":"; js += String(static_cast<unsigned long long>(s.bytes));
  number("requests", s.requests);
  number("capture_last_us", s.captureLastUs); number("capture_max_us", s.captureMaxUs);
  number("request_ms", s.requestMs);
  number("worker_stack_free_min", task ? uxTaskGetStackHighWaterMark(task) : 0);
  number("retry_in_ms", s.retryAt && !loop_metrics::due(now, s.retryAt) ? s.retryAt - now : 0);
  js += ",\"http_status\":"; js += String(s.httpStatus);
  js += ",\"transport_error\":"; js += String(s.transportError);
  js += ",\"last_success_age_ms\":"; js += s.haveSuccess ? String(uint32_t(now - s.lastSuccessMs)) : "null";
  js += '}'; return js;
}
} // namespace axiom_log
#endif
