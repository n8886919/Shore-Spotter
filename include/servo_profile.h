#pragma once

#include <stddef.h>
#include <stdint.h>
#include <cmath>

namespace servo_profile {

constexpr char kName[] = "gx42-180-v1";
constexpr int32_t kHardMinimumMdeg = 0;
constexpr int32_t kHardMaximumMdeg = 180000;
// These are deliberately explicit even though they currently equal the known
// 180-degree endpoints. Narrow them after supervised mechanical commissioning.
constexpr int32_t kSoftMinimumMdeg = 0;
constexpr int32_t kSoftMaximumMdeg = 180000;
constexpr int32_t kHomeMdeg = 90000;
constexpr uint16_t kPulseMinimumUs = 500;
constexpr uint16_t kPulseMaximumUs = 2500;
constexpr uint16_t kPwmHz = 333;
constexpr uint8_t kPwmResolutionBits = 14;
constexpr uint32_t kPwmMaxDuty = (1u << kPwmResolutionBits) - 1;
constexpr uint16_t kWatchdogMs = 250;

static_assert(kHardMinimumMdeg < kHardMaximumMdeg,
              "hard angle range must increase");
static_assert(kHardMinimumMdeg <= kSoftMinimumMdeg &&
                  kSoftMinimumMdeg < kSoftMaximumMdeg &&
                  kSoftMaximumMdeg <= kHardMaximumMdeg,
              "soft limits must be an increasing subset of hard limits");
static_assert(kHomeMdeg >= kSoftMinimumMdeg &&
                  kHomeMdeg <= kSoftMaximumMdeg,
              "home must be inside soft limits");
static_assert(kPulseMinimumUs < kPulseMaximumUs,
              "pulse range must increase");
static_assert(static_cast<uint32_t>(kPulseMaximumUs) * kPwmHz < 1000000UL,
              "maximum pulse must fit inside one PWM period");
static_assert(kPwmResolutionBits > 0 && kPwmResolutionBits <= 14,
              "ESP32-S3 LEDC resolution must be in [1, 14]");
static_assert(kWatchdogMs > 0 && kWatchdogMs <= 500,
              "watchdog must stop stale commands quickly");

inline bool withinSoftLimits(int32_t angleMdeg) {
  return angleMdeg >= kSoftMinimumMdeg && angleMdeg <= kSoftMaximumMdeg;
}

inline int32_t clampHard(int32_t angleMdeg) {
  if (angleMdeg < kHardMinimumMdeg) return kHardMinimumMdeg;
  if (angleMdeg > kHardMaximumMdeg) return kHardMaximumMdeg;
  return angleMdeg;
}

inline uint16_t pulseForAngleMdeg(int32_t angleMdeg) {
  const int32_t safe = clampHard(angleMdeg);
  const int64_t angleSpan =
      static_cast<int64_t>(kHardMaximumMdeg) - kHardMinimumMdeg;
  const int64_t pulseSpan =
      static_cast<int64_t>(kPulseMaximumUs) - kPulseMinimumUs;
  const int64_t numerator =
      (static_cast<int64_t>(safe) - kHardMinimumMdeg) * pulseSpan;
  return static_cast<uint16_t>(
      kPulseMinimumUs + (numerator + angleSpan / 2) / angleSpan);
}

// Keep fractional microseconds until the final hardware duty quantization.
// Callers validate finite angles; clamp the fixed mechanical command range.
inline uint32_t dutyForAngle(double angleDeg) {
  if(angleDeg<0)angleDeg=0;
  if(angleDeg>180)angleDeg=180;
  const double pulseUs=kPulseMinimumUs+
      angleDeg*(kPulseMaximumUs-kPulseMinimumUs)/180.0;
  return static_cast<uint32_t>(std::lround(pulseUs*kPwmHz*kPwmMaxDuty/1000000.0));
}

}  // namespace servo_profile
