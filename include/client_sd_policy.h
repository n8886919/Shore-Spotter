#pragma once
#include "gnss_snapshot.h"
#include "geo_math.h"
#include "client_cadence.h"
#include "tracking_policy.h"

namespace client_sd {
// Only the logger is gated. Never use this policy to change RF or GNSS cadence.
class Gate {
 public:
  bool observe(uint32_t now, const gnss_snapshot::Snapshot &s) {
    if (!s.haveEpoch || !s.fix || s.arrivalAgeMs >= tracking_policy::kGpsFreshMs ||
        !isfinite(s.lat) || !isfinite(s.lon) || s.lat < -90 || s.lat > 90 || s.lon < -180 || s.lon > 180)
      return allowed_ = false;
    if (!seen_ || epoch_ != s.epochMsOfDay) {
      seen_ = true; epoch_ = s.epochMsOfDay; observed_ = now;
    }
    // Do not power-cycle the card between the RMC and GGA of each new epoch.
    if (!(s.haveRmc && s.haveGga) && now - observed_ < client_cadence::kPairWaitMs)
      return allowed_;
    const auto quality = geo::gpsSignal(true, s.satellites == 255 ? 0 : s.satellites, s.hdop);
    return allowed_ = s.haveGga && isfinite(s.hdop) && s.hdop >= 0 &&
        (quality == geo::SIG_GOOD || quality == geo::SIG_OK);
  }
 private:
  bool allowed_ = false, seen_ = false;
  uint32_t epoch_ = 0, observed_ = 0;
};
}
