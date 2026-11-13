#pragma once

#include <stdint.h>

#include "client_control.h"
#include "ble_control_protocol.h"

// Arduino-free control/replay policy for the T096 Client.  The radio driver
// decides when its preamble-gated receive window begins; this class decides what a
// decoded command is allowed to change.
namespace t096_control {

enum class CommandResult : uint8_t { Ignored, Applied, Duplicate };
enum class BleError : int16_t { None=0, Owner=1, Boot=2, Replay=3, Battery=4, Session=5, Channel=6, Client=7 };

inline bool elapsed(uint32_t now, uint32_t then, uint32_t interval) {
  return uint32_t(now - then) >= interval;
}

class Controller {
 public:
  explicit Controller(uint32_t boot) : boot_(boot) {}

  client_control::State state() const { return state_; }
  uint16_t station() const { return station_; }
  uint16_t command() const { return command_; }
  uint32_t stationBoot() const { return stationBoot_; }
  uint8_t channel() const { return channel_; }
  bool hasStation() const { return station_ != 0; }
  void requestReady() { state_ = client_control::State::Ready; }
  // Local USER operation needs no owner and never changes the remote command
  // sequence/session or channel. A later BLE Claim may adopt active operation.
  bool requestLocalMode(bool active, bool batteryProtected) {
    if (active && batteryProtected) return false;
    state_ = active ? client_control::State::Tracking : client_control::State::Ready;
    return true;
  }
  uint32_t pollWaitMs(uint32_t now) const {
    const uint32_t interval = state_ == client_control::State::Ready
                                 ? client_control::kReadyPollMs : client_control::kActivePollMs;
    const uint32_t age = uint32_t(now - lastStateMs_);
    return age >= interval ? 0 : interval - age;
  }
  bool pollDue(uint32_t now) const {
    return pollWaitMs(now) == 0;
  }
  bool probeDue(uint32_t now) const {
    return state_ == client_control::State::Test && elapsed(now, lastProbeMs_, 500);
  }
  void notedState(uint32_t now) { lastStateMs_ = now; }
  void notedProbe(uint32_t now) { lastProbeMs_ = now; }

  BleError rejectionReason(const ble_control::Command &in, bool batteryProtected) const {
    if (in.client != clientId_) return BleError::Client;
    if (in.boot != boot_) return BleError::Boot;
    if (!station_ || station_ != in.station) return BleError::Owner;
    if (in.op != ble_control::Op::Claim && in.stationBoot != stationBoot_) return BleError::Session;
    if ((in.op == ble_control::Op::Start || in.op == ble_control::Op::Test) && batteryProtected) return BleError::Battery;
    if ((in.op == ble_control::Op::Start || in.op == ble_control::Op::Test) &&
        state_ != client_control::State::Ready && in.channel != channel_) return BleError::Channel;
    return BleError::Replay;
  }

  CommandResult applyBle(const ble_control::Command &in, bool batteryProtected) {
    if (!ble_control::validCommand(in) || in.client != clientId_ || in.boot != boot_ ||
        (station_ && station_ != in.station)) return CommandResult::Ignored;
    if (in.op == ble_control::Op::Claim) {
      if (haveCommand_ && in.seq == command_)
        return lastAction_ == 0 && stationBoot_ == in.stationBoot ? CommandResult::Duplicate : CommandResult::Ignored;
      if (haveCommand_ && int16_t(in.seq - command_) <= 0) return CommandResult::Ignored;
      station_ = in.station; command_ = in.seq; haveCommand_ = true; lastAction_ = 0;
      stationBoot_ = in.stationBoot;
      return CommandResult::Applied;
    }
    if (!station_ || stationBoot_ != in.stationBoot) return CommandResult::Ignored;
    if (haveCommand_ && in.seq == command_) {
      const uint8_t action = in.op == ble_control::Op::Start ? 1 : in.op == ble_control::Op::Stop ? 2 : 3;
      const auto desired = action == 1 ? client_control::State::Tracking : action == 2 ? client_control::State::Ready : client_control::State::Test;
      return lastAction_ == action && state_ == desired &&
             (action == 2 || (!batteryProtected && in.channel == channel_))
                 ? CommandResult::Duplicate : CommandResult::Ignored;
    }
    if (haveCommand_ && int16_t(in.seq - command_) <= 0) return CommandResult::Ignored;
    if ((in.op == ble_control::Op::Start || in.op == ble_control::Op::Test) &&
        (batteryProtected || (state_ != client_control::State::Ready && in.channel != channel_)))
      return CommandResult::Ignored;
    if (in.op == ble_control::Op::Start || in.op == ble_control::Op::Test) {
      if (state_ == client_control::State::Ready) channel_ = in.channel;
      state_ = in.op == ble_control::Op::Start ? client_control::State::Tracking : client_control::State::Test;
    } else state_ = client_control::State::Ready;
    command_ = in.seq; haveCommand_ = true;
    lastAction_ = in.op == ble_control::Op::Start ? 1 : in.op == ble_control::Op::Stop ? 2 : 3;
    return CommandResult::Applied;
  }

  CommandResult apply(const PacketHeader &header, const client_control::Command &in,
                      uint32_t /*now*/) {
    if (!station_ || header.clientId != clientId_ || in.boot != boot_ || !client_control::valid(in.action))
      return CommandResult::Ignored;
    // First valid requester owns this boot session.  A Station change is only
    // possible after reboot, preventing two web stations from fighting.
    if (station_ && station_ != in.station) return CommandResult::Ignored;
    if (haveCommand_) {
      if (header.seq == command_ && in.station == station_) {
        const auto desired = in.action == client_control::Action::Start ? client_control::State::Tracking :
                             in.action == client_control::Action::Test ? client_control::State::Test : client_control::State::Ready;
        return lastAction_ == uint8_t(in.action) && state_ == desired ? CommandResult::Duplicate : CommandResult::Ignored;
      }
      // 16-bit command ids are serial numbers.  Equal was handled above;
      // values behind the current one must never replay state or the timer.
      if (int16_t(header.seq - command_) <= 0) return CommandResult::Ignored;
    }
    station_ = in.station;
    command_ = header.seq;
    lastAction_ = uint8_t(in.action);
    haveCommand_ = true;
    switch (in.action) {
      case client_control::Action::Start: state_ = client_control::State::Tracking; break;
      case client_control::Action::Stop: state_ = client_control::State::Ready; break;
      case client_control::Action::Test: state_ = client_control::State::Test; break;
      // Retain wire-v5 decoding for older Stations, but never enter SystemOFF.
      case client_control::Action::Store: state_ = client_control::State::Ready; break;
    }
    return CommandResult::Applied;
  }

  void setClientId(uint16_t id) { clientId_ = id; }

 private:
  uint32_t boot_;
  uint16_t clientId_ = 0;
  client_control::State state_ = client_control::State::Ready;
  uint16_t station_ = 0, command_ = 0;
  uint32_t stationBoot_ = 0;
  uint8_t channel_ = 0;
  uint8_t lastAction_ = 0;
  uint32_t lastStateMs_ = 0, lastProbeMs_ = 0;
  bool haveCommand_ = false;
};

}  // namespace t096_control
