#pragma once

#if defined(ROLE_SERVER)
#include <Arduino.h>
#include <HardwareSerial.h>
#include "uart_target.h"
#include "uart_set_parser.h"

namespace uart_servo_mode {

// UART is opened only in UART mode; leaving it discards its target and input.
class Endpoint {
 public:
  Endpoint();
  void enter(uint32_t epoch);
  void leave();
  void poll();
  bool active() const { return active_; }
  bool ready() const { return active_ && mailbox_.ready(); }
  uint32_t latePolls() const { return latePolls_; }
  uint32_t discardedBytes() const { return discardedBytes_; }
  uint32_t rejectedCommands() const { return rejectedCommands_; }
  int32_t targetMdeg() const { return mailbox_.target(); }
  const char *stateName() const {
    if (!active_) return "inactive";
    return mailbox_.state();
  }

 private:
  static constexpr uint32_t kSerialBaud = 115200;
  static constexpr int kSerialRxPin = 44;
  static constexpr int kSerialTxPin = 43;
  HardwareSerial serial_;
  uart_target::Mailbox mailbox_;
  command_freshness::HttpGate gate_;
  bool sequenced_ = false;
  bool active_ = false;
  uart_set_parser::Parser parser_;
  uint32_t lastServiceMs_ = 0;
  uint32_t latePolls_ = 0;
  uint32_t discardedBytes_ = 0;
  uint32_t rejectedCommands_ = 0;
};
}  // namespace uart_servo_mode
#endif
