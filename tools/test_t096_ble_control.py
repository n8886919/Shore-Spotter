#!/usr/bin/env python3
"""Exercise the actual T096 ownership, replay, battery and channel policy."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = r'''
#include <cassert>
#include "t096_control.h"
using namespace ble_control;
using t096_control::CommandResult;
int main() {
  t096_control::Controller c(77); c.setClientId(42);
  PacketHeader h{42,1,MSG_CLIENT_CONTROL};
  client_control::Command l{77,client_control::Action::Start,9};
  assert(c.apply(h,l,0)==CommandResult::Ignored);
  Command q{Op::Claim,0,42,9,78,1,100};
  assert(c.applyBle(q,false)==CommandResult::Ignored && !c.hasStation());
  q.boot=77; assert(c.applyBle(q,false)==CommandResult::Applied);
  q.op=Op::Start; q.seq=2; q.channel=1;
  assert(c.applyBle(q,true)==CommandResult::Ignored && c.command()==1);
  assert(c.rejectionReason(q,true)==t096_control::BleError::Battery);
  assert(c.applyBle(q,false)==CommandResult::Applied && c.channel()==1 && c.state()==client_control::State::Tracking);
  assert(c.applyBle(q,false)==CommandResult::Duplicate);
  q.op=Op::Stop; assert(c.applyBle(q,false)==CommandResult::Ignored);
  q.seq=1; assert(c.applyBle(q,false)==CommandResult::Ignored);
  q.seq=3; q.station=10; assert(c.applyBle(q,false)==CommandResult::Ignored);
  assert(c.rejectionReason(q,false)==t096_control::BleError::Owner);
  q.station=9; q.op=Op::Test; q.channel=0;
  assert(c.applyBle(q,false)==CommandResult::Ignored && c.state()==client_control::State::Tracking);
  assert(c.rejectionReason(q,false)==t096_control::BleError::Channel);
  q.op=Op::Claim; q.stationBoot=200; q.seq=0;
  assert(c.applyBle(q,false)==CommandResult::Ignored && c.stationBoot()==100);
  q.seq=3; assert(c.applyBle(q,false)==CommandResult::Applied && c.command()==3 && c.state()==client_control::State::Tracking);
  assert(c.applyBle(q,false)==CommandResult::Duplicate);
  q.stationBoot=100; assert(c.applyBle(q,false)==CommandResult::Ignored && c.stationBoot()==200);
  q.stationBoot=200; q.op=Op::Stop; q.seq=4; assert(c.applyBle(q,false)==CommandResult::Applied && c.state()==client_control::State::Ready);
  q.op=Op::Test; q.seq=5; q.channel=0; assert(c.applyBle(q,false)==CommandResult::Applied && c.channel()==0);
  h.seq=6; l.action=client_control::Action::Stop;
  assert(c.apply(h,l,0)==CommandResult::Applied && c.state()==client_control::State::Ready);
  c.requestReady(); assert(c.station()==9);
  // Local mode leaves owner, channel, boot-session and sequence intact.
  assert(c.requestLocalMode(true,false) && c.command()==6 && c.station()==9 && c.channel()==0);
  assert(c.stationBoot()==200 && c.state()==client_control::State::Tracking);
  // An old Stop must not claim success after local ACT, through either transport.
  assert(c.apply(h,l,0)==CommandResult::Ignored && c.state()==client_control::State::Tracking);
  q.op=Op::Stop;q.seq=7;assert(c.applyBle(q,false)==CommandResult::Applied);
  assert(c.requestLocalMode(true,false));
  assert(c.applyBle(q,false)==CommandResult::Ignored && c.state()==client_control::State::Tracking);
  q.seq=8;assert(c.applyBle(q,false)==CommandResult::Applied);
  assert(!c.requestLocalMode(true,true) && c.state()==client_control::State::Ready);
  assert(c.requestLocalMode(false,true));
  // Local stop also rejects a stale Start confirmation; a new command still works.
  h.seq=9;l.action=client_control::Action::Start;
  assert(c.apply(h,l,0)==CommandResult::Applied);
  assert(c.requestLocalMode(false,false));
  assert(c.apply(h,l,0)==CommandResult::Ignored && c.state()==client_control::State::Ready);
  h.seq=10;assert(c.apply(h,l,0)==CommandResult::Applied);
  assert(c.requestLocalMode(false,false));
  q.seq=11;q.op=Op::Start;assert(c.applyBle(q,false)==CommandResult::Applied);
  assert(c.requestLocalMode(false,false));
  assert(c.applyBle(q,false)==CommandResult::Ignored && c.state()==client_control::State::Ready);
  q.seq=12;assert(c.applyBle(q,false)==CommandResult::Applied);
  // Standalone ACT does not synthesize an owner or consume command IDs.
  t096_control::Controller solo(99);solo.setClientId(42);
  assert(solo.requestLocalMode(true,false));
  assert(!solo.hasStation() && solo.command()==0 && solo.channel()==0);
  assert(solo.apply({42,1,MSG_CLIENT_CONTROL},{99,client_control::Action::Stop,9},0)==CommandResult::Ignored);
  Command claim{Op::Claim,1,42,9,99,1,300};
  assert(solo.applyBle(claim,false)==CommandResult::Applied);
  assert(solo.state()==client_control::State::Tracking && solo.channel()==0 && solo.stationBoot()==300);
  claim.station=10;claim.seq=2;assert(solo.applyBle(claim,false)==CommandResult::Ignored);
  claim.station=9;claim.op=Op::Stop;
  assert(solo.applyBle(claim,false)==CommandResult::Applied && solo.state()==client_control::State::Ready);
  claim.op=Op::Start;claim.seq=3;
  assert(solo.applyBle(claim,false)==CommandResult::Applied && solo.channel()==1);
  t096_control::Controller reboot(78); reboot.setClientId(42); assert(!reboot.hasStation());
}
'''
with tempfile.TemporaryDirectory() as td:
    p = Path(td) / 'test.cpp'; p.write_text(source)
    binary = Path(td) / 'test'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-I', str(root / 'include'), str(p), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS T096 BLE ownership, sequence, mode, battery and channel policy')
