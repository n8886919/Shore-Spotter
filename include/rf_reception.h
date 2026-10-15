#pragma once

#include <stdint.h>
#include "geo_math.h"

// Only observe accepted, bound-client packets. Position freshness is separate.
namespace rf_reception {
class Latest {
 public:
  void observe(uint32_t now, float rssi, float snr) {
    have_ = true; at_ = now; rssi_ = rssi; snr_ = snr;
  }
  bool fresh(uint32_t now) const { return have_ && uint32_t(now-at_) < 90000; }
  geo::SigLevel signal(uint32_t now) const {
    return fresh(now) ? geo::loraSignal(rssi_, snr_) : geo::SIG_MISS;
  }
  float rssi() const { return rssi_; }
  float snr() const { return snr_; }
 private:
  bool have_ = false;
  uint32_t at_ = 0;
  float rssi_ = 0, snr_ = 0;
};
}
