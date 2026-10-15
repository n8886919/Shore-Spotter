#pragma once

#include <stdint.h>

namespace t096_power {
constexpr uint16_t kWarnMv = 3500, kShutdownMv = 3200;
constexpr uint32_t kScreenMs = 5000, kHoldMs = 2000;
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

enum class ButtonEvent : uint8_t { None, Short, Hold, LongReleased };
class Button {
 public:
  void begin(bool pressed, uint32_t now) {
    raw_ = stable_ = pressed; changed_ = pressedAt_ = now;
    armed_ = !pressed; active_ = held_ = false;
  }
  ButtonEvent poll(bool pressed, uint32_t now) {
    if (pressed != raw_) { raw_ = pressed; changed_ = now; }
    if (raw_ != stable_ && uint32_t(now-changed_) >= 30) {
      stable_ = raw_;
      if (!stable_ && !armed_) { armed_ = true; return ButtonEvent::None; }
      if (stable_ && armed_) { active_ = true; held_ = false; pressedAt_ = now; }
      else if (!stable_ && active_) {
        active_ = false;
        return held_ ? ButtonEvent::LongReleased : ButtonEvent::Short;
      }
    }
    if (active_ && stable_ && !held_ && uint32_t(now-pressedAt_) >= kHoldMs) {
      held_ = true; return ButtonEvent::Hold;
    }
    return ButtonEvent::None;
  }
  bool held() const { return active_ && held_; }
  // Includes the release debounce: its GPIO edge may have already woken us.
  bool busy() const { return raw_ || stable_ || active_; }
 private:
  bool raw_ = false, stable_ = false, armed_ = true, active_ = false, held_ = false;
  uint32_t changed_ = 0, pressedAt_ = 0;
};
}
