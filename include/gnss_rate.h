#pragma once
#include <cstdint>

namespace gnss_rate {
#if defined(FIELD_DIAGNOSTIC)
constexpr uint32_t kTargetIntervalMs = 1000;
#else
constexpr uint32_t kTargetIntervalMs = 500;
#endif
constexpr uint32_t kBaud = 115200;
constexpr uint32_t kWindowMs = 5000;
class Monitor {
 public:
  bool observe(uint32_t now, uint32_t epochs, uint32_t rmc, uint32_t gga) {
    if (!started_) { started_ = true; baseline(now, epochs, rmc, gga); return false; }
    const uint32_t elapsed = now - lastMs_;
    if (elapsed < kWindowMs) return false;
    hz_ = (epochs - epochs_) * 1000.0f / elapsed;
    rmcHz_ = (rmc - rmc_) * 1000.0f / elapsed;
    ggaHz_ = (gga - gga_) * 1000.0f / elapsed;
    ready_ = true; baseline(now, epochs, rmc, gga); return true;
  }
  bool ready() const { return ready_; }
  float hz() const { return hz_; }
  float rmcHz() const { return rmcHz_; }
  float ggaHz() const { return ggaHz_; }
  const char *state() const {
    if (!ready_) return "measuring";
    if (!rmcHz_ && !ggaHz_) return "no_position_sentences";
    if (!rmcHz_) return "missing_rmc";
    if (!ggaHz_) return "missing_gga";
    return inRange(hz_) && inRange(rmcHz_) && inRange(ggaHz_) ? (kTargetIntervalMs == 1000 ? "observed_1hz" : "observed_2hz") : "rate_mismatch";
  }
 private:
  bool started_ = false, ready_ = false;
  uint32_t lastMs_ = 0, epochs_ = 0, rmc_ = 0, gga_ = 0;
  float hz_ = 0, rmcHz_ = 0, ggaHz_ = 0;
  static bool inRange(float hz) { const float target = 1000.0f/kTargetIntervalMs; return hz >= target*0.8f && hz <= target*1.2f; }
  void baseline(uint32_t now, uint32_t epochs, uint32_t rmc, uint32_t gga) {
    lastMs_ = now; epochs_ = epochs; rmc_ = rmc; gga_ = gga;
  }
};
} // namespace gnss_rate
