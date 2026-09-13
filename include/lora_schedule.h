#pragma once
#include <stdint.h>

namespace lora_schedule {
inline bool dataFits(uint32_t now, uint32_t nextData, uint32_t dataMs,
                     uint32_t ackMs, uint32_t guardMs, bool ackCycle) {
  return static_cast<int32_t>(nextData - now) > 0 &&
         nextData - now >= dataMs + (ackCycle ? ackMs : 0) + guardMs;
}
// Claim at most one current slot. Missed slots are skipped, never replayed.
inline bool claim(uint32_t now, uint32_t period, uint32_t &next, uint32_t &skipped) {
  if (!period || static_cast<int32_t>(now - next) < 0) return false;
  const uint32_t missed = (now - next) / period;
  skipped += missed;
  next += (missed + 1) * period;
  return true;
}
inline bool telemetryFits(uint32_t now, uint32_t lastDataStart, uint32_t nextData,
                          uint32_t dataMs, uint32_t telemetryMs, uint32_t guardMs,
                          bool haveData, bool ackCycle) {
  if (!haveData || ackCycle || static_cast<int32_t>(nextData - now) <= 0) return false;
  return now - lastDataStart >= dataMs + guardMs &&
         nextData - now >= telemetryMs + guardMs;
}
class AckWindow {
 public:
  void expect(uint16_t seq, uint32_t now, uint32_t lengthMs) {
    seq_ = seq; started_ = now; length_ = lengthMs; pending_ = true;
  }
  void clear() { pending_ = false; }
  bool pending(uint32_t now) const { return pending_ && now - started_ < length_; }
  bool accept(uint16_t seq, uint32_t now) {
    if (!pending_ || seq != seq_ || now - started_ >= length_) return false;
    pending_ = false; return true;
  }
 private:
  uint16_t seq_ = 0;
  uint32_t started_ = 0, length_ = 0;
  bool pending_ = false;
};
}  // namespace lora_schedule
