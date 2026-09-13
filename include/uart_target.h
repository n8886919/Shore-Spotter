#pragma once
#include <stdint.h>
#include "servo_profile.h"

namespace uart_target {
class Mailbox {
 public:
  bool set(int32_t angle, uint32_t now) {
    if (!servo_profile::withinSoftLimits(angle)) return false;
    target_ = angle; received_ = now; valid_ = true; expired_ = false;
    return true;
  }
  void expire(uint32_t now) {
    if (valid_ && now - received_ >= servo_profile::kWatchdogMs) {
      valid_ = false; expired_ = true;
    }
  }
  void hold() { valid_ = false; expired_ = true; }
  bool ready() const { return valid_; }
  int32_t target() const { return target_; }
  const char *state() const { return valid_ ? "tracking" : expired_ ? "watchdog_hold" : "waiting"; }
 private:
  int32_t target_ = 90000;
  uint32_t received_ = 0;
  bool valid_ = false, expired_ = false;
};
}  // namespace uart_target
