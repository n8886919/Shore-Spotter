#pragma once

#include <stdint.h>

#include "client_control.h"

// Arduino-free control/replay policy for the T096 Client.  The radio driver
// decides when its preamble-gated receive window begins; this class decides what a
// decoded command is allowed to change.
namespace t096_control {

enum class CommandResult : uint8_t { Ignored, Applied, Duplicate };

inline bool elapsed(uint32_t now, uint32_t then, uint32_t interval) {
  return uint32_t(now - then) >= interval;
}

class Controller {
 public:
  explicit Controller(uint32_t boot) : boot_(boot) {}

  client_control::State state() const { return state_; }
  uint16_t station() const { return station_; }
  uint16_t command() const { return command_; }
  bool hasStation() const { return station_ != 0; }
  bool storageDue(uint32_t now, bool charging) const {
    return !charging && state_ == client_control::State::Ready &&
           elapsed(now, readySince_, client_control::kStorageAfterMs);
  }
  void requestStorage() { state_ = client_control::State::Storage; }
  void cancelStorage(uint32_t now) {
    if (state_ == client_control::State::Storage) {
      state_ = client_control::State::Ready;
      readySince_ = now;
    }
  }
  bool pollDue(uint32_t now) const {
    return elapsed(now, lastStateMs_, state_ == client_control::State::Ready
                                          ? client_control::kReadyPollMs
                                          : client_control::kActivePollMs);
  }
  bool probeDue(uint32_t now) const {
    return state_ == client_control::State::Test && elapsed(now, lastProbeMs_, 500);
  }
  void notedState(uint32_t now) { lastStateMs_ = now; }
  void notedProbe(uint32_t now) { lastProbeMs_ = now; }

  CommandResult apply(const PacketHeader &header, const client_control::Command &in,
                      uint32_t now) {
    if (header.clientId != clientId_ || in.boot != boot_ || !client_control::valid(in.action))
      return CommandResult::Ignored;
    // First valid requester owns this boot session.  A Station change is only
    // possible after reboot, preventing two web stations from fighting.
    if (station_ && station_ != in.station) return CommandResult::Ignored;
    if (haveCommand_) {
      if (header.seq == command_ && in.station == station_) return CommandResult::Duplicate;
      // 16-bit command ids are serial numbers.  Equal was handled above;
      // values behind the current one must never replay state or the timer.
      if (int16_t(header.seq - command_) <= 0) return CommandResult::Ignored;
    }
    station_ = in.station;
    command_ = header.seq;
    haveCommand_ = true;
    switch (in.action) {
      case client_control::Action::Start: state_ = client_control::State::Tracking; break;
      case client_control::Action::Stop: state_ = client_control::State::Ready; readySince_ = now; break;
      case client_control::Action::Test: state_ = client_control::State::Test; break;
      case client_control::Action::Store: state_ = client_control::State::Storage; break;
    }
    return CommandResult::Applied;
  }

  void resetReadyTimer(uint32_t now) { readySince_ = now; }
  void setClientId(uint16_t id) { clientId_ = id; }

 private:
  uint32_t boot_;
  uint16_t clientId_ = 0;
  client_control::State state_ = client_control::State::Ready;
  uint16_t station_ = 0, command_ = 0;
  uint32_t readySince_ = 0, lastStateMs_ = 0, lastProbeMs_ = 0;
  bool haveCommand_ = false;
};

}  // namespace t096_control
