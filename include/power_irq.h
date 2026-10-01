#pragma once
#include <stdint.h>

// AXP2101 key IRQs. Keep signed bus results until they have been checked:
// XPowersLib's getIrqStatus() narrows a failed read (-1) to 0xff.
namespace power_irq {
constexpr uint8_t kStatus1 = 0x48;
constexpr uint8_t kShort = 0x08, kLong = 0x04;
// AXP2101 REG49: PKEY is pulled to GND when pressed. Negative = press,
// positive = release. These are latched edges, not the current pin level.
constexpr uint8_t kNegative = 0x02, kPositive = 0x01;
constexpr uint8_t kAllKeys = kShort | kLong | kNegative | kPositive;
constexpr int kNotAttempted = -2;

struct Sample {
  int raw[3] = {-1, -1, -1};
  int clear[3] = {kNotAttempted, kNotAttempted, kNotAttempted};
  uint8_t readFailed = 0, clearFailed = 0, suppressed = 0, fresh = 0;
  bool shortPress = false, longPress = false;
};
struct State {
  uint8_t consumed = 0;
  uint32_t readErrors = 0, clearErrors = 0, suppressedKeys = 0;
};

template <typename Pmu>
Sample read(Pmu &pmu, State &state, uint8_t keyMask = kShort | kLong) {
  Sample sample;
  for (unsigned i = 0; i < 3; ++i) {
    sample.raw[i] = pmu.readRegister(kStatus1 + i);
    if (sample.raw[i] < 0 || sample.raw[i] > 255) {
      sample.readFailed |= uint8_t(1U << i);
      ++state.readErrors;
    }
  }
  // Leave all latched bits alone after an incomplete snapshot. A later valid
  // read can still deliver a real key event; errors never fabricate a press.
  if (sample.readFailed) return sample;

  const uint8_t keys = uint8_t(sample.raw[1]) & keyMask;
  state.consumed &= keys;
  sample.suppressed = keys & state.consumed;
  if (sample.suppressed) ++state.suppressedKeys;
  sample.fresh = keys & ~state.consumed;
  sample.shortPress = sample.fresh & kShort;
  sample.longPress = sample.fresh & kLong;
  state.consumed |= keys;
  return sample;
}

template <typename Pmu>
void clear(Pmu &pmu, State &state, Sample &sample) {
  if (sample.readFailed) return;
  for (unsigned i = 0; i < 3; ++i) {
    // Clear only observed bits, never an unread/new unrelated IRQ.
    if (!sample.raw[i]) continue;
    sample.clear[i] = pmu.writeRegister(kStatus1 + i, uint8_t(sample.raw[i]));
    if (sample.clear[i] != 0) {
      sample.clearFailed |= uint8_t(1U << i);
      ++state.clearErrors;
    }
  }
  // Dispatch a valid key once even if clearing fails. Retry its clear, but do
  // not replay OLED/shutdown actions while that same latch remains set.
  if (sample.clear[1] == 0) state.consumed = 0;
}

template <typename Pmu>
Sample poll(Pmu &pmu, State &state) {
  Sample sample = read(pmu, state);
  clear(pmu, state, sample);
  return sample;
}

// Client only: the press that powers the board up cannot also request a
// software shutdown. Capture/clear the first valid IRQ snapshot, then require
// a NEW falling edge after that baseline, including while the boot screen is
// visible. A held starting press cannot create another falling edge: the key
// must have been released and pressed again. Quiet IRQs do not prove release,
// and a positive edge alone is never an actionable press.
// Once armed, preserve the existing runtime key and clear-retry behavior.
struct ClientBootGuard {
  enum Change : uint8_t { None, BaselineReady, Armed };
  bool baselineReady = false, armed = false;

  Change filter(Sample &sample) {
    if (armed) return None;
    Change change = None;
    if (sample.readFailed || sample.clearFailed) {
      baselineReady = false;
      armed = false;
    } else if (!baselineReady) {
      baselineReady = true;
      change = BaselineReady;
    } else if (sample.fresh & kNegative) {
      armed = true;
      return Armed;
    }
    sample.shortPress = sample.longPress = false;
    return change;
  }
};

// Same AXP2101 VBUS predicate as XPowersLib, with an explicit unknown result.
inline bool vbusKnown(int status1, int status2) {
  return status1 >= 0 && status1 <= 255 && status2 >= 0 && status2 <= 255;
}
inline bool vbusPresent(int status1, int status2) {
  return vbusKnown(status1, status2) && (status1 & (1 << 5)) && !(status2 & (1 << 3));
}
} // namespace power_irq
