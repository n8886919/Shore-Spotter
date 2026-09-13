#pragma once
#include <stdint.h>
#include "loop_metrics.h"

namespace control_cadence {
// Only GPS target prediction is periodic. Servo output is never gated here.
constexpr uint32_t kGpsPeriodMs=50;
struct GpsCadence {
  bool poll(uint32_t now) {
    if(!started_) {started_=true;next_=now+kGpsPeriodMs;return false;}
    if(!loop_metrics::due(now,next_))return false;
    next_=now+kGpsPeriodMs;return true;
  }
 private:
  bool started_=false;uint32_t next_=0;
};
}
