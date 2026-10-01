#pragma once
#include <cmath>
#include <cstdint>
namespace phone_position {
struct Fix {
  double lat=0, lon=0, accuracy=0, utcMs=0;
  uint32_t receivedMs=0;
  bool enabled=false;
  bool set(double la,double lo,double ac,double utc,uint32_t now) {
    // Browser supplied time is metadata; no claim of clock synchronization.
    if(!std::isfinite(la)||!std::isfinite(lo)||!std::isfinite(ac)||!std::isfinite(utc)||
        la < -90 || la > 90 || lo < -180 || lo > 180 || ac < 0 || ac > 100000 ||
        utc < 1577836800000.0 || utc > 4102444800000.0 || (enabled && utc < utcMs)) return false;
    lat=la;lon=lo;accuracy=ac;utcMs=utc;receivedMs=now;enabled=true;return true;
  }
  // A stationary position remains usable while the phone browser is suspended.
  // Age is exposed separately; it must never pretend to be a live GNSS fix.
  uint32_t age(uint32_t now) const { return uint32_t(now-receivedMs); }
};
}
