#pragma once
#include <cmath>
#include <cstdio>
#include <cstdint>
#include "servo_profile.h"

// Youmi's separate controller accepts ASCII pulse commands. It has no
// documented ACK or physical-position feedback. Never infer either from TX.
namespace servo_controller {
constexpr uint32_t kBaud = 9600, kPeriodMs = 20;
enum class Result { Idle, Deferred, Sent, Fault };
class Output {
 public:
  constexpr uint8_t channel() const { return 0; }
  uint16_t pulse() const { return lastPulse_; }
  uint32_t sent() const { return sent_; }
  uint32_t deferred() const { return deferred_; }
  uint32_t errors() const { return errors_; }
  const char *frame() const { return frame_; }
  uint32_t lastSentMs() const { return lastSentMs_; }
  template<class Serial>
  Result send(Serial &serial, double angle, uint32_t now) {
    if (!std::isfinite(angle) || angle < 0 || angle > 359) return Result::Fault;
    const auto pulse = servo_profile::pulseForAngleMdeg(std::lround(angle * 1000));
    if (pulse == lastPulse_) return Result::Idle;
    if (attempted_ && uint32_t(now - lastAttemptMs_) < kPeriodMs) return Result::Deferred;
    attempted_ = true; lastAttemptMs_ = now;
    char command[10];
    const int length = std::snprintf(command, sizeof(command), "#%02uP%04u!", 0u, pulse);
    if (length != 9) { ++errors_; return Result::Fault; }
    // Bounded nonblocking output: no queues of stale targets, no flush/wait.
    if (serial.availableForWrite() < length) { ++deferred_; return Result::Deferred; }
    if (serial.write(reinterpret_cast<const uint8_t *>(command), length) != size_t(length)) {
      ++errors_; return Result::Fault;
    }
    for (size_t i = 0; i < sizeof(command); ++i) frame_[i] = command[i];
    lastPulse_ = pulse; lastSentMs_ = now; ++sent_; return Result::Sent;
  }
 private:
  uint16_t lastPulse_ = 0;
  bool attempted_ = false;
  uint32_t lastAttemptMs_ = 0, lastSentMs_ = 0, sent_ = 0, deferred_ = 0, errors_ = 0;
  char frame_[10]{};
};
} // namespace servo_controller
