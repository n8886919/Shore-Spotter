#pragma once
#include <stdint.h>

namespace client_cadence {
constexpr uint32_t kPairWaitMs = 150;

// Observe the latest fresh GNSS fix. No periodic DATA heartbeat, queued history,
// or same-epoch retry. A valid->invalid transition gets one DATA status packet.
// The radio scheduler owns airtime/guard; this class adds no fixed rate cap.
class Scheduler {
 public:
  void reset(uint32_t /*now*/) { *this = Scheduler{}; }
  bool due(uint32_t now, bool validFix, uint32_t epoch, bool paired) {
    if (!validFix) return lastAttemptValid_;
    if (!observed_ || epoch != observedEpoch_) {
      observed_ = true; observedEpoch_ = epoch; observedMs_ = now;
    }
    const bool newEpoch = !lastAttemptValid_ || epoch != lastEpoch_;
    return newEpoch && (paired || now - observedMs_ >= kPairWaitMs);
  }
  // Consume even a failed local TX. Only a newer fix may trigger another DATA.
  void attempted(uint32_t /*now*/, bool validFix, uint32_t epoch) {
    lastAttemptValid_ = validFix; lastEpoch_ = epoch;
  }
 private:
  uint32_t lastEpoch_ = 0, observedEpoch_ = 0, observedMs_ = 0;
  bool lastAttemptValid_ = false, observed_ = false;
};
}  // namespace client_cadence
