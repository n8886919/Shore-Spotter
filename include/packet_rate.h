#pragma once
#include <cstddef>
#include <cstdint>

namespace packet_rate {
// 64 arrivals exceed the SF10 DATA airtime limit (~31 frames / 10 seconds).
// Fixed storage, no allocation; timestamps are actual accepted DATA arrivals.
class Window10s {
 public:
  void record(uint32_t now) {
    expire(now);
    times_[next_] = now;
    next_ = (next_ + 1) % kCapacity;
    if (count_ < kCapacity) ++count_;
  }
  float fps(uint32_t now) {
    expire(now);
    return count_ / 10.0f;
  }
 private:
  static constexpr size_t kCapacity = 64;
  uint32_t times_[kCapacity]{};
  size_t next_ = 0, count_ = 0;
  void expire(uint32_t now) {
    while (count_ && uint32_t(now - times_[(next_ + kCapacity - count_) % kCapacity]) >= 10000)
      --count_;
  }
};
}  // namespace packet_rate
