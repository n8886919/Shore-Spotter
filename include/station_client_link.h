#pragma once
#include "client_control.h"
namespace station_client_link {
class Link {
 public:
  client_control::Status status{};
  uint16_t client=0, commandId=0;
  uint32_t receivedMs=0, commandMs=0, requestedBoot=0;
  bool have=false;
  const char *command="idle";
  client_control::Action action=client_control::Action::Stop;
  uint32_t probeBoot=0, probeCounter=0, probeMs=0, probes=0, missing=0, maxGapMs=0;
  float rssi=0, snr=0;
  int16_t lastError=0;
  bool pending()const{return command[0]=='p';}
  bool fresh(uint32_t now)const{return have && uint32_t(now-receivedMs)<90000;}
  void tick(uint32_t now) {
    if(pending() && uint32_t(now-commandMs)>=client_control::kCommandTimeoutMs) command="timeout";
  }
  void observe(uint16_t id,const client_control::Status &s,uint32_t now,uint16_t station) {
    if(have && (client!=id || status.boot!=s.boot)) {
      if(pending())command="error";
    }
    status=s;client=id;receivedMs=now;have=true;
    if(s.station==station && !pending())commandId=s.command;
    if(pending() && s.boot==requestedBoot && s.station==station && s.command==commandId) {
      const auto expected=action==client_control::Action::Start ? client_control::State::Tracking :
        action==client_control::Action::Test ? client_control::State::Test :
        action==client_control::Action::Store ? client_control::State::Storage : client_control::State::Ready;
      command=s.state==expected?"confirmed":"error";
    }
  }
  bool request(client_control::Action a,uint32_t now,uint16_t station) {
    tick(now);
    if(!fresh(now)||pending()||!client_control::valid(a)||
        (status.station && status.station!=station))return false;
    action=a;requestedBoot=status.boot;commandMs=now;command="pending";lastError=0;++commandId;
    if(a==client_control::Action::Test)resetProbes();
    return true;
  }
  void resetProbes(){probeBoot=probeCounter=probeMs=probes=missing=maxGapMs=0;rssi=snr=0;}
  bool probe(const client_control::Probe &p,uint32_t now,float rs,float sn) {
    if(p.boot!=probeBoot)resetProbes();
    if(probes) {
      const uint32_t delta=p.counter-probeCounter;
      if(!delta || delta>=0x80000000UL)return false;
      missing+=delta-1;
      const uint32_t gap=now-probeMs;if(gap>maxGapMs)maxGapMs=gap;
    }
    probeBoot=p.boot;probeCounter=p.counter;probeMs=now;++probes;rssi=rs;snr=sn;return true;
  }
};
}
