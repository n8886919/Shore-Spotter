#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Independent continuous-trip profile. The short RF/SD comparison diagnostic
// keeps its original Plan; neither profile changes the normal 2 Hz build.
namespace trip_log {
constexpr size_t kRawChunk = 96;
constexpr uint32_t kSnapshotMs = 60000, kSdStatusMs = 300000;

class ContinuousPlan {
 public:
  bool observe(uint32_t, bool) { return false; }
  constexpr uint8_t phase() const { return 7; }
  constexpr bool rf() const { return true; }
  constexpr bool sd() const { return true; }
  void abort() {} // A recorder failure must not stop tracking or the SD logger.
};

// ASCII hex preserves every UART byte through the existing bounded Text queue,
// including invalid UTF-8, NUL and CRLF. The sequence identifies missing chunks.
// Returns the complete line length, excluding its trailing NUL, or zero without
// modifying out when any argument is invalid. Never silently truncates data.
inline size_t encodeRaw(char *out, size_t capacity, uint32_t seq, uint16_t kind,
                        const uint8_t *data, size_t length) {
  if (!out || length > kRawChunk || (length && !data) || (kind != 1 && kind != 6)) return 0;
  char prefix[64];
  const int prefixLength = snprintf(prefix, sizeof(prefix), "[GNSS_RAW seq=%lu kind=%u hex=",
      static_cast<unsigned long>(seq), static_cast<unsigned>(kind));
  if (prefixLength < 0 || static_cast<size_t>(prefixLength) >= sizeof(prefix)) return 0;
  const size_t total = static_cast<size_t>(prefixLength) + length * 2 + 2; // ]\n
  if (capacity <= total) return 0;
  memcpy(out, prefix, prefixLength);
  static constexpr char hex[] = "0123456789abcdef";
  size_t at = prefixLength;
  for (size_t i = 0; i < length; ++i) {
    out[at++] = hex[data[i] >> 4]; out[at++] = hex[data[i] & 15];
  }
  out[at++] = ']'; out[at++] = '\n'; out[at] = '\0';
  return at;
}
} // namespace trip_log
