#pragma once

// Bounded, Arduino-free BLE control wire format shared by the Client and Station.
#include <stddef.h>
#include <stdint.h>

#include "client_control.h"
#include "protocol.h"

namespace ble_control {

constexpr const char kServiceUuid[] = "a87e0001-74e8-4da8-9b2a-46b710924010";
constexpr const char kCommandUuid[] = "a87e0002-74e8-4da8-9b2a-46b710924010";
constexpr const char kStatusUuid[] = "a87e0003-74e8-4da8-9b2a-46b710924010";
constexpr size_t kCommandSize = 20;
constexpr size_t kStatusSize = 40;
constexpr uint8_t kSf = 9;
constexpr float kBandwidthKhz = 125.0f;
constexpr uint8_t kCodingRate = 5;
constexpr uint8_t kSyncWord = 0x12;
constexpr uint8_t kMagic = 0x53;
constexpr uint8_t kVersion = 1;

enum class Op : uint8_t { Claim = 1, Start = 2, Stop = 3, Test = 4 };
enum class Result : uint8_t { Idle = 0, Accepted = 1, Rejected = 2 };

struct Command {
  Op op = Op::Claim;
  uint8_t channel = 0;
  uint16_t client = 0;
  uint16_t station = 0;
  uint32_t boot = 0;
  uint16_t seq = 0;
  uint32_t stationBoot = 0;
};

struct Status {
  client_control::State state = client_control::State::Ready;
  uint8_t channel = 0;
  uint16_t client = 0;
  uint16_t owner = 0;
  uint32_t boot = 0;
  uint16_t command = 0;
  uint16_t batteryMv = 0;
  uint32_t txPackets = 0;
  uint16_t rxErrors = 0;
  uint8_t flags = 0;
  Result result = Result::Idle;
  int16_t error = 0;
  uint8_t sf = kSf;
  uint32_t uptime = 0;
  uint32_t stationBoot = 0;
  uint16_t replySeq = 0;
};

inline bool validChannel(uint8_t channel) { return channel <= 1; }
inline float frequencyMhz(uint8_t channel) {
  return channel == 0 ? 923.2f : (channel == 1 ? 923.8f : 0.0f);
}
inline const char *channelName(uint8_t channel) {
  return channel == 0 ? "A" : (channel == 1 ? "B" : "invalid");
}
inline bool validOp(Op op) { return uint8_t(op) >= 1 && uint8_t(op) <= 4; }
inline bool validState(client_control::State state) {
  return state == client_control::State::Ready ||
         state == client_control::State::Tracking ||
         state == client_control::State::Test;
}
inline bool validResult(Result result) { return uint8_t(result) <= 2; }

inline uint16_t get16(const uint8_t *p) {
  return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
inline uint32_t get32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
         uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline int16_t getSigned16(const uint8_t *p) {
  const uint16_t raw = get16(p);
  return raw <= 0x7fff ? int16_t(raw) : int16_t(int32_t(raw) - 0x10000);
}
inline void put16(uint8_t *p, uint16_t v) {
  p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
}
inline void put32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

inline bool validCommand(const Command &c) {
  return validOp(c.op) && validChannel(c.channel) &&
         protocol::validClientId(c.client) && protocol::validClientId(c.station) &&
         c.boot != 0 && c.stationBoot != 0;
}
inline size_t encodeCommand(uint8_t *out, size_t capacity, const Command &c) {
  if (!out || capacity < kCommandSize || !validCommand(c)) return 0;
  out[0] = kMagic; out[1] = kVersion; out[2] = uint8_t(c.op); out[3] = c.channel;
  put16(out + 4, c.client); put16(out + 6, c.station); put32(out + 8, c.boot);
  put16(out + 12, c.seq); put16(out + 14, 0); put32(out + 16, c.stationBoot);
  return kCommandSize;
}
inline bool decodeCommand(const uint8_t *data, size_t size, Command &out) {
  if (!data || size != kCommandSize || data[0] != kMagic || data[1] != kVersion ||
      get16(data + 14) != 0) return false;
  Command c{Op(data[2]), data[3], get16(data + 4), get16(data + 6),
            get32(data + 8), get16(data + 12), get32(data + 16)};
  if (!validCommand(c)) return false;
  out = c;
  return true;
}

inline bool validStatus(const Status &s) {
  return validState(s.state) && validChannel(s.channel) &&
         protocol::validClientId(s.client) &&
         (s.owner == 0 || protocol::validClientId(s.owner)) && s.boot != 0 &&
         (s.owner == 0 || s.stationBoot != 0) && (s.flags & 0xF0) == 0 &&
         validResult(s.result) && s.sf == kSf;
}
inline size_t encodeStatus(uint8_t *out, size_t capacity, const Status &s) {
  if (!out || capacity < kStatusSize || !validStatus(s)) return 0;
  out[0] = kMagic; out[1] = kVersion; out[2] = uint8_t(s.state); out[3] = s.channel;
  put16(out + 4, s.client); put16(out + 6, s.owner); put32(out + 8, s.boot);
  put16(out + 12, s.command); put16(out + 14, s.batteryMv); put32(out + 16, s.txPackets);
  put16(out + 20, s.rxErrors); out[22] = s.flags; out[23] = uint8_t(s.result);
  put16(out + 24, uint16_t(s.error)); out[26] = s.sf; out[27] = 0;
  put32(out + 28, s.uptime); put32(out + 32, s.stationBoot);
  put16(out + 36, s.replySeq); put16(out + 38, 0);
  return kStatusSize;
}
inline bool decodeStatus(const uint8_t *data, size_t size, Status &out) {
  if (!data || size != kStatusSize || data[0] != kMagic || data[1] != kVersion ||
      data[27] != 0 || get16(data + 38) != 0) return false;
  Status s{client_control::State(data[2]), data[3], get16(data + 4), get16(data + 6),
           get32(data + 8), get16(data + 12), get16(data + 14), get32(data + 16),
           get16(data + 20), data[22], Result(data[23]), getSigned16(data + 24),
           data[26], get32(data + 28), get32(data + 32), get16(data + 36)};
  if (!validStatus(s)) return false;
  out = s;
  return true;
}

}  // namespace ble_control
