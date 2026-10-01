#pragma once
#include <stdint.h>
#include <stddef.h>

namespace command_freshness {
inline bool parseUint32(const char *text, size_t length, uint32_t &out) {
  if (!text || !length || length > 10) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < length; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + text[i] - '0';
    if (value > UINT32_MAX) return false;
  }
  out = static_cast<uint32_t>(value); return true;
}
class HttpGate {
 public:
  void reset(uint32_t epoch) { epoch_ = epoch; sequence_ = 0; }
  bool accept(uint32_t epoch, uint32_t sequence, uint32_t stamp, uint32_t now,
              uint32_t maxAge = 2000) {
    if (epoch != epoch_ || now - stamp >= maxAge ||
        static_cast<int32_t>(sequence - sequence_) <= 0) return false;
    sequence_ = sequence; return true;
  }
  uint32_t epoch() const { return epoch_; }
  uint32_t sequence() const { return sequence_; }
 private:
  uint32_t epoch_ = 0, sequence_ = 0;
};

// Single transmitter, no retries/queued history. A >=3 s DATA gap explicitly
// starts a new sequence baseline on the first valid fix. This deliberately
// cannot distinguish reboot from an outage or authenticate a replay.
// Within a connected interval, keep rejecting duplicate/backward sequences.
class RadioSequence {
 public:
  static constexpr uint32_t kResetGapMs = 3000;
  bool accept(uint16_t seq, uint32_t now, bool validFix = true) {
    resetAfterGap_ = false;
    if (!have_ || now - acceptedMs_ >= kResetGapMs) {
      if (!validFix) return false;
      resetAfterGap_ = have_;
      commit(seq, now); return true;
    }
    const uint16_t delta = seq - sequence_;
    if (!delta || delta >= 0x8000) return false;
    commit(seq, now); return true;
  }
  bool resetAfterGap() const { return resetAfterGap_; }
 private:
  void commit(uint16_t seq, uint32_t now) {
    have_ = true; sequence_ = seq; acceptedMs_ = now;
  }
  bool have_ = false, resetAfterGap_ = false;
  uint16_t sequence_ = 0;
  uint32_t acceptedMs_ = 0;
};
}  // namespace command_freshness
