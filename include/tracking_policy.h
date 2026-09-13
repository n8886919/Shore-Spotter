#pragma once

#include <stdint.h>
#include "geo_math.h"

namespace tracking_policy {

constexpr uint32_t kGpsFreshMs = 2000;
constexpr uint32_t kGpsRecoveryMs = 2000;

enum class Source : uint8_t { Hold, Gps, Uart };
enum class Mode : uint8_t { Manual, Gps, Uart, Paused };

inline bool usableGps(bool fix, int satellites, float hdop, uint32_t ageMs) {
  return isfinite(hdop) && hdop >= 0.0f && ageMs < kGpsFreshMs &&
         (geo::gpsSignal(fix, satellites, hdop) == geo::SIG_GOOD ||
          geo::gpsSignal(fix, satellites, hdop) == geo::SIG_OK);
}

// GPS and UART are mutually exclusive, explicitly selected modes. A bad
// signal holds in the selected mode and must never activate the other source.
class Selector {
 public:
  void reset() { usableSinceMs_ = 0; recovering_ = false; gpsReady_ = false; }

  Source update(uint32_t nowMs, Mode mode, bool gpsUsable, bool uartReady) {
    if (mode != Mode::Gps) {
      reset();
      return mode == Mode::Uart && uartReady ? Source::Uart : Source::Hold;
    }
    if (!gpsUsable) {
      reset();
    } else if (!recovering_) {
      recovering_ = true;
      usableSinceMs_ = nowMs;
    } else if (nowMs - usableSinceMs_ >= kGpsRecoveryMs) {
      gpsReady_ = true;
    }
    if (gpsReady_) return Source::Gps;
    return Source::Hold;
  }

  bool gpsReady() const { return gpsReady_; }

 private:
  uint32_t usableSinceMs_ = 0;
  bool recovering_ = false;
  bool gpsReady_ = false;
};

inline const char *sourceName(Source source) {
  switch (source) {
    case Source::Gps: return "gps";
    case Source::Uart: return "uart";
    default: return "hold";
  }
}

}  // namespace tracking_policy
