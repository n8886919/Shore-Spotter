#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// On-flash v1 format. All integers are little endian; CRCs use IEEE CRC32.
// The ownership header occupies frame 0. Data frames contain complete records,
// so a damaged frame never prevents decoding a later frame or a later boot.
namespace diagnostic_store {
namespace codec {
constexpr size_t kFrameBytes = 512, kHeaderBytes = 28, kCrcOffset = 508;
constexpr size_t kPayloadBytes = kCrcOffset - kHeaderBytes;
constexpr size_t kEnvelopeBytes = 8, kMaxRecordBytes = kPayloadBytes - kEnvelopeBytes;
constexpr uint32_t kPartitionAddress = 0x670000, kPartitionBytes = 0x180000;
constexpr char kHeaderMagic[9] = "SSDHDR01", kFrameMagic[9] = "SSDFRM01";
inline uint16_t get16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t get32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void put16(uint8_t *p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
inline void put32(uint8_t *p, uint32_t n) { for (unsigned i = 0; i < 4; ++i) p[i] = n >> (8 * i); }
inline uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0U);
  }
  return crc ^ 0xffffffffU;
}
inline bool erased(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; ++i) if (p[i] != 0xff) return false;
  return true;
}
inline void seal(uint8_t *frame) { put32(frame + kCrcOffset, crc32(frame, kCrcOffset)); }
inline bool validCrc(const uint8_t *frame) {
  return get32(frame + kCrcOffset) == crc32(frame, kCrcOffset);
}
inline void makeHeader(uint8_t *frame) {
  memset(frame, 0xff, kFrameBytes); memcpy(frame, kHeaderMagic, 8);
  put32(frame + 8, kPartitionAddress); put32(frame + 12, kPartitionBytes);
  put32(frame + 16, kFrameBytes); put32(frame + 20, 1); put32(frame + 24, 0); seal(frame);
}
inline bool validHeader(const uint8_t *frame) {
  return memcmp(frame, kHeaderMagic, 8) == 0 && validCrc(frame) &&
    get32(frame + 8) == kPartitionAddress && get32(frame + 12) == kPartitionBytes &&
    get32(frame + 16) == kFrameBytes && get32(frame + 20) == 1;
}
inline void beginFrame(uint8_t *frame, uint32_t boot, uint32_t seq, uint32_t ms) {
  memset(frame, 0xff, kFrameBytes); memcpy(frame, kFrameMagic, 8);
  put16(frame + 8, 0); put16(frame + 10, 0); put32(frame + 12, boot);
  put32(frame + 16, seq); put32(frame + 20, ms); put32(frame + 24, 0);
}
inline bool append(uint8_t *frame, uint16_t kind, const void *payload, size_t length, uint32_t ms) {
  const size_t used = get16(frame + 8);
  if (length > kMaxRecordBytes || (length && !payload) || used + kEnvelopeBytes + length > kPayloadBytes) return false;
  uint8_t *out = frame + kHeaderBytes + used;
  put16(out, kind); put16(out + 2, length); put32(out + 4, ms);
  if (length) memcpy(out + kEnvelopeBytes, payload, length);
  put16(frame + 8, used + kEnvelopeBytes + length); put16(frame + 10, get16(frame + 10) + 1);
  return true;
}
inline bool validFrame(const uint8_t *frame) {
  if (memcmp(frame, kFrameMagic, 8) != 0 || !validCrc(frame)) return false;
  const size_t used = get16(frame + 8);
  if (!used || used > kPayloadBytes) return false;
  size_t offset = 0, records = 0;
  while (offset < used) {
    if (used - offset < kEnvelopeBytes) return false;
    const size_t length = get16(frame + kHeaderBytes + offset + 2);
    if (length > kMaxRecordBytes || kEnvelopeBytes + length > used - offset) return false;
    offset += kEnvelopeBytes + length; ++records;
  }
  return records == get16(frame + 10);
}
} // namespace codec
} // namespace diagnostic_store
