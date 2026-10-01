#pragma once

#include <stdint.h>
#include "geo_math.h"

namespace tracking_policy {

constexpr uint32_t kGpsFreshMs = 2000;

enum class Source : uint8_t { Hold, Gps, Uart };
enum class Mode : uint8_t { Manual, Gps, Uart, Paused };

inline bool usableGps(bool fix, int /*satellites*/, float /*hdop*/, uint32_t ageMs) {
  return fix && ageMs < kGpsFreshMs;
}
// Quality controls optional extrapolation, not direct position tracking.
inline bool predictionQuality(int satellites, float hdop) {
  return satellites >= 6 && satellites != 255 && isfinite(hdop) && hdop >= 0 && hdop <= 3.0f;
}

// GPS and UART are mutually exclusive, explicitly selected modes. A bad
// signal holds in the selected mode and must never activate the other source.
class Selector {
 public:
  void reset() { gpsReady_ = false; }

  Source update(uint32_t /*nowMs*/, Mode mode, bool gpsUsable, bool uartReady) {
    // Quality and age checks already determine usability. A newly valid fix
    // may resume GPS immediately, without an additional recovery timer.
    gpsReady_ = mode == Mode::Gps && gpsUsable;
    if (gpsReady_) return Source::Gps;
    return mode == Mode::Uart && uartReady ? Source::Uart : Source::Hold;
  }

  bool gpsReady() const { return gpsReady_; }

 private:
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
