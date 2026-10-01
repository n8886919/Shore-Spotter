#if defined(ARDUINO)
#include "sd_log.h"
#include "sd_retention.h"
#if defined(FIELD_DIAGNOSTIC)
#include "diagnostic_store.h"
#endif
#include "firmware_version.h"
#include <SD.h>
#include <SPI.h>
#include <sd_diskio.h>
#include <ff.h>
#include <esp_vfs_fat.h>
#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace sd_log {
namespace {
enum class State { Stopped, Mounting, Recording, Stopping, Error };
struct Status {
  State state = State::Stopped;
  char error[48]{}, path[80]{};
  uint32_t packets = 0, texts = 0, samples = 0;
  uint32_t packetDrops = 0, textDrops = 0, sampleDrops = 0;
  uint64_t bytes = 0, syncedBytes = 0, unconfirmedBytes = 0;
  uint32_t syncs = 0, verified = 0;
  uint32_t writeMaxUs = 0, syncMaxUs = 0, captureMaxUs = 0, stackMin = UINT32_MAX;
  uint32_t queueHigh = 0, errors = 0;
  uint64_t cardBytes = 0;
  uint64_t freeBytes = 0, deletedBytes = 0;
  uint32_t deletedFiles = 0, clientRecords = 0, clientDrops = 0;
} shared;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t packets = nullptr, texts = nullptr, samples = nullptr;
QueueHandle_t clientRecords = nullptr;
enum class UsbOp { Status, Start, Stop, List, Read, Format, Invalid };
struct UsbCommand {
  UsbOp op = UsbOp::Invalid; char path[80]{};
  uint16_t rxLength = 0; bool rxOverflow = false;
  uint8_t rxSnippetLength = 0, rxSnippet[32]{};
};
QueueHandle_t commands = nullptr;
TaskHandle_t task = nullptr;
std::atomic<bool> wanted{false}, accepting{false};
std::atomic<bool> usbActive{false};
// The main-loop router retains one complete command until either worker releases
// the stream. In particular, END can be visible before worker cleanup finishes.
std::atomic<bool> usbPending{false};
std::atomic<uint32_t> usbDeferred{0}, usbQueueErrors{0}, usbWriteErrors{0};
bool powerReady = false;
PowerSwitch powerSwitch = nullptr;
std::atomic<bool> cardPowered{false}, autoEnabled{false}, gpsAllowed{false};
ClientRecord pauseRecord;
bool pausePending = false;
uint32_t bootId = 0, nextSample = 0, sampleId = 0;
SPIClass sdSpi(HSPI); // LoRa uses the global SPI / FSPI controller.
constexpr uint32_t kSpiHz = 1000000;
char batch[kBatchBytes], encoded[kEncodeBytes]; // worker-only bounded storage
int fd = -1;
size_t used = 0;
uint32_t fileBytes = 0;
uint64_t fileSequence = 0;
uint32_t pendingSyncBytes = 0;
char lastWriteTail[256];
size_t lastWriteTailSize = 0;
axiom_log::LogHistory history;

const char *stateName(State state) {
  return state == State::Recording ? "recording" : state == State::Mounting ? "mounting" :
      state == State::Stopping ? "stopping" : state == State::Error ? "error" : "stopped";
}
void setState(State state) {
  portENTER_CRITICAL(&mux); shared.state = state; portEXIT_CRITICAL(&mux);
}
void discard() {
  Packet p; Text t; axiom_log::Sample s;
  ClientRecord c;
  uint32_t np = 0, nt = 0, ns = 0;
  while (xQueueReceive(packets, &p, 0) == pdTRUE) ++np;
  while (xQueueReceive(texts, &t, 0) == pdTRUE) ++nt;
  while (xQueueReceive(samples, &s, 0) == pdTRUE) ++ns;
  uint32_t nc = 0;
  while (clientRecords && xQueueReceive(clientRecords, &c, 0) == pdTRUE) ++nc;
  portENTER_CRITICAL(&mux);
  shared.packetDrops += np; shared.textDrops += nt; shared.sampleDrops += ns;
  shared.clientDrops += nc;
  portEXIT_CRITICAL(&mux);
}
bool cardPower(bool on) {
  if (!powerSwitch) { cardPowered.store(powerReady); return powerReady; }
  if (cardPowered.load() == on) return true;
  if (!on) {
    sdSpi.end();
    // Do not feed an unpowered SD through its host outputs.
    pinMode(36, INPUT); pinMode(35, INPUT); pinMode(47, INPUT);
  }
  if (!powerSwitch(on)) {
    // A readback failure can follow a successful enable. Still attempt OFF.
    if (on) cardPowered.store(!powerSwitch(false));
    return false;
  }
  cardPowered.store(on);
  if (on) vTaskDelay(pdMS_TO_TICKS(100));
  return true;
}
void fail(const char *error) {
  accepting.store(false); wanted.store(false);
  autoEnabled.store(false);
  // ERROR is exposed only after releasing the card. A partial final line may remain.
  if (fd >= 0) { close(fd); fd = -1; }
  SD.end(); discard();
  cardPower(false);
  portENTER_CRITICAL(&mux);
  shared.unconfirmedBytes += pendingSyncBytes + used;
  shared.state = State::Error; ++shared.errors;
  snprintf(shared.error, sizeof(shared.error), "%s", error);
  portEXIT_CRITICAL(&mux);
  pendingSyncBytes = 0; used = 0;
}
bool writeBatch() {
  if (!used) return true;
  const uint32_t started = micros();
  size_t written = 0;
  // Avoid the SPI multi-block path after repeated writes failed on this card.
  // Keep batching/queue limits, but issue at most one 512-byte sector per write.
  while (written < used) {
    const size_t take = used - written < 512 ? used - written : 512;
    const ssize_t n = ::write(fd, batch + written, take);
    if (n > 0) written += n;
    if (n != ssize_t(take)) break;
  }
  const uint32_t elapsed = micros() - started;
  portENTER_CRITICAL(&mux);
  if (elapsed > shared.writeMaxUs) shared.writeMaxUs = elapsed;
  shared.bytes += written;
  portEXIT_CRITICAL(&mux);
  pendingSyncBytes += written;
  if (written != used) {
    used -= written;
    fail("write_failed_or_card_full"); return false;
  }
  lastWriteTailSize = used < sizeof(lastWriteTail) ? used : sizeof(lastWriteTail);
  memcpy(lastWriteTail, batch + used - lastWriteTailSize, lastWriteTailSize);
  fileBytes += used; used = 0; return true;
}
bool append(const char *data, size_t size) {
  if (!size) { fail("encode_overflow"); return false; }
  while (size) {
    const size_t n = size < sizeof(batch) - used ? size : sizeof(batch) - used;
    memcpy(batch + used, data, n); used += n; data += n; size -= n;
    if (used == sizeof(batch) && !writeBatch()) return false;
  }
  return true;
}
bool syncFile() {
  if (!writeBatch()) return false;
  const uint32_t started = micros();
  const int result = fsync(fd);
  const uint32_t elapsed = micros() - started;
  portENTER_CRITICAL(&mux);
  if (elapsed > shared.syncMaxUs) shared.syncMaxUs = elapsed;
  portEXIT_CRITICAL(&mux);
  if (result != 0) { fail("sync_failed"); return false; }
  // Compare the last written bytes after sync, not just the API return value.
  // This is filesystem readback, not power-loss endurance or whole-card health.
  char tail[sizeof(lastWriteTail)];
  if (!lastWriteTailSize || pread(fd, tail, lastWriteTailSize, fileBytes - lastWriteTailSize) != ssize_t(lastWriteTailSize) ||
      memcmp(tail, lastWriteTail, lastWriteTailSize) || tail[lastWriteTailSize - 1] != '\n') {
    fail("readback_failed"); return false;
  }
  portENTER_CRITICAL(&mux);
  shared.syncedBytes += pendingSyncBytes; ++shared.syncs; ++shared.verified;
  portEXIT_CRITICAL(&mux);
  pendingSyncBytes = 0;
  return true;
}
struct Oldest {
  char path[96]{};
  uint64_t sequence = 0, bytes = 0;
  time_t modified = 0;
  bool legacy = false;
};
bool scanLogs(Oldest &oldest) {
  DIR *dir = opendir("/sd/logs");
  if (!dir) return false;
  bool ok = true;
  for (;;) {
    errno = 0;
    dirent *entry = readdir(dir);
    if (!entry) { if (errno) ok = false; break; }
    uint64_t seq; bool legacy;
    if (!sd_retention::name(entry->d_name, seq, legacy)) continue;
    // Reserve every matching sequence, even an interrupted/foreign file. Never truncate it.
    if (!legacy && seq > fileSequence) fileSequence = seq;
    char path[96]; snprintf(path, sizeof(path), "/sd/logs/%.70s", entry->d_name);
    if (!strcmp(path + 3, shared.path) && fd >= 0) continue;
    struct stat info{};
    if (stat(path, &info) || !S_ISREG(info.st_mode)) continue;
    const int input = open(path, O_RDONLY);
    if (input < 0) { ok = false; break; }
    char header[513]{};
    const ssize_t n = pread(input, header, sizeof(header) - 1, 0);
    close(input);
    if (n < 0) { ok = false; break; }
    char *lineEnd = strchr(header, '\n'); if (lineEnd) *lineEnd = 0;
    const bool ours = strstr(header, "\"kind\":\"session\"") &&
        (strstr(header, "\"app\":\"shore-spotter\"") ||
         (legacy && strstr(header, "\"schema_version\":1,") && strstr(header, "\"firmware\":") &&
          strstr(header, "\"frequency_mhz\":923.2,")));
    if (ours && (!oldest.path[0] || (legacy && !oldest.legacy) ||
        (legacy == oldest.legacy && (legacy ?
          (info.st_mtime < oldest.modified || (info.st_mtime == oldest.modified && strcmp(path, oldest.path) < 0)) :
          seq < oldest.sequence)))) {
      snprintf(oldest.path, sizeof(oldest.path), "%s", path);
      oldest.sequence = seq; oldest.legacy = legacy; oldest.modified = info.st_mtime; oldest.bytes = info.st_size;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  closedir(dir);
  return ok;
}
bool ensureSpace() {
  // FAT free-cluster scans can be long on a large card. Let core-0 idle run;
  // neither this scan nor deletion has any main-loop filesystem calls.
  struct Priority {
    Priority() { vTaskPrioritySet(nullptr, 0); }
    ~Priority() { vTaskPrioritySet(nullptr, 1); }
  } priority;
  for (;;) {
    uint64_t total = 0, free = 0;
    if (esp_vfs_fat_info("/sd", &total, &free) != ESP_OK) { fail("filesystem_space_failed"); return false; }
    const uint64_t target = sd_retention::reserve(total) + kRotateBytes;
    if (!total || target >= total || free > total) { fail("filesystem_capacity_invalid"); return false; }
    portENTER_CRITICAL(&mux); shared.freeBytes = free; portEXIT_CRITICAL(&mux);
    Oldest oldest;
    if (!scanLogs(oldest)) { fail("retention_scan_failed"); return false; }
    if (free >= target) return true;
    if (!oldest.path[0]) { fail("full_no_deletable_logs"); return false; }
    if (unlink(oldest.path)) { fail("retention_delete_failed"); return false; }
    portENTER_CRITICAL(&mux);
    ++shared.deletedFiles; shared.deletedBytes += oldest.bytes;
    portEXIT_CRITICAL(&mux);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
bool openPart() {
  if (!ensureSpace()) return false;
  char path[80];
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    if (fileSequence == UINT64_MAX) { fail("log_sequence_exhausted"); return false; }
    snprintf(path, sizeof(path), "/sd/logs/%016llx-0000.ndjson", (unsigned long long)++fileSequence);
    fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd >= 0) break;
    if (errno != EEXIST) break;
  }
  if (fd < 0) { fail("create_file_failed"); return false; }
  fileBytes = 0; used = 0; pendingSyncBytes = 0; lastWriteTailSize = 0; history.valid = false;
  portENTER_CRITICAL(&mux);
  snprintf(shared.path, sizeof(shared.path), "%s", path + 3);
  portEXIT_CRITICAL(&mux);
  const int n = snprintf(encoded, sizeof(encoded),
      "{\"kind\":\"session\",\"app\":\"shore-spotter\",\"role\":\"%s\",\"schema_version\":1,\"boot_id\":%lu,\"firmware\":\"%s\","
      "\"build\":\"%s %s\",\"protocol\":%u,\"sf\":10,\"bw_khz\":125,\"cr\":5,"
      "\"frequency_mhz\":923.2,\"clock\":\"boot_ms; _time null until UTC available\","
      "\"packet_queue\":%u,\"sync_ms\":%lu,\"overwrite\":true,\"gps_gate\":%s,\"profile\":\"%s\"}\n",
#if defined(ROLE_CLIENT)
      "client",
#else
      "station",
#endif
      (unsigned long)bootId, SHORE_SPOTTER_VERSION, __DATE__, __TIME__, PROTO_VERSION,
      unsigned(kPacketQueue), (unsigned long)kSyncMs,
#if defined(CLIENT_TRIP_LOG)
      "false", "client-trip-1hz");
#else
      powerSwitch ? "true" : "false", "standard");
#endif
  return n > 0 && append(encoded, n) && syncFile();
}
bool mountCard() {
  if (!powerReady || !cardPower(true)) return false;
  pinMode(34, OUTPUT); digitalWrite(34, HIGH); // deselect IMU on the same SD SPI bus
  pinMode(47, OUTPUT); digitalWrite(47, HIGH);
  if (!sdSpi.begin(36, 37, 35, 47) || !SD.begin(47, sdSpi, kSpiHz, "/sd", 2, false)) {
    return false;
  }
  const uint64_t capacity = SD.cardSize();
  portENTER_CRITICAL(&mux); shared.cardBytes = capacity; portEXIT_CRITICAL(&mux);
  return true;
}
bool mount() {
  setState(State::Mounting);
  portENTER_CRITICAL(&mux); shared.error[0] = 0; portEXIT_CRITICAL(&mux);
  if (!mountCard()) { fail("mount_failed_no_format"); return false; }
  if (!SD.exists("/logs") && !SD.mkdir("/logs")) { fail("mkdir_failed"); return false; }
  if (!openPart()) return false;
  accepting.store(wanted.load()); setState(State::Recording); return true;
}
bool statusRecord() {
  const String json = statusJson();
  return append("{\"kind\":\"sd_status\",\"status\":", 29) &&
      append(json.c_str(), json.length()) && append("}\n", 2);
}
bool usbWrite(const void *data, size_t size) {
  const auto *p = static_cast<const uint8_t *>(data);
  const uint32_t started = millis();
  const bool recording = fd >= 0;
  uint32_t lastProgress = started;
  while (size) {
    // A stalled or trickling host must not hold up the same worker that drains
    // raw GNSS to SD. Offline LIST/READ retain their longer progress timeout.
    if ((recording && uint32_t(millis() - started) >= 50) ||
        (!recording && uint32_t(millis() - lastProgress) >= 3000)) {
      ++usbWriteErrors; return false;
    }
    const size_t chunk = size < 256 ? size : 256;
    const size_t sent = Serial.write(p, chunk);
    if (sent) { p += sent; size -= sent; lastProgress = millis(); }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return true;
}
bool usbLine(const char *s) { return usbWrite(s, strlen(s)) && usbWrite("\n", 1); }
bool validPath(const char *path) {
  if (strncmp(path, "/logs/", 6)) return false;
  const size_t n = strlen(path);
  if (n < 20 || n >= 80 || strcmp(path + n - 7, ".ndjson")) return false;
  for (size_t i = 6; i < n - 7; ++i)
    if (!((path[i] >= '0' && path[i] <= '9') || (path[i] >= 'a' && path[i] <= 'f') || path[i] == '-')) return false;
  return true;
}
uint32_t crc32(uint32_t crc, const uint8_t *data, size_t n) {
  while (n--) { crc ^= *data++; for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1))); }
  return crc;
}
void formatCard() {
  // Never called on mount failure: only the exact, explicit erase command.
  SD.end();
  if (!powerReady || !cardPower(true)) { usbLine("@SD ERROR sd_power_failed"); return; }
  pinMode(34, OUTPUT); digitalWrite(34, HIGH);
  pinMode(47, OUTPUT); digitalWrite(47, HIGH);
  if (!sdSpi.begin(36, 37, 35, 47)) { usbLine("@SD ERROR spi_failed"); return; }
  const uint8_t drive = sdcard_init(47, &sdSpi, kSpiHz);
  if (drive == 0xff) { usbLine("@SD ERROR card_init_failed"); return; }
  // Initialize the card even when the old filesystem is unsupported.
  sdcard_mount(drive, "/sd-format", 1, false);
  char volume[] = {char('0' + drive), ':', 0};
  f_mount(nullptr, volume, 0);
  const uint32_t sectors = sdcard_num_sectors(drive);
  uint8_t sector[512]{};
  const bool readable = sectors && sectors != UINT32_MAX && sd_read_raw(drive, sector, 0);
  char line[160];
  snprintf(line, sizeof(line), "@SD FORMAT BEGIN bytes=%llu old_partition_type=%02x",
           (unsigned long long)sectors * 512, unsigned(sector[450]));
  usbLine(line);
  FRESULT result = FR_DISK_ERR;
  if (readable) {
    // Replace the partition table with one partition, then force FAT32.
    // f_fdisk / create_partition clear FF_MAX_SS bytes even on a 512-byte SD.
    // Allocate that full size, but let mkfs clear FAT data one SD sector at a time.
    BYTE work[FF_MAX_SS]; const LBA_t partitions[] = {100, 0, 0, 0};
    result = f_fdisk(drive, partitions, work);
    snprintf(line, sizeof(line), "@SD FORMAT PARTITION result=%u", unsigned(result));
    usbLine(line);
    if (result == FR_OK) {
      const MKFS_PARM options = {FM_FAT32, 0, 0, 0, 0};
      result = f_mkfs(volume, &options, work, 512);
      snprintf(line, sizeof(line), "@SD FORMAT FILESYSTEM result=%u", unsigned(result));
      usbLine(line);
    }
  }
  sdcard_unmount(drive); sdcard_uninit(drive);
  if (result != FR_OK) {
    snprintf(line, sizeof(line), "@SD ERROR fat32_format_failed_%u", unsigned(result));
    usbLine(line); fail("fat32_format_failed"); return;
  }
  if (!mountCard()) { SD.end(); usbLine("@SD ERROR formatted_but_mount_failed"); fail("format_verify_failed"); return; }
  SD.end();
  portENTER_CRITICAL(&mux); shared.error[0] = 0; shared.path[0] = 0; portEXIT_CRITICAL(&mux);
  setState(State::Stopped);
  usbLine("@SD FORMAT OK FAT32");
}
void serveUsb(const UsbCommand &command) {
  usbActive.store(true);
  // Let a preceding main-loop console write finish before the framed response.
  vTaskDelay(pdMS_TO_TICKS(20));
  if (command.op == UsbOp::Status || command.op == UsbOp::Stop || command.op == UsbOp::Start) {
    if (command.op == UsbOp::Start && !start()) usbLine("@SD ERROR cannot_start");
    const String response = String("@SD STATUS ") + statusJson(); usbLine(response.c_str());
  } else if (command.op == UsbOp::Format) {
    // FAT initialization can write for seconds; share idle priority so the
    // core-0 idle task / watchdog remains serviced during synchronous mkfs.
    vTaskPrioritySet(nullptr, 0);
    formatCard();
    vTaskPrioritySet(nullptr, 1);
  } else if (command.op == UsbOp::Invalid) {
    char snippet[65], response[200];
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < command.rxSnippetLength; ++i) {
      snippet[i * 2] = hex[command.rxSnippet[i] >> 4];
      snippet[i * 2 + 1] = hex[command.rxSnippet[i] & 15];
    }
    snippet[command.rxSnippetLength * 2] = 0;
    snprintf(response, sizeof(response), "@SD ERROR expected_STATUS_START_STOP_LIST_or_READ_path rx_len=%u overflow=%u rx_hex=%s",
        unsigned(command.rxLength), unsigned(command.rxOverflow), snippet);
    usbLine(response);
  } else if (!mountCard()) {
    SD.end(); usbLine("@SD ERROR mount_failed_no_format");
  } else if (command.op == UsbOp::List) {
    DIR *dir = opendir("/sd/logs");
    // A fresh Client card has no logs directory until its first good GPS fix.
    if (!dir && errno == ENOENT) {
      usbLine("@SD LIST BEGIN"); usbLine("@SD LIST END");
    } else if (!dir) usbLine("@SD ERROR logs_directory_unavailable");
    else {
      usbLine("@SD LIST BEGIN");
      while (dirent *entry = readdir(dir)) {
        char full[96], path[80], line[128];
        if (strlen(entry->d_name) > 70) continue;
        snprintf(path, sizeof(path), "/logs/%.70s", entry->d_name);
        if (!validPath(path)) continue;
        snprintf(full, sizeof(full), "/sd%s", path);
        struct stat info{};
        if (stat(full, &info) || !S_ISREG(info.st_mode)) continue;
        snprintf(line, sizeof(line), "@SD FILE %lld %s", (long long)info.st_size, path);
        if (!usbLine(line)) break;
      }
      closedir(dir); usbLine("@SD LIST END");
    }
    SD.end();
  } else {
    char path[80], full[96], line[160];
    if (!strcmp(command.path, "LAST")) {
      portENTER_CRITICAL(&mux); snprintf(path, sizeof(path), "%s", shared.path); portEXIT_CRITICAL(&mux);
    } else snprintf(path, sizeof(path), "%s", command.path);
    snprintf(full, sizeof(full), "/sd%s", path);
    const int input = validPath(path) ? open(full, O_RDONLY) : -1;
    struct stat info{};
    if (input < 0 || fstat(input, &info) || !S_ISREG(info.st_mode)) usbLine("@SD ERROR file_unavailable");
    else {
      snprintf(line, sizeof(line), "@SD BEGIN %lld %s", (long long)info.st_size, path);
      bool ok = usbLine(line); uint32_t crc = 0xffffffffU;
      off_t remaining = info.st_size;
      while (ok && remaining > 0) {
        const size_t take = remaining < 1024 ? size_t(remaining) : 1024;
        const ssize_t n = read(input, encoded, take);
        if (n <= 0) { ok = false; break; }
        crc = crc32(crc, reinterpret_cast<const uint8_t *>(encoded), n);
        ok = usbWrite(encoded, n); remaining -= n;
      }
      snprintf(line, sizeof(line), "@SD END %08lx", (unsigned long)(crc ^ 0xffffffffU));
      if (ok) usbLine(line); else usbLine("@SD ERROR read_or_usb_failed");
    }
    if (input >= 0) close(input);
    SD.end();
  }
  if (!wanted.load() && fd < 0 && !cardPower(false)) fail("sd_power_off_failed");
  usbActive.store(false);
}
void worker(void *) {
  uint32_t lastWrite = millis(), lastSync = millis();
  UsbCommand command; bool pendingCommand = false;
  for (;;) {
    if (!pendingCommand && xQueueReceive(commands, &command, 0) == pdTRUE) {
      pendingCommand = true;
      if (command.op == UsbOp::Stop || command.op == UsbOp::List || command.op == UsbOp::Read || command.op == UsbOp::Format) stop();
    }
    if (fd < 0 && wanted.load()) {
      if (mount()) { lastWrite = lastSync = millis(); }
    }
    if (fd < 0 && !wanted.load()) {
      portENTER_CRITICAL(&mux); const bool cancelled = shared.state == State::Stopping; portEXIT_CRITICAL(&mux);
      if (cancelled) {
        discard();
        portENTER_CRITICAL(&mux); pausePending = false; portEXIT_CRITICAL(&mux);
        if (cardPower(false)) setState(State::Stopped); else fail("sd_power_off_failed");
      }
    }
    if (pendingCommand && (fd < 0 || command.op == UsbOp::Status || command.op == UsbOp::Start || command.op == UsbOp::Invalid)) {
      serveUsb(command); pendingCommand = false;
    }
    if (fd >= 0) {
      if (!wanted.load()) setState(State::Stopping);
      Packet p; Text t; axiom_log::Sample s;
      ClientRecord c;
      for (unsigned i = 0; i < 8 && fd >= 0 && clientRecords && xQueueReceive(clientRecords, &c, 0) == pdTRUE; ++i) {
        if (append(encoded, encodeClient(encoded, sizeof(encoded), bootId, c))) {
          portENTER_CRITICAL(&mux); ++shared.clientRecords; portEXIT_CRITICAL(&mux);
        }
      }
      // Bound each queue drain, so neither text storms nor new packets starve
      // snapshots, sync, stop or the idle task / Wi-Fi on core 0.
      for (unsigned i = 0; i < 8 && fd >= 0 && xQueueReceive(packets, &p, 0) == pdTRUE; ++i) {
        if (append(encoded, encodePacket(encoded, sizeof(encoded), bootId, p))) {
          portENTER_CRITICAL(&mux); ++shared.packets; portEXIT_CRITICAL(&mux);
        }
      }
      for (unsigned i = 0; i < 4 && fd >= 0 && xQueueReceive(texts, &t, 0) == pdTRUE; ++i) {
        if (append(encoded, encodeText(encoded, sizeof(encoded), bootId, t))) {
          portENTER_CRITICAL(&mux); ++shared.texts; portEXIT_CRITICAL(&mux);
        }
      }
      if (fd >= 0 && xQueueReceive(samples, &s, 0) == pdTRUE) {
        timeval tv{}; gettimeofday(&tv, nullptr);
        const int64_t utc = tv.tv_sec >= 1735689600 ?
            int64_t(tv.tv_sec) * 1000 + tv.tv_usec / 1000 - uint32_t(millis() - s.ms) : -1;
        uint32_t dropped, capture;
        portENTER_CRITICAL(&mux); dropped = shared.sampleDrops; capture = shared.captureMaxUs; portEXIT_CRITICAL(&mux);
        const size_t n = axiom_log::encodeSample(encoded, sizeof(encoded), s, utc, dropped, capture, &history);
        if (append(encoded, n)) { portENTER_CRITICAL(&mux); ++shared.samples; portEXIT_CRITICAL(&mux); }
      }
      const uint32_t now = millis();
      if (fd >= 0 && now - lastWrite >= 1000) { writeBatch(); lastWrite = now; }
      if (fd >= 0 && now - lastSync >= kSyncMs) {
        if (statusRecord() && syncFile() && wanted.load() && fileBytes >= kRotateBytes) {
          close(fd); fd = -1; openPart();
        }
        lastSync = now;
      }
      if (fd >= 0 && !wanted.load() && !uxQueueMessagesWaiting(packets) &&
          !uxQueueMessagesWaiting(texts) && !uxQueueMessagesWaiting(samples) &&
          (!clientRecords || !uxQueueMessagesWaiting(clientRecords))) {
        bool pause;
        portENTER_CRITICAL(&mux); pause = pausePending; c = pauseRecord; pausePending = false; portEXIT_CRITICAL(&mux);
        if (pause) {
          if (!append(encoded, encodeClient(encoded, sizeof(encoded), bootId, c))) continue;
          portENTER_CRITICAL(&mux); ++shared.clientRecords; portEXIT_CRITICAL(&mux);
        }
        if (statusRecord() && syncFile()) {
          close(fd); fd = -1; SD.end();
          if (cardPower(false)) setState(State::Stopped); else fail("sd_power_off_failed");
        }
      }
    }
    const uint32_t stack = uxTaskGetStackHighWaterMark(nullptr);
    portENTER_CRITICAL(&mux); if (stack < shared.stackMin) shared.stackMin = stack; portEXIT_CRITICAL(&mux);
    vTaskDelay(pdMS_TO_TICKS(powerSwitch && fd < 0 && !wanted.load() ? 200 : 20));
  }
}
}
void begin(uint32_t boot, bool powered, PowerSwitch power) {
  if (task) return;
  bootId = boot; powerReady = powered; powerSwitch = power;
  cardPowered.store(!power && powered); autoEnabled.store(power && powered);
  packets = xQueueCreate(kPacketQueue, sizeof(Packet));
  texts = xQueueCreate(kTextQueue, sizeof(Text));
  samples = xQueueCreate(kSampleQueue, sizeof(axiom_log::Sample));
  if (power) clientRecords = xQueueCreate(32, sizeof(ClientRecord));
  commands = xQueueCreate(2, sizeof(UsbCommand));
  if (!packets || !texts || !samples || !commands || (power && !clientRecords)) {
    if (packets) vQueueDelete(packets);
    if (texts) vQueueDelete(texts);
    if (samples) vQueueDelete(samples);
    if (commands) vQueueDelete(commands);
    if (clientRecords) vQueueDelete(clientRecords);
    clientRecords = nullptr;
    packets = texts = samples = commands = nullptr;
    shared.state = State::Error; snprintf(shared.error, sizeof(shared.error), "queue_memory_failed"); return;
  }
  const bool recordNow = powered && !power;
  wanted.store(recordNow); accepting.store(recordNow);
  setState(!powered ? State::Error : recordNow ? State::Mounting : State::Stopped);
  if (!powered) snprintf(shared.error, sizeof(shared.error), "sd_power_failed");
  if (xTaskCreatePinnedToCore(worker, "sd-log", 16384, nullptr, 1, &task, 0) != pdPASS) {
    accepting.store(false); wanted.store(false); task = nullptr;
    vQueueDelete(packets); vQueueDelete(texts); vQueueDelete(samples); vQueueDelete(commands); packets = texts = samples = commands = nullptr;
    if (clientRecords) vQueueDelete(clientRecords);
    clientRecords = nullptr;
    shared.state = State::Error; snprintf(shared.error, sizeof(shared.error), "worker_memory_failed");
  }
}
bool start() {
  if (!task || !powerReady) return false;
  portENTER_CRITICAL(&mux); const State state = shared.state; portEXIT_CRITICAL(&mux);
  if (state != State::Stopped && state != State::Error) return state == State::Recording && wanted.load();
  if (powerSwitch) {
    autoEnabled.store(true);
    if (!gpsAllowed.load()) { setState(State::Stopped); return true; }
  }
  setState(State::Mounting); wanted.store(true); accepting.store(true); return true;
}
void stop() {
  autoEnabled.store(false);
  accepting.store(false); wanted.store(false);
  portENTER_CRITICAL(&mux);
  if (shared.state == State::Recording || shared.state == State::Mounting) shared.state = State::Stopping;
  portEXIT_CRITICAL(&mux);
}
void clientEvent(const ClientRecord &r) {
  if (!accepting.load() || !clientRecords) return;
  const uint32_t started = micros();
  const bool ok = xQueueSend(clientRecords, &r, 0) == pdTRUE;
  const uint32_t depth = uxQueueMessagesWaiting(clientRecords), elapsed = micros() - started;
  portENTER_CRITICAL(&mux);
  if (!ok) ++shared.clientDrops;
  if (depth > shared.queueHigh) shared.queueHigh = depth;
  if (elapsed > shared.captureMaxUs) shared.captureMaxUs = elapsed;
  portEXIT_CRITICAL(&mux);
}
void clientGps(bool allowed, const ClientRecord &record) {
  gpsAllowed.store(allowed);
  if (!powerSwitch || !autoEnabled.load()) return;
  if (allowed && !wanted.load()) {
    // Finish any previous drain before resuming; the caller continues observing GPS.
    if (!stopped()) return;
    if (!start()) return;
    ClientRecord r = record; r.kind = ClientKind::Resume; clientEvent(r);
  } else if (!allowed && wanted.load()) {
    ClientRecord r = record; r.kind = ClientKind::Pause;
    portENTER_CRITICAL(&mux); pauseRecord = r; pausePending = true; portEXIT_CRITICAL(&mux);
    accepting.store(false); wanted.store(false); setState(State::Stopping);
  }
}
bool stopped() {
  portENTER_CRITICAL(&mux); const State state = shared.state; portEXIT_CRITICAL(&mux);
  return state == State::Stopped || state == State::Error;
}
bool captureDue(uint32_t now, bool busy) {
  if (!accepting.load() || int32_t(now - nextSample) < 0) return false;
  nextSample = now + 1000;
  if (busy || ESP.getFreeHeap() < 32768) {
    portENTER_CRITICAL(&mux); ++shared.sampleDrops; portEXIT_CRITICAL(&mux); return false;
  }
  return true;
}
void submit(const axiom_log::Sample &sample, uint32_t started) {
  if (!accepting.load() || !samples) return;
  axiom_log::Sample s = sample; s.id = ++sampleId; s.eventCount = 0;
  const bool ok = xQueueSend(samples, &s, 0) == pdTRUE;
  const uint32_t elapsed = micros() - started;
  portENTER_CRITICAL(&mux);
  if (!ok) ++shared.sampleDrops;
  if (elapsed > shared.captureMaxUs) shared.captureMaxUs = elapsed;
  portEXIT_CRITICAL(&mux);
}
void packet(const packet_diagnostics::Event &event, const uint8_t *raw, size_t length) {
  if (!accepting.load() || !packets) return;
  Packet p; p.event = event;
  p.rawLength = raw ? (length < sizeof(p.raw) ? length : sizeof(p.raw)) : 0;
  if (p.rawLength) memcpy(p.raw, raw, p.rawLength);
  const bool ok = xQueueSend(packets, &p, 0) == pdTRUE;
  const uint32_t depth = uxQueueMessagesWaiting(packets);
  portENTER_CRITICAL(&mux);
  if (!ok) ++shared.packetDrops;
  if (depth > shared.queueHigh) shared.queueHigh = depth;
  portEXIT_CRITICAL(&mux);
}
void text(const uint8_t *bytes, size_t length, uint32_t ms) {
  if (!accepting.load() || !texts || !length) return;
  Text t; t.ms = ms; t.length = length < sizeof(t.bytes) ? length : sizeof(t.bytes);
  memcpy(t.bytes, bytes, t.length);
  if (xQueueSend(texts, &t, 0) != pdTRUE) {
    portENTER_CRITICAL(&mux); ++shared.textDrops; portEXIT_CRITICAL(&mux);
  }
}
String statusJson() {
  Status s; portENTER_CRITICAL(&mux); s = shared; portEXIT_CRITICAL(&mux);
  const char *state = stateName(s.state);
  char out[1600];
  snprintf(out, sizeof(out),
      "{\"state\":\"%s\",\"enabled\":%s,\"boot_id\":%lu,\"clock_ms\":%lu,\"path\":\"%s\",\"error\":\"%s\","
      "\"card_bytes\":%llu,\"packets\":%lu,\"texts\":%lu,\"samples\":%lu,"
      "\"packet_dropped\":%lu,\"text_dropped\":%lu,\"sample_dropped\":%lu,\"bytes\":%llu,\"synced_bytes\":%llu,"
      "\"unconfirmed_bytes\":%llu,\"syncs\":%lu,\"readback_checks\":%lu,\"errors\":%lu,\"write_max_us\":%lu,\"sync_max_us\":%lu,"
      "\"capture_max_us\":%lu,\"worker_stack_free_min\":%lu,\"packet_queue_high\":%lu,\"packet_queue_depth\":%u,"
      "\"overwrite\":true,\"free_bytes_at_open\":%llu,\"deleted_files\":%lu,\"deleted_bytes\":%llu,"
      "\"auto_enabled\":%s,\"gps_allowed\":%s,\"card_powered\":%s,\"client_records\":%lu,\"client_dropped\":%lu,"
      "\"usb_pending\":%s,\"usb_deferred\":%lu,\"usb_queue_errors\":%lu,\"usb_write_errors\":%lu}",
      state, wanted.load() ? "true" : "false", (unsigned long)bootId, (unsigned long)millis(), s.path, s.error,
      (unsigned long long)s.cardBytes, (unsigned long)s.packets, (unsigned long)s.texts, (unsigned long)s.samples,
      (unsigned long)s.packetDrops, (unsigned long)s.textDrops, (unsigned long)s.sampleDrops,
      (unsigned long long)s.bytes, (unsigned long long)s.syncedBytes, (unsigned long long)s.unconfirmedBytes, (unsigned long)s.syncs, (unsigned long)s.verified,
      (unsigned long)s.errors, (unsigned long)s.writeMaxUs, (unsigned long)s.syncMaxUs,
      (unsigned long)s.captureMaxUs, (unsigned long)(s.stackMin == UINT32_MAX ? 0 : s.stackMin),
      (unsigned long)s.queueHigh, unsigned(packets ? uxQueueMessagesWaiting(packets) : 0),
      (unsigned long long)s.freeBytes, (unsigned long)s.deletedFiles, (unsigned long long)s.deletedBytes,
      autoEnabled.load() ? "true" : "false", gpsAllowed.load() ? "true" : "false", cardPowered.load() ? "true" : "false",
      (unsigned long)s.clientRecords, (unsigned long)s.clientDrops,
      usbPending.load() ? "true" : "false", (unsigned long)usbDeferred.load(), (unsigned long)usbQueueErrors.load(),
      (unsigned long)usbWriteErrors.load());
  return String(out);
}
const char *stateName() {
  portENTER_CRITICAL(&mux); const State state = shared.state; portEXIT_CRITICAL(&mux);
  return stateName(state);
}
bool usbTransferActive() { return usbActive.load(); }
void serviceUsb() {
  static char line[96]; static size_t length = 0; static bool overflow = false;
  static uint16_t rxLength = 0;
  static uint8_t rxSnippet[32], rxSnippetLength = 0;
  static bool ready = false, deferred = false, queueError = false;
  static uint32_t lastByte = 0;
  const auto dispatch = [&]() {
    bool busy = usbActive.load() || (commands && uxQueueMessagesWaiting(commands));
#if defined(FIELD_DIAGNOSTIC)
    busy = busy || diagnostic_store::transferActive();
#endif
    if (busy) {
      if (!deferred) { ++usbDeferred; deferred = true; }
      return;
    }
    bool accepted = false;
#if defined(FIELD_DIAGNOSTIC)
    if (!overflow && !memchr(line, 0, length) && !strncmp(line, "DIAG ", 5)) {
      accepted = diagnostic_store::command(line);
    } else
#endif
    {
      UsbCommand command;
      command.rxLength = rxLength; command.rxOverflow = overflow;
      command.rxSnippetLength = rxSnippetLength;
      memcpy(command.rxSnippet, rxSnippet, rxSnippetLength);
      if (!overflow && !memchr(line, 0, length)) {
        if (!strcmp(line, "SD STATUS")) command.op = UsbOp::Status;
        else if (!strcmp(line, "SD START")) command.op = UsbOp::Start;
        else if (!strcmp(line, "SD STOP")) command.op = UsbOp::Stop;
        else if (!strcmp(line, "SD LIST")) command.op = UsbOp::List;
        else if (!strcmp(line, "SD FORMAT FAT32 ERASE")) command.op = UsbOp::Format;
        else if (!strncmp(line, "SD READ ", 8) && (!strcmp(line + 8, "LAST") || validPath(line + 8))) {
          command.op = UsbOp::Read; snprintf(command.path, sizeof(command.path), "%.79s", line + 8);
        }
      }
      // Reserve before enqueue: the worker can dequeue while still draining SD.
      if (commands && !usbActive.exchange(true)) {
        accepted = xQueueSend(commands, &command, 0) == pdTRUE;
        if (!accepted) usbActive.store(false);
      }
    }
    if (!accepted) {
      // Retain the command on queue failure too; never silently discard it.
      if (!queueError) { ++usbQueueErrors; queueError = true; }
      return;
    }
    ready = deferred = queueError = false; length = 0; overflow = false;
    rxLength = rxSnippetLength = 0;
    usbPending.store(false);
  };
  // No reads while a complete request is waiting; leave subsequent bytes in the
  // bounded USB receive buffer. Hosts use one request / complete response at a time.
  if (ready) { dispatch(); return; }
  if (rxLength && millis() - lastByte > 1000) { length = 0; overflow = false; rxLength = rxSnippetLength = 0; }
  for (unsigned i = 0; i < 64 && Serial.available(); ++i) {
    const int c = Serial.read(); lastByte = millis();
    if (c != '\n') {
      if (rxLength < UINT16_MAX) ++rxLength; else overflow = true;
      if (rxSnippetLength < sizeof(rxSnippet)) rxSnippet[rxSnippetLength++] = uint8_t(c);
    }
    if (c == '\r') continue;
    if (c != '\n') {
      if (length < sizeof(line) - 1) line[length++] = char(c); else overflow = true;
      continue;
    }
    line[length] = 0; ready = true; usbPending.store(true);
    dispatch(); return;
  }
}
}
#endif
