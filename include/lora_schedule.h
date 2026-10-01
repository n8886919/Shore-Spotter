#pragma once
#include <stdint.h>

namespace lora_schedule {
// Two guard intervals are needed to place an extra packet after DATA and
// before the next slot. At SF10 this is impossible, so reserve a whole slot.
inline bool dedicatedTelemetrySlot(uint32_t periodMs, uint32_t dataMs,
                                   uint32_t extraMs, uint32_t guardMs) {
  return uint64_t(dataMs) + extraMs + 2ULL * guardMs > periodMs;
}
inline bool dataFits(uint32_t now, uint32_t nextData, uint32_t dataMs,
                     uint32_t guardMs) {
  return static_cast<int32_t>(nextData - now) > 0 &&
         nextData - now >= dataMs + guardMs;
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
                          bool haveData) {
  if (!haveData || static_cast<int32_t>(nextData - now) <= 0) return false;
  return now - lastDataStart >= dataMs + guardMs &&
         nextData - now >= telemetryMs + guardMs;
}
}  // namespace lora_schedule
