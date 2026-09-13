#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "servo_profile.h"
#include "command_freshness.h"

namespace uart_set_parser {
enum class Result : uint8_t { None, Target, Rejected, Sync };
struct Frame { bool sequenced = false; uint32_t epoch = 0, seq = 0, stamp = 0; };

// Partial lines expire too: a late suffix must not complete an old command.
class Parser {
 public:
  void reset(bool discardUntilNewline = false) {
    size_ = 0;
    discard_ = discardUntilNewline;
  }
  Result push(char byte, uint32_t nowMs, int32_t &angleMdeg) {
    if (size_ != 0 && nowMs - firstByteMs_ >= servo_profile::kWatchdogMs) reset(true);
    if (byte == '\r' || byte == '\n') {
      if (discard_) { reset(); return Result::Rejected; }
      if (size_ == 0) return Result::None;
      if (size_ == 4 && memcmp(line_, "SYNC", 4) == 0) { reset(); return Result::Sync; }
      const bool valid = parse(angleMdeg);
      reset();
      return valid ? Result::Target : Result::Rejected;
    }
    if (discard_) return Result::None;
    if (size_ == 0) firstByteMs_ = nowMs;
    if (size_ == sizeof(line_)) { reset(true); return Result::None; }
    line_[size_++] = byte;
    return Result::None;
  }
  const Frame &frame() const { return frame_; }
 private:
  bool parse(int32_t &angleMdeg) {
    frame_ = Frame{};
    if (size_ >= 5 && memcmp(line_, "SET2 ", 5) == 0) {
      uint32_t values[4]{}; size_t start = 5;
      for (size_t field = 0; field < 4; ++field) {
        size_t end = start;
        while (end < size_ && line_[end] != ' ') ++end;
        if (!command_freshness::parseUint32(line_ + start, end - start, values[field])) return false;
        if ((field < 3 && end == size_) || (field == 3 && end != size_)) return false;
        start = end + 1;
      }
      if (values[3] > static_cast<uint32_t>(servo_profile::kSoftMaximumMdeg)) return false;
      angleMdeg = static_cast<int32_t>(values[3]);
      frame_ = {true, values[0], values[1], values[2]}; return true;
    }
    if (size_ < 5 || size_ > 10 || memcmp(line_, "SET ", 4) != 0) return false;
    int32_t value = 0;
    for (size_t i = 4; i < size_; ++i) {
      if (line_[i] < '0' || line_[i] > '9') return false;
      value = value * 10 + line_[i] - '0';
    }
    if (!servo_profile::withinSoftLimits(value)) return false;
    angleMdeg = value;
    return true;
  }
  char line_[64]{};
  Frame frame_;
  size_t size_ = 0;
  bool discard_ = false;
  uint32_t firstByteMs_ = 0;
};
}  // namespace uart_set_parser
