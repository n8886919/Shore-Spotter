#pragma once
#include <atomic>
#include <stdint.h>
#include "radio_profile.h"

// One atomic snapshot for the main-loop producer and SD worker. A log session
// records the profile at file-open time; channel changes are separate events.
namespace runtime_radio_profile {
struct Profile { float frequencyMhz; uint8_t sf; };
inline uint8_t initialSf() {
#if defined(ROLE_STATION)
  return 9;
#else
  return radio_profile::spreadingFactor;
#endif
}
inline std::atomic<uint32_t> &word() {
  static std::atomic<uint32_t> value{(uint32_t(initialSf())<<24)|uint32_t(radio_profile::frequencyMhz*1000.0f+0.5f)};
  return value;
}
inline void set(float frequencyMhz,uint8_t sf) {
  word().store((uint32_t(sf)<<24)|uint32_t(frequencyMhz*1000.0f+0.5f),std::memory_order_relaxed);
}
inline Profile get() {
  const uint32_t value=word().load(std::memory_order_relaxed);
  return {float(value&0xffffff)/1000.0f,uint8_t(value>>24)};
}
}
