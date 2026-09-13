#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace servo_motion {

constexpr double kDefaultSpeed=30;
constexpr double kMinimumSpeed=1;
constexpr double kMaximumSpeed=90;
constexpr uint32_t kMaximumStepUs=50000;
// A single atomic NVS value, in millidegrees/second. Old motioncfg profiles are
// intentionally not migrated: the simplified controller starts at 30 deg/s.
constexpr const char *kSpeedKey="servospd";
inline bool validSpeed(double speed) {
  return std::isfinite(speed) && speed>=kMinimumSpeed && speed<=kMaximumSpeed;
}
inline uint32_t encodeSpeed(double speed) {
  return static_cast<uint32_t>(std::lround(speed*1000));
}
inline bool decodeSpeed(uint32_t saved,double &speed) {
  const double value=saved/1000.0;
  if(!validSpeed(value))return false;
  speed=value;return true;
}

// One elapsed-time rate limiter for every source. No acceleration, jerk,
// deadband, per-mode settings or fixed output cadence.
class Controller {
 public:
  explicit Controller(double held=90):position_(held),requested_(held) {}
  bool setSpeed(double speed) {
    if(!validSpeed(speed))return false;
    speed_=speed;return true;  // a live speed change preserves the latest target
  }
  bool target(double angle) {
    if(fault_ || !std::isfinite(angle) || angle<0 || angle>180)return false;
    requested_=std::round(angle*1000)/1000.0;
    return true;
  }
  void holdUs(uint32_t nowUs) {
    requested_=position_;velocity_=0;lastTickUs_=nowUs;
  }
  void faultUs(uint32_t nowUs) {holdUs(nowUs);fault_=true;}
  void initializeUs(double held,uint32_t nowUs) {
    if(!std::isfinite(held) || held<0 || held>180) {faultUs(nowUs);return;}
    position_=held;fault_=false;holdUs(nowUs);
  }
  bool tickUs(uint32_t nowUs) {
    const uint32_t dtUs=std::min(nowUs-lastTickUs_,kMaximumStepUs);
    lastTickUs_=nowUs;
    if(fault_ || !dtUs)return false;
    const double previous=position_,error=requested_-position_;
    const double step=speed_*dtUs/1000000.0;
    if(std::abs(error)<=step) {position_=requested_;velocity_=0;}
    else {velocity_=std::copysign(speed_,error);position_+=std::copysign(step,error);}
    return position_!=previous;
  }
  double speed() const {return speed_;}
  double position() const {return position_;}
  double requested() const {return requested_;}
  double velocity() const {return velocity_;}
  bool moving() const {return !fault_ && requested_!=position_;}
  bool faulted() const {return fault_;}
 private:
  double position_,requested_,speed_=kDefaultSpeed,velocity_=0;
  uint32_t lastTickUs_=0;
  bool fault_=false;
};
}  // namespace servo_motion
