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

// Legacy LoRa lacks a boot/session id. While connected, reject duplicates and
// backwards seq. After a gap, require two advancing candidates before rebinding
// the baseline (supports client reboot without accepting a lone late packet).
class RadioSequence {
 public:
  bool accept(uint16_t seq, uint32_t now) {
    if (!have_) { commit(seq, now); return true; }
    if (now - acceptedMs_ < 2500) {
      const uint16_t delta = seq - sequence_;
      if (!delta || delta >= 0x8000) return false;
      commit(seq, now); return true;
    }
    const uint16_t delta = seq - candidate_;
    if (candidateValid_ && now - candidateMs_ >= 250 && now - candidateMs_ < 2000 &&
        delta && delta < 0x8000) {
      commit(seq, now); return true;
    }
    if (!candidateValid_ || now - candidateMs_ >= 2000) {
      candidate_ = seq; candidateMs_ = now; candidateValid_ = true;
    }
    return false;
  }
 private:
  void commit(uint16_t seq, uint32_t now) {
    have_ = true; sequence_ = seq; acceptedMs_ = now; candidateValid_ = false;
  }
  bool have_ = false, candidateValid_ = false;
  uint16_t sequence_ = 0, candidate_ = 0;
  uint32_t acceptedMs_ = 0, candidateMs_ = 0;
};
}  // namespace command_freshness
