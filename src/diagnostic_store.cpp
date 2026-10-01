#if defined(ARDUINO)
#include "diagnostic_store.h"
#if defined(FIELD_DIAGNOSTIC) || defined(BOARD_HELTEC_V4)
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace diagnostic_store {
namespace {
enum class State { Scanning, Recording, Full, Unknown, Error, Erasing };
struct Status {
  State state = State::Scanning;
  char error[48]{};
  uint32_t usedFrames = 0, validFrames = 0, corruptFrames = 0, records = 0;
  uint32_t accepted = 0, dropped = 0, queueHigh = 0, errors = 0, usbErrors = 0, commandDrops = 0;
  uint32_t writeMaxUs = 0, pendingRecords = 0, eraseCount = 0;
} shared;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
struct Record { uint32_t ms; uint16_t kind, length; uint8_t payload[codec::kMaxRecordBytes]; };
enum class Op { Status, Read, Erase, Invalid };
struct Command { Op op = Op::Invalid; uint32_t start = 0, count = 0; bool all = false; };
QueueHandle_t records = nullptr, commands = nullptr;
TaskHandle_t task = nullptr;
std::atomic<bool> accepting{false}, usbActive{false};
const esp_partition_t *partition = nullptr;
uint32_t bootId = 0, nextIndex = 0, nextSequence = 0, bufferedAt = 0;
uint8_t frame[codec::kFrameBytes], verify[codec::kFrameBytes];
bool buffered = false, initialized = false, reading = false;
uint32_t readStart = 0, readCount = 0, readSent = 0;
constexpr uint32_t kCapacity = codec::kPartitionBytes / codec::kFrameBytes;

const char *stateName(State state) {
  switch (state) {
    case State::Scanning: return "scanning"; case State::Recording: return "recording";
    case State::Full: return "full"; case State::Unknown: return "unknown";
    case State::Error: return "error"; case State::Erasing: return "erasing";
  }
  return "error";
}
void setState(State state, const char *error = "") {
  portENTER_CRITICAL(&mux);
  shared.state = state; snprintf(shared.error, sizeof(shared.error), "%s", error);
  portEXIT_CRITICAL(&mux);
}
void discard() {
  Record record; uint32_t lost = buffered ? codec::get16(frame + 10) : 0;
  while (records && xQueueReceive(records, &record, 0) == pdTRUE) ++lost;
  buffered = false;
  portENTER_CRITICAL(&mux); shared.dropped += lost; shared.pendingRecords = 0; portEXIT_CRITICAL(&mux);
}
void fail(State state, const char *error) {
  accepting.store(false); discard();
  portENTER_CRITICAL(&mux); ++shared.errors; portEXIT_CRITICAL(&mux);
  setState(state, error);
}
bool readFrame(uint32_t index, uint8_t *out) {
  return partition && index < kCapacity &&
    esp_partition_read(partition, size_t(index) * codec::kFrameBytes, out, codec::kFrameBytes) == ESP_OK;
}
bool writeFrame(uint32_t index, const uint8_t *data) {
  // Check erased bytes immediately before writing. Never rewrite a partial frame.
  if (!readFrame(index, verify) || !codec::erased(verify, sizeof(verify))) return false;
  const uint32_t started = micros();
  const bool ok = esp_partition_write(partition, size_t(index) * codec::kFrameBytes, data, codec::kFrameBytes) == ESP_OK;
  const uint32_t elapsed = micros() - started;
  portENTER_CRITICAL(&mux); if (elapsed > shared.writeMaxUs) shared.writeMaxUs = elapsed; portEXIT_CRITICAL(&mux);
  return ok && readFrame(index, verify) && memcmp(data, verify, codec::kFrameBytes) == 0 && codec::validCrc(verify);
}
bool flushFrame() {
  if (!buffered) return true;
  if (nextIndex >= kCapacity) { fail(State::Full, "capacity_reached"); return false; }
  // Persist the cumulative loss counter in every frame, including queue overflow.
  portENTER_CRITICAL(&mux); const uint32_t lost = shared.dropped; portEXIT_CRITICAL(&mux);
  codec::put32(frame + 24, lost);
  codec::seal(frame);
  const uint16_t count = codec::get16(frame + 10);
  const uint32_t index = nextIndex++;
  // Include attempted/torn writes in READ immediately, even before the next boot.
  portENTER_CRITICAL(&mux); shared.usedFrames = nextIndex; portEXIT_CRITICAL(&mux);
  if (!writeFrame(index, frame)) {
    portENTER_CRITICAL(&mux); ++shared.corruptFrames; portEXIT_CRITICAL(&mux);
    fail(State::Error, "write_or_readback_failed"); return false;
  }
  buffered = false; ++nextSequence;
  portENTER_CRITICAL(&mux);
  ++shared.validFrames; shared.records += count; shared.pendingRecords = 0;
  portEXIT_CRITICAL(&mux);
  if (nextIndex == kCapacity) { accepting.store(false); discard(); setState(State::Full, "capacity_reached"); }
  return true;
}
void scan() {
  partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!partition || partition->address != codec::kPartitionAddress || partition->size != codec::kPartitionBytes || partition->encrypted) {
    partition = nullptr; // ERASE must never reach an unexpected partition either.
    fail(State::Error, "partition_layout_mismatch"); return;
  }
  bool allErased = true, owned = false;
  uint32_t valid = 0, corrupt = 0, storedRecords = 0, high = 0;
  for (uint32_t index = 0; index < kCapacity; ++index) {
    if (!readFrame(index, verify)) { fail(State::Error, "scan_read_failed"); return; }
    if (!codec::erased(verify, sizeof(verify))) {
      allErased = false; high = index + 1;
      if (index == 0) owned = codec::validHeader(verify);
      else if (codec::validFrame(verify)) { ++valid; storedRecords += codec::get16(verify + 10); }
      else ++corrupt;
    }
    if ((index & 7U) == 7U) vTaskDelay(pdMS_TO_TICKS(1));
  }
  nextIndex = high;
  portENTER_CRITICAL(&mux);
  shared.usedFrames = high; shared.validFrames = valid; shared.corruptFrames = corrupt; shared.records = storedRecords;
  portEXIT_CRITICAL(&mux);
  if (!owned && !allErased) { fail(State::Unknown, "unrecognized_partition_preserved"); return; }
  if (allErased) {
    codec::makeHeader(frame); nextIndex = 1;
    portENTER_CRITICAL(&mux); shared.usedFrames = 1; portEXIT_CRITICAL(&mux);
    if (!writeFrame(0, frame)) { fail(State::Error, "header_write_failed"); return; }
  }
  if (nextIndex == kCapacity) { accepting.store(false); discard(); setState(State::Full, "capacity_reached"); return; }
  setState(State::Recording); accepting.store(true);
}
void drainRecords() {
  if (!accepting.load()) return;
  Record record;
  // A bounded batch lets USB commands and the idle task make progress.
  for (size_t n = 0; n < kQueueRecords && xQueueReceive(records, &record, 0) == pdTRUE; ++n) {
    if (buffered && codec::get16(frame + 8) + codec::kEnvelopeBytes + record.length > codec::kPayloadBytes) {
      if (!flushFrame() || !accepting.load()) {
        portENTER_CRITICAL(&mux); ++shared.dropped; portEXIT_CRITICAL(&mux); return;
      }
    }
    if (!buffered) {
      codec::beginFrame(frame, bootId, nextSequence, record.ms); bufferedAt = millis(); buffered = true;
    }
    codec::append(frame, record.kind, record.payload, record.length, record.ms);
    portENTER_CRITICAL(&mux); shared.pendingRecords = codec::get16(frame + 10); portEXIT_CRITICAL(&mux);
    if (codec::get16(frame + 8) == codec::kPayloadBytes && !flushFrame()) return;
  }
  if (buffered && uint32_t(millis() - bufferedAt) >= kFlushMs) flushFrame();
}
bool usbWrite(const char *data, size_t length) {
  uint32_t lastProgress = millis();
  while (length) {
    const int available = Serial.availableForWrite();
    const size_t take = available > 0 ? (length < size_t(available) ? length : size_t(available)) : 0;
    if (take) {
      const size_t written = Serial.write(reinterpret_cast<const uint8_t *>(data), take < 64 ? take : 64);
      if (written) { data += written; length -= written; lastProgress = millis(); }
    }
    if (uint32_t(millis() - lastProgress) >= 2000) return false;
    if (length) vTaskDelay(pdMS_TO_TICKS(1));
  }
  return true;
}
bool usbLine(const char *text) { return usbWrite(text, strlen(text)); }
void usbFailed() {
  portENTER_CRITICAL(&mux); ++shared.usbErrors; portEXIT_CRITICAL(&mux);
  reading = false; usbActive.store(false);
}
void readNext() {
  if (!reading) return;
  if (readSent == readCount) {
    char tail[80]; snprintf(tail, sizeof(tail), "@DIAG END %lu %lu\n", (unsigned long)readStart, (unsigned long)readSent);
    if (!usbLine(tail)) { usbFailed(); return; }
    reading = false; usbActive.store(false); return;
  }
  uint8_t data[codec::kFrameBytes];
  if (!readFrame(readStart + readSent, data)) {
    usbLine("@DIAG ERROR read_failed\n"); usbFailed(); return;
  }
  char line[1120];
  const int prefix = snprintf(line, sizeof(line), "@DIAG FRAME %lu %08lx ",
    (unsigned long)(readStart + readSent), (unsigned long)codec::crc32(data, sizeof(data)));
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(data); ++i) { line[prefix + i * 2] = hex[data[i] >> 4]; line[prefix + i * 2 + 1] = hex[data[i] & 15]; }
  line[prefix + sizeof(data) * 2] = '\n';
  if (!usbWrite(line, prefix + sizeof(data) * 2 + 1)) { usbFailed(); return; }
  ++readSent;
}
void erase() {
  accepting.store(false); discard(); setState(State::Erasing);
  if (!partition) { fail(State::Error, "partition_unavailable"); usbLine("@DIAG ERROR partition_unavailable\n"); return; }
  // Only the explicit ERASE CONFIRM command reaches here. Yield between sectors.
  for (size_t offset = 0; offset < codec::kPartitionBytes; offset += 4096) {
    if (esp_partition_erase_range(partition, offset, 4096) != ESP_OK) {
      fail(State::Error, "erase_failed"); usbLine("@DIAG ERROR erase_failed\n"); return;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  nextIndex = 0; nextSequence = 0;
  portENTER_CRITICAL(&mux); ++shared.eraseCount; portEXIT_CRITICAL(&mux);
  scan();
  if (!usbLine(accepting.load() ? "@DIAG ERASE OK\n" : "@DIAG ERROR erase_scan_failed\n")) usbFailed();
}
void processCommand(const Command &cmd) {
  // A console line may have ended at its bounded 256-byte chunk boundary.
  if (!usbLine("\n")) { usbFailed(); return; }
  if (cmd.op == Op::Status) {
    const String json = statusJson();
    if (!usbLine("@DIAG STATUS ") || !usbWrite(json.c_str(), json.length()) || !usbLine("\n")) usbFailed();
  } else if (cmd.op == Op::Read) {
    if (accepting.load() && !flushFrame()) { /* A failed flush is still downloadable. */ }
    const uint32_t available = nextIndex;
    readStart = cmd.start; readCount = cmd.all ? available : cmd.count; readSent = 0;
    if (!partition || readStart > available || readCount > available - readStart) {
      if (!usbLine("@DIAG ERROR read_range\n")) usbFailed();
    } else {
      char head[80]; snprintf(head, sizeof(head), "@DIAG BEGIN %lu %lu\n", (unsigned long)readStart, (unsigned long)readCount);
      reading = usbLine(head); if (!reading) usbFailed();
    }
  } else if (cmd.op == Op::Erase) erase();
  else if (!usbLine("@DIAG ERROR syntax\n")) usbFailed();
  if (!reading) usbActive.store(false);
}
void worker(void *) {
  if (!initialized) { scan(); initialized = true; }
  for (;;) {
    drainRecords();
    Command cmd;
    if (!reading && xQueueReceive(commands, &cmd, 0) == pdTRUE) processCommand(cmd);
    if (reading) readNext();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
} // namespace

void begin(uint32_t boot) {
  if (task) return;
  bootId = boot;
  records = xQueueCreate(kQueueRecords, sizeof(Record)); commands = xQueueCreate(1, sizeof(Command));
  if (!records || !commands) { fail(State::Error, "queue_allocation_failed"); return; }
  accepting.store(true); // Preserve early boot records while the worker scans.
  if (xTaskCreatePinnedToCore(worker, "diag-flash", 6144, nullptr, 1, &task, 0) != pdPASS) {
    task = nullptr; fail(State::Error, "task_allocation_failed");
  }
}
bool submit(uint16_t kind, const void *payload, size_t length, uint32_t ms) {
  if (!records || !accepting.load() || length > codec::kMaxRecordBytes || (length && !payload)) {
    portENTER_CRITICAL(&mux); ++shared.dropped; portEXIT_CRITICAL(&mux); return false;
  }
  Record record{}; record.ms = ms; record.kind = kind; record.length = length;
  if (length) memcpy(record.payload, payload, length);
  if (xQueueSend(records, &record, 0) != pdTRUE) {
    portENTER_CRITICAL(&mux); ++shared.dropped; portEXIT_CRITICAL(&mux); return false;
  }
  const uint32_t queued = uxQueueMessagesWaiting(records);
  portENTER_CRITICAL(&mux); ++shared.accepted; if (queued > shared.queueHigh) shared.queueHigh = queued; portEXIT_CRITICAL(&mux);
  return true;
}
String statusJson() {
  Status state; portENTER_CRITICAL(&mux); state = shared; portEXIT_CRITICAL(&mux);
  char json[720];
  snprintf(json, sizeof(json),
    "{\"enabled\":true,\"accepting\":%s,\"state\":\"%s\",\"error\":\"%s\",\"boot_id\":%lu,"
    "\"used_frames\":%lu,\"capacity_frames\":%lu,\"valid_frames\":%lu,\"corrupt_frames\":%lu,\"records\":%lu,"
    "\"accepted\":%lu,\"dropped\":%lu,\"queued\":%lu,\"queue_high\":%lu,\"pending_records\":%lu,"
    "\"write_max_us\":%lu,\"errors\":%lu,\"usb_errors\":%lu,\"command_dropped\":%lu,\"erase_count\":%lu,\"flush_ms\":%lu}",
    accepting.load() ? "true" : "false", stateName(state.state), state.error, (unsigned long)bootId,
    (unsigned long)state.usedFrames, (unsigned long)kCapacity, (unsigned long)state.validFrames,
    (unsigned long)state.corruptFrames, (unsigned long)state.records, (unsigned long)state.accepted,
    (unsigned long)state.dropped, (unsigned long)(records ? uxQueueMessagesWaiting(records) : 0),
    (unsigned long)state.queueHigh, (unsigned long)state.pendingRecords, (unsigned long)state.writeMaxUs,
    (unsigned long)state.errors, (unsigned long)state.usbErrors, (unsigned long)state.commandDrops,
    (unsigned long)state.eraseCount, (unsigned long)kFlushMs);
  return String(json);
}
bool command(const char *line) {
  if (!line || strncmp(line, "DIAG", 4) != 0 || (line[4] && line[4] != ' ')) return false;
  if (!commands || usbActive.exchange(true)) {
    portENTER_CRITICAL(&mux); ++shared.commandDrops; portEXIT_CRITICAL(&mux); return false;
  }
  Command cmd;
  if (!strcmp(line, "DIAG STATUS")) cmd.op = Op::Status;
  else if (!strcmp(line, "DIAG ERASE CONFIRM")) cmd.op = Op::Erase;
  else if (!strcmp(line, "DIAG READ")) { cmd.op = Op::Read; cmd.all = true; }
  else if (!strncmp(line, "DIAG READ ", 10)) {
    const char *p = line + 10; char *end = nullptr;
    if (*p >= '0' && *p <= '9') {
      const unsigned long start = strtoul(p, &end, 10);
      if (start < kCapacity && (*end == 0 || *end == ' ')) {
        cmd.start = start; cmd.count = 1; cmd.op = Op::Read;
        if (*end) {
          p = end + 1;
          if (*p < '0' || *p > '9') cmd.op = Op::Invalid;
          else {
            const unsigned long count = strtoul(p, &end, 10);
            if (*end || !count || count > kCapacity) cmd.op = Op::Invalid;
            else cmd.count = count;
          }
        }
      }
    }
  }
  if (xQueueSend(commands, &cmd, 0) != pdTRUE) {
    usbActive.store(false); portENTER_CRITICAL(&mux); ++shared.commandDrops; portEXIT_CRITICAL(&mux);
    return false;
  }
  return true;
}
bool transferActive() { return usbActive.load(); }
bool healthy() {
  portENTER_CRITICAL(&mux); const bool ok = shared.state == State::Recording; portEXIT_CRITICAL(&mux);
  return ok;
}
const char *stateName() {
  portENTER_CRITICAL(&mux); const State state = shared.state; portEXIT_CRITICAL(&mux);
  return stateName(state);
}
uint32_t writeMaxUs() {
  portENTER_CRITICAL(&mux); const uint32_t value = shared.writeMaxUs; portEXIT_CRITICAL(&mux);
  return value;
}
uint32_t dropped() {
  portENTER_CRITICAL(&mux); const uint32_t lost = shared.dropped; portEXIT_CRITICAL(&mux);
  return lost;
}
} // namespace diagnostic_store

#else
namespace diagnostic_store {
void begin(uint32_t) {}
bool submit(uint16_t, const void *, size_t, uint32_t) { return false; }
String statusJson() { return String("{\"enabled\":false,\"state\":\"disabled\"}"); }
bool command(const char *) { return false; }
bool transferActive() { return false; }
bool healthy() { return false; }
const char *stateName() { return "disabled"; }
uint32_t dropped() { return 0; }
uint32_t writeMaxUs() { return 0; }
}
#endif
#endif
