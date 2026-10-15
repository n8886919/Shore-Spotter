#if defined(ARDUINO) && defined(ESP32)
#include "sdk_log.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace sdk_log {
namespace {
constexpr size_t kSlots = 8, kBytes = 384;
struct Entry { char text[kBytes]; size_t size; };
Entry queue[kSlots];
size_t head = 0, count = 0;
uint32_t dropped = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Called concurrently by SDK tasks. No USB, SD, heap allocation or LogTee here.
int capture(const char *format, va_list args) {
  Entry entry{};
  const int size = vsnprintf(entry.text, sizeof(entry.text), format, args);
  if (size < 0) return size;
  entry.size = size_t(size) < kBytes ? size_t(size) : kBytes - 1;
  if (size_t(size) >= kBytes) {
    constexpr char marker[] = " [SDK log truncated]\n";
    memcpy(entry.text + entry.size - (sizeof(marker) - 1), marker, sizeof(marker) - 1);
  }
  portENTER_CRITICAL(&mux);
  if (count < kSlots) {
    queue[(head + count) % kSlots] = entry;
    ++count;
  } else if (dropped != UINT32_MAX) {
    ++dropped;
  }
  portEXIT_CRITICAL(&mux);
  return size;
}
}

void begin() { esp_log_set_vprintf(capture); }

void drain(Print &output) {
  // Bound work even when producers keep logging. Only the main loop calls this.
  for (size_t i = 0; i < kSlots; ++i) {
    Entry entry{};
    portENTER_CRITICAL(&mux);
    const bool available = count != 0;
    if (available) {
      entry = queue[head];
      head = (head + 1) % kSlots;
      --count;
    }
    portEXIT_CRITICAL(&mux);
    if (!available) break;
    output.write(reinterpret_cast<const uint8_t *>(entry.text), entry.size);
  }
  portENTER_CRITICAL(&mux);
  const uint32_t lost = dropped;
  dropped = 0;
  portEXIT_CRITICAL(&mux);
  if (lost) {
    char notice[80];
    const int n = snprintf(notice, sizeof(notice), "\n[SDK] log queue overflow: dropped=%lu\n", (unsigned long)lost);
    output.write(reinterpret_cast<const uint8_t *>(notice), size_t(n));
  }
}
}
#endif
