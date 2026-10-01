#pragma once
#include "protocol.h"

// Optional v5 extensions; old DATA/GNSS and telemetry layouts are unchanged.
// Target boot + requesting station + sequence identify a command and its reply.
// This is replay/freshness separation, not cryptographic authentication.
namespace client_control {
enum class Action : uint8_t { Start = 1, Stop = 2, Test = 3, Store = 4 };
enum class State : uint8_t { Ready = 1, Tracking = 2, Test = 3, Storage = 4 };
constexpr uint32_t kReadyPollMs = 30000, kActivePollMs = 5000;
constexpr uint32_t kReceiveWindowMs = 800;
constexpr uint32_t kStorageAfterMs = 12UL * 60 * 60 * 1000;
constexpr uint32_t kCommandTimeoutMs = 65000;
inline bool valid(Action a) { return uint8_t(a) >= 1 && uint8_t(a) <= 4; }
inline bool valid(State s) { return uint8_t(s) >= 1 && uint8_t(s) <= 4; }
inline const char *name(State s) {
  switch(s) { case State::Ready:return "ready"; case State::Tracking:return "tracking";
    case State::Test:return "test"; case State::Storage:return "storage"; }
  return "unknown";
}
inline void put32(uint8_t *p, uint32_t x) {
  for (unsigned i=0;i<4;++i) p[i]=uint8_t(x>>(8*i));
}
inline uint32_t get32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
struct Command { uint32_t boot=0; Action action=Action::Stop; uint16_t station=0; };
struct Status {
  uint32_t boot=0; State state=State::Ready; uint16_t station=0, command=0;
  uint32_t uptime=0; uint16_t batteryMv=0; uint32_t txPackets=0;
  uint16_t rxErrors=0; bool charging=false;
};
struct Probe { uint32_t boot=0, counter=0, uptime=0; };
inline size_t encodeCommand(uint8_t *b,size_t cap,const PacketHeader &h,const Command &c) {
  if(!protocol::canEncode(b,cap,h,MSG_CLIENT_CONTROL)||!c.boot||!valid(c.action)||
      !protocol::validClientId(c.station))return 0;
  protocol::encodeHeader(b,h);put32(b+6,c.boot);b[10]=uint8_t(c.action);
  protocol::putU16(b+11,c.station);return CLIENT_CONTROL_PACKET_LEN;
}
inline bool decodeCommand(const uint8_t *b,size_t n,PacketHeader &h,Command &c) {
  PacketHeader p{};if(!protocol::decodeHeader(b,n,p)||p.msgType!=MSG_CLIENT_CONTROL)return false;
  Command v{get32(b+6),Action(b[10]),protocol::getU16(b+11)};
  if(!v.boot||!valid(v.action)||!protocol::validClientId(v.station))return false;
  h=p;c=v;return true;
}
inline size_t encodeStatus(uint8_t *b,size_t cap,const PacketHeader &h,const Status &s) {
  if(!protocol::canEncode(b,cap,h,MSG_CLIENT_STATE)||!s.boot||!valid(s.state))return 0;
  protocol::encodeHeader(b,h);put32(b+6,s.boot);b[10]=uint8_t(s.state);
  protocol::putU16(b+11,s.station);protocol::putU16(b+13,s.command);put32(b+15,s.uptime);
  protocol::putU16(b+19,s.batteryMv);put32(b+21,s.txPackets);protocol::putU16(b+25,s.rxErrors);
  b[27]=s.charging?1:0;return CLIENT_STATE_PACKET_LEN;
}
inline bool decodeStatus(const uint8_t *b,size_t n,PacketHeader &h,Status &s) {
  PacketHeader p{};if(!protocol::decodeHeader(b,n,p)||p.msgType!=MSG_CLIENT_STATE)return false;
  Status v{get32(b+6),State(b[10]),protocol::getU16(b+11),protocol::getU16(b+13),
    get32(b+15),protocol::getU16(b+19),get32(b+21),protocol::getU16(b+25),b[27]!=0};
  if(!v.boot||!valid(v.state)||b[27]>1||v.station==ID_BROADCAST)return false;
  h=p;s=v;return true;
}
inline size_t encodeProbe(uint8_t *b,size_t cap,const PacketHeader &h,const Probe &p) {
  if(!protocol::canEncode(b,cap,h,MSG_LINK_TEST)||!p.boot)return 0;
  protocol::encodeHeader(b,h);put32(b+6,p.boot);put32(b+10,p.counter);put32(b+14,p.uptime);
  return LINK_TEST_PACKET_LEN;
}
inline bool decodeProbe(const uint8_t *b,size_t n,PacketHeader &h,Probe &p) {
  PacketHeader v{};if(!protocol::decodeHeader(b,n,v)||v.msgType!=MSG_LINK_TEST||!get32(b+6))return false;
  h=v;p={get32(b+6),get32(b+10),get32(b+14)};return true;
}
} // namespace client_control
