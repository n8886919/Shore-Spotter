#pragma once
#include <stdint.h>
#include <stddef.h>

// UC6580 / UFirebird II R1.5: supported navigation periods are 1000/200/100 ms.
// 100 ms navigation with NMEA divisor 5 yields 500 ms RMC/GGA output.
// UC6580 R6 Build3700 requires its third field to remain 1000 (bench readback).
// Do not transplant L76K PCAS commands, write receiver flash, or change constellations.
namespace t096_gnss {
constexpr const char *commands[] = {
  "$PDTINFO\r\n", "$CFGMSG,0,1,0\r\n", "$CFGMSG,0,2,0\r\n",
  "$CFGMSG,0,3,0\r\n", "$CFGMSG,0,5,0\r\n", "$CFGMSG,0,6,0\r\n",
  "$CFGMSG,0,7,0\r\n", "$CFGMSG,0,8,0\r\n",
  "$CFGNAV,100,100,1000\r\n", "$CFGMSG,0,0,5\r\n", "$CFGMSG,0,4,5\r\n",
  "$CFGMSG,0,3,0\r\n", // CFGNAV restores GSV; disable it again.
  "$CFGNAV\r\n", "$CFGMSG,0,0\r\n", "$CFGMSG,0,4\r\n"
};
class Setup {
 public:
  void begin(uint32_t now) { active_ = true; index_ = 0; next_ = now + 1500; }
  void stop() { active_ = false; }
  const char *due(uint32_t now) {
    if (!active_ || int32_t(now-next_) < 0) return nullptr;
    const char *out = commands[index_++]; next_ = now + 80;
    if (index_ == sizeof(commands)/sizeof(commands[0])) active_ = false;
    return out;
  }
 private:
  bool active_ = false;
  size_t index_ = 0;
  uint32_t next_ = 0;
};
}
