#pragma once
// Shared LoRa v4 wire contract. Logical structs are NOT the wire format.
// Only the explicit little-endian codecs below define transmitted bytes.
// Arduino-free; see docs/interface.md and native packet tests.
#include <math.h>
#include <stddef.h>
#include <stdint.h>

constexpr uint8_t PROTO_MAGIC = 0x53;
constexpr uint8_t PROTO_VERSION = 4;
constexpr uint16_t ID_BROADCAST = 0xFFFF;
// Legacy client-binding reservation only, never sent as a Server ID.
constexpr uint16_t SERVER_ID = 0x0010;
enum MsgType : uint8_t {
  MSG_DATA = 1, MSG_ACK = 2, MSG_TELEMETRY = 4, MSG_DIAGNOSTIC = 5
};
constexpr size_t PACKET_HEADER_LEN = 6;
constexpr size_t DATA_PACKET_LEN = 17;
constexpr size_t TELEMETRY_PACKET_LEN = 11;
constexpr size_t ACK_PACKET_LEN = 11;
constexpr size_t DIAGNOSTIC_PACKET_LEN = 17;
constexpr size_t MAX_PACKET_LEN = DATA_PACKET_LEN;

struct PacketHeader {
  uint16_t clientId;
  uint16_t seq;  // DATA-only sequence for DATA; per-type semantics otherwise.
  uint8_t msgType;
};
struct PositionPayload {
  int32_t latE6;  // absolute coordinates; wire carries signed24 fixed offsets
  int32_t lonE6;
  uint8_t speedDmS;  // 0.1 m/s, 0..254; 255 unknown/out of range
  uint16_t courseDeg10;  // 0..3599; 4095 unknown
  uint8_t satelliteClass;  // 0 unknown, 1 <=5, 2 6..7, 3 >=8
  bool fix;
  bool velocityValid;
  uint8_t hdop10;  // HDOP * 10, rounded UP; 255 unknown
  uint8_t age10ms;  // source age rounded UP to 10 ms; 255 unusable
};
struct TelemetryPayload {
  uint16_t batteryMv;  // 0 unknown
  int8_t tempC;  // INT8_MIN unknown
  uint8_t humidityPct;  // 0..100; 255 unknown
  uint8_t satellites;  // exact diagnostic count; 255 unknown
};
struct AckPayload {
  uint16_t ackSeq;
  int16_t rssiDbm10;
  int8_t snrQuarterDb;  // SX126x native 0.25 dB units, -32..31.75 dB
};
// Separate low-rate diagnostics; no raw NMEA and no extra DATA bytes.
struct DiagnosticPayload {
  uint16_t epochIntervalMs;
  uint16_t backlogDrops;  // counters saturate at 65535 at the sender
  uint16_t nmeaErrors;
  uint16_t txErrors;
  uint16_t skippedSlots;
  // bits 0..5: haveEpoch, fix, velocityValid, haveGga, haveRmc,
  // ageUncertaintySet. Bits 6..7 are reserved and must be zero.
  uint8_t status;
};

namespace protocol {
using Header = PacketHeader;
constexpr int32_t kLatitudeOriginE6 = 24000000;
constexpr int32_t kLongitudeOriginE6 = 121000000;
constexpr int32_t kSigned24Min = -8388608;
constexpr int32_t kSigned24Max = 8388607;
constexpr uint8_t kUnknown = 255;
constexpr uint16_t kUnknownCourse = 4095;
inline bool validClientId(uint16_t id) { return id != 0 && id != ID_BROADCAST; }
inline size_t packetLength(uint8_t type) {
  switch (type) {
    case MSG_DATA: return DATA_PACKET_LEN;
    case MSG_ACK: return ACK_PACKET_LEN;
    case MSG_TELEMETRY: return TELEMETRY_PACKET_LEN;
    case MSG_DIAGNOSTIC: return DIAGNOSTIC_PACKET_LEN;
    default: return 0;
  }
}
inline uint8_t satClass(int count) {
  if (count < 0 || count == kUnknown) return 0;
  if (count <= 5) return 1;
  return count <= 7 ? 2 : 3;
}
// Threshold representative only: never present it as an exact sat count.
inline int satLowerBound(uint8_t value) {
  switch (value) {
    case 1: return 0;
    case 2: return 6;
    case 3: return 8;
    default: return -1;
  }
}
inline uint8_t quantizeSpeed(double metresPerSecond) {
  if (!isfinite(metresPerSecond) || metresPerSecond < 0.0 ||
      metresPerSecond > 25.4) return kUnknown;
  return static_cast<uint8_t>(lround(metresPerSecond * 10.0));
}
// Prefer TinyGPSPlus's native integer hundredths to avoid floating-point
// threshold changes at 1.50 and 3.00.
inline uint8_t quantizeHdopCenti(uint32_t hundredths) {
  if (hundredths > 2540) return kUnknown;
  return static_cast<uint8_t>((hundredths + 9) / 10);
}
inline uint8_t quantizeHdop(double hdop) {
  if (!isfinite(hdop) || hdop < 0.0 || hdop > 25.4) return kUnknown;
  const double roundedUp = ceil(hdop * 10.0);
  return roundedUp <= 254.0 ? static_cast<uint8_t>(roundedUp) : kUnknown;
}
inline uint8_t quantizeAge(uint32_t ageMs) {
  if (ageMs > 2540) return kUnknown;
  return static_cast<uint8_t>((ageMs + 9) / 10);
}
inline bool signed24Fits(int64_t value) {
  return value >= kSigned24Min && value <= kSigned24Max;
}
inline bool coordinatesFit(int32_t latE6, int32_t lonE6) {
  return signed24Fits(static_cast<int64_t>(latE6) - kLatitudeOriginE6) &&
         signed24Fits(static_cast<int64_t>(lonE6) - kLongitudeOriginE6);
}
inline void putU16(uint8_t *out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
}
inline uint16_t getU16(const uint8_t *in) {
  return static_cast<uint16_t>(in[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(in[1]) << 8);
}
inline void putI24(uint8_t *out, int32_t value) {
  const uint32_t bits = static_cast<uint32_t>(value);
  out[0] = static_cast<uint8_t>(bits);
  out[1] = static_cast<uint8_t>(bits >> 8);
  out[2] = static_cast<uint8_t>(bits >> 16);
}
inline int32_t getI24(const uint8_t *in) {
  const uint32_t bits = static_cast<uint32_t>(in[0]) |
      (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16);
  return bits & 0x800000U ? static_cast<int32_t>(bits) - 0x1000000
                          : static_cast<int32_t>(bits);
}
inline bool validPosition(const PositionPayload &p) {
  if (p.satelliteClass > 3 ||
      (p.courseDeg10 > 3599 && p.courseDeg10 != kUnknownCourse)) return false;
  if (p.fix && (p.age10ms == kUnknown || !coordinatesFit(p.latE6, p.lonE6)))
    return false;
  // Bad vectors can retain usable positions, but contradictory flags reject.
  if (p.velocityValid && (!p.fix || p.speedDmS == kUnknown || p.speedDmS < 3 ||
                          p.courseDeg10 == kUnknownCourse)) return false;
  return true;
}
inline bool canEncode(const uint8_t *buf, size_t capacity, const Header &h,
                      uint8_t type) {
  return buf && capacity >= packetLength(type) && h.msgType == type &&
         validClientId(h.clientId);
}
inline void encodeHeader(uint8_t *buf, const Header &h) {
  buf[0] = PROTO_MAGIC;
  buf[1] = static_cast<uint8_t>((PROTO_VERSION << 4) | h.msgType);
  putU16(buf + 2, h.clientId);
  putU16(buf + 4, h.seq);
}
inline bool decodeHeader(const uint8_t *buf, size_t n, Header &out) {
  if (!buf || n < PACKET_HEADER_LEN || buf[0] != PROTO_MAGIC ||
      (buf[1] >> 4) != PROTO_VERSION) return false;
  Header h{getU16(buf + 2), getU16(buf + 4), static_cast<uint8_t>(buf[1] & 0x0F)};
  const size_t expected = packetLength(h.msgType);
  if (!expected || n != expected || !validClientId(h.clientId)) return false;
  out = h;
  return true;
}
inline size_t encodeData(uint8_t *buf, size_t capacity, const Header &h,
                         const PositionPayload &p) {
  if (!canEncode(buf, capacity, h, MSG_DATA) || !validPosition(p)) return 0;
  encodeHeader(buf, h);
  // Invalid/out-of-range coordinates may report fix=false, never wrap into
  // a plausible target. Zero offsets with fix=false have no position meaning.
  const bool inRange = coordinatesFit(p.latE6, p.lonE6);
  putI24(buf + 6, inRange ? p.latE6 - kLatitudeOriginE6 : 0);
  putI24(buf + 9, inRange ? p.lonE6 - kLongitudeOriginE6 : 0);
  buf[12] = p.speedDmS;
  const uint16_t courseFlags = p.courseDeg10 |
      (static_cast<uint16_t>(p.satelliteClass) << 12) |
      (static_cast<uint16_t>(p.fix) << 14) |
      (static_cast<uint16_t>(p.velocityValid) << 15);
  putU16(buf + 13, courseFlags);
  buf[15] = p.hdop10;
  buf[16] = p.age10ms;
  return DATA_PACKET_LEN;
}
inline bool decodeData(const uint8_t *buf, size_t n, Header &header,
                        PositionPayload &out) {
  Header h{};
  if (!decodeHeader(buf, n, h) || h.msgType != MSG_DATA) return false;
  const uint16_t flags = getU16(buf + 13);
  PositionPayload p{
      getI24(buf + 6) + kLatitudeOriginE6, getI24(buf + 9) + kLongitudeOriginE6,
      buf[12], static_cast<uint16_t>(flags & 0x0FFF),
      static_cast<uint8_t>((flags >> 12) & 3),
      (flags & 0x4000) != 0, (flags & 0x8000) != 0, buf[15], buf[16]};
  if (!validPosition(p)) return false;
  header = h;
  out = p;
  return true;
}
inline bool validTelemetry(const TelemetryPayload &p) {
  return p.humidityPct <= 100 || p.humidityPct == kUnknown;
}
inline size_t encodeTelemetry(uint8_t *buf, size_t capacity, const Header &h,
                              const TelemetryPayload &p) {
  if (!canEncode(buf, capacity, h, MSG_TELEMETRY) || !validTelemetry(p)) return 0;
  encodeHeader(buf, h);
  putU16(buf + 6, p.batteryMv);
  buf[8] = static_cast<uint8_t>(p.tempC);
  buf[9] = p.humidityPct;
  buf[10] = p.satellites;
  return TELEMETRY_PACKET_LEN;
}
inline bool decodeTelemetry(const uint8_t *buf, size_t n, Header &header,
                             TelemetryPayload &out) {
  Header h{};
  if (!decodeHeader(buf, n, h) || h.msgType != MSG_TELEMETRY) return false;
  const int temperature = buf[8] < 128 ? buf[8] : static_cast<int>(buf[8]) - 256;
  TelemetryPayload p{getU16(buf + 6), static_cast<int8_t>(temperature), buf[9], buf[10]};
  if (!validTelemetry(p)) return false;
  header = h;
  out = p;
  return true;
}
inline size_t encodeAck(uint8_t *buf, size_t capacity, const Header &h,
                        const AckPayload &p) {
  if (!canEncode(buf, capacity, h, MSG_ACK)) return 0;
  encodeHeader(buf, h);
  putU16(buf + 6, p.ackSeq);
  putU16(buf + 8, static_cast<uint16_t>(p.rssiDbm10));
  buf[10] = static_cast<uint8_t>(p.snrQuarterDb);
  return ACK_PACKET_LEN;
}
inline bool decodeAck(const uint8_t *buf, size_t n, Header &header, AckPayload &out) {
  Header h{};
  if (!decodeHeader(buf, n, h) || h.msgType != MSG_ACK) return false;
  const uint16_t rawRssi = getU16(buf + 8);
  const int32_t rssi = rawRssi < 32768 ? rawRssi : static_cast<int32_t>(rawRssi) - 65536;
  const int snr = buf[10] < 128 ? buf[10] : static_cast<int>(buf[10]) - 256;
  AckPayload p{getU16(buf + 6), static_cast<int16_t>(rssi), static_cast<int8_t>(snr)};
  header = h;
  out = p;
  return true;
}
inline size_t encodeDiagnostic(uint8_t *buf, size_t capacity, const Header &h,
                              const DiagnosticPayload &p) {
  if (!canEncode(buf, capacity, h, MSG_DIAGNOSTIC) || (p.status & 0xC0)) return 0;
  encodeHeader(buf, h);
  putU16(buf + 6, p.epochIntervalMs);
  putU16(buf + 8, p.backlogDrops);
  putU16(buf + 10, p.nmeaErrors);
  putU16(buf + 12, p.txErrors);
  putU16(buf + 14, p.skippedSlots);
  buf[16] = p.status;
  return DIAGNOSTIC_PACKET_LEN;
}
inline bool decodeDiagnostic(const uint8_t *buf, size_t n, Header &header,
                             DiagnosticPayload &out) {
  Header h{};
  if (!decodeHeader(buf, n, h) || h.msgType != MSG_DIAGNOSTIC || (buf[16] & 0xC0))
    return false;
  const DiagnosticPayload p{getU16(buf + 6), getU16(buf + 8), getU16(buf + 10),
                            getU16(buf + 12), getU16(buf + 14), buf[16]};
  header = h;
  out = p;
  return true;
}
}  // namespace protocol
