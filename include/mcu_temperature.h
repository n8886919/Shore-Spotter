#pragma once
#include <stdint.h>
#include <math.h>

namespace mcu_temperature {
constexpr uint32_t kSampleMs=30000, kValidMs=60000, kTimeoutMs=10;
// Only called while the existing main loop is awake. No sampling wake timer.
struct Sample {
  bool attempted=false, valid=false, pending=false, haveMaximum=false;
  uint32_t attemptMs=0, sampleMs=0, failures=0;
  int32_t quarterC=0, maximumQuarterC=0;
  bool due(uint32_t now) const { return !pending && (!attempted || now-attemptMs>=kSampleMs); }
  void start(uint32_t now) { attempted=true;valid=false;pending=true;attemptMs=now; }
  void finish(uint32_t now, bool ok, int32_t value) {
    pending=false;valid=ok && value>=-160 && value<=340;
    if(!valid) { ++failures;return; }
    quarterC=value;sampleMs=now;
    if(!haveMaximum || value>maximumQuarterC) maximumQuarterC=value;
    haveMaximum=true;
  }
  int8_t wire(uint32_t now) const {
    return valid && now-sampleMs<kValidMs ? int8_t(lround(quarterC/4.0)) : INT8_MIN;
  }
};
// Station's maximum is observed telemetry, not the MCU's unsent peak.
struct Received {
  bool have=false, haveMaximum=false;
  int8_t current=INT8_MIN, maximum=INT8_MIN;
  uint32_t rxMs=0;
  void observe(int8_t value, uint32_t now) {
    have=true;current=value;rxMs=now;
    if(value!=INT8_MIN) { if(!haveMaximum || value>maximum)maximum=value;haveMaximum=true; }
  }
  bool fresh(uint32_t now) const { return have && current!=INT8_MIN && now-rxMs<kValidMs; }
};
} // namespace mcu_temperature
