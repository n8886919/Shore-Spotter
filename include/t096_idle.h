#pragma once
#include <stdint.h>

namespace t096_idle {
// Only future deadlines within INT32_MAX ms are accepted. Relative periodic
// timers use their own unsigned age, so a stale absolute timer cannot strand us.
class Budget {
 public:
  explicit Budget(uint32_t now, uint32_t limit) : now_(now), wait_(limit) {}
  void cap(uint32_t ms) { if (ms < wait_) wait_ = ms; }
  void until(uint32_t deadline) {
    const int32_t remaining = int32_t(deadline - now_);
    cap(remaining <= 0 ? 0 : uint32_t(remaining));
  }
  uint32_t waitMs() const { return wait_; }
 private:
  uint32_t now_, wait_;
};
}
