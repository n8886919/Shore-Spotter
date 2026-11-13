#pragma once

#include <stdint.h>

namespace t096_power {
constexpr uint16_t kWarnMv = 3500, kShutdownMv = 3200;
constexpr uint32_t kScreenMs = 60000, kAutomaticScreenMs = 5000;
constexpr uint32_t kPreviewMs = 1000, kHoldMs = 5000;
constexpr uint32_t kBatteryReadyMs = 30000, kBatteryActiveMs = 1000, kBatteryProtectMs = 60000;

// Heltec: 12-bit ADC, internal 3.0 V reference, (390k+100k)/100k divider.
inline uint16_t batteryMv(uint16_t raw) {
  if (raw > 4095) return 0;
  const uint16_t mv = (uint32_t(raw)*14700+2048)/4096;
  return mv >= 2500 && mv <= 4500 ? mv : 0;
}
inline uint8_t batteryPercent(uint16_t mv) {
  if (mv <= 3200) return 0;
  if (mv >= 4150) return 100;
  return (uint32_t(mv-3200)*100+475)/950;
}
class Battery {
 public:
  void observe(uint16_t mv, bool usb) {
    mv_ = mv;
    if (!mv || usb || mv >= kShutdownMv) low_ = 0;
    else if (low_ < 3) ++low_;
    if (low_ >= 3) protected_ = true;
    if (protected_) {
      if (mv >= kWarnMv) { if (recovered_ < 3) ++recovered_; }
      else recovered_ = 0;
      if (recovered_ >= 3) { protected_ = false; recovered_ = 0; low_ = 0; }
    }
  }
  uint16_t mv() const { return mv_; }
  bool warning() const { return mv_ && mv_ < kWarnMv; }
  bool shutdown() const { return low_ >= 3; }
  bool protectedSleep() const { return protected_; }
 private:
  uint16_t mv_ = 0;
  uint8_t low_ = 0, recovered_ = 0;
  bool protected_ = false;
};

enum class ButtonEvent : uint8_t { None, Short, Preview, Hold, Cancelled, LongReleased };
class Button {
 public:
  void begin(bool pressed, uint32_t now) {
    raw_ = stable_ = pressed; changed_ = pressedAt_ = now;
    armed_ = !pressed; active_ = held_ = preview_ = false;
  }
  ButtonEvent poll(bool pressed, uint32_t now) {
    if (pressed != raw_) { raw_ = pressed; changed_ = now; }
    if (raw_ != stable_ && uint32_t(now-changed_) >= 30) {
      stable_ = raw_;
      if (!stable_ && !armed_) { armed_ = true; return ButtonEvent::None; }
      if (stable_ && armed_) { active_ = true; held_ = preview_ = false; pressedAt_ = now; }
      else if (!stable_ && active_) {
        active_ = false;
        // Measure at the first release edge, not after release debounce. A
        // release just before 5 s must not become a mode switch 30 ms later.
        const uint32_t duration = uint32_t(changed_ - pressedAt_);
        return duration >= kHoldMs ? ButtonEvent::LongReleased :
               duration >= kPreviewMs ? ButtonEvent::Cancelled : ButtonEvent::Short;
      }
    }
    if (active_ && stable_ && raw_ && !held_ && uint32_t(now-pressedAt_) >= kHoldMs) {
      preview_ = true;
      held_ = true; return ButtonEvent::Hold;
    }
    if (active_ && stable_ && raw_ && !preview_ && uint32_t(now-pressedAt_) >= kPreviewMs) {
      preview_ = true; return ButtonEvent::Preview;
    }
    return ButtonEvent::None;
  }
  bool held() const { return active_ && held_; }
  bool previewing() const { return active_ && preview_; }
  uint32_t remainingSeconds(uint32_t now) const {
    const uint32_t age = uint32_t(now-pressedAt_);
    return age >= kHoldMs ? 0 : (kHoldMs-age+999)/1000;
  }
  // Includes the release debounce: its GPIO edge may have already woken us.
  bool busy() const { return raw_ || stable_ || active_; }
 private:
  bool raw_ = false, stable_ = false, armed_ = true, active_ = false, held_ = false, preview_ = false;
  uint32_t changed_ = 0, pressedAt_ = 0;
};
}
