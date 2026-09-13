#pragma once
#include <stdint.h>

namespace loop_metrics {
inline bool due(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

// Cumulative since boot; duration is measured around the actual synchronous call.
struct Duration {
  uint32_t lastUs = 0;
  uint32_t maxUs = 0;
  uint32_t over50ms = 0;
  void record(uint32_t startUs, uint32_t endUs) {
    lastUs = endUs - startUs;
    if (lastUs > maxUs) maxUs = lastUs;
    if (lastUs >= 50000 && over50ms != UINT32_MAX) ++over50ms;
  }
};

struct Gap {
  uint32_t maxMs = 0;
  uint32_t over250ms = 0;
  void observe(uint32_t nowMs) {
    if (seen_) {
      const uint32_t gap = nowMs - previousMs_;
      if (gap > maxMs) maxMs = gap;
      if (gap >= 250 && over250ms != UINT32_MAX) ++over250ms;
    }
    previousMs_ = nowMs;
    seen_ = true;
  }
 private:
  bool seen_ = false;
  uint32_t previousMs_ = 0;
};

// Clock must expose micros(). RAII also records an early return.
template <typename Clock>
class Measure {
 public:
  explicit Measure(Duration &duration) : duration_(duration), start_(Clock::micros()) {}
  ~Measure() { duration_.record(start_, Clock::micros()); }
 private:
  Duration &duration_;
  uint32_t start_;
};
}  // namespace loop_metrics
