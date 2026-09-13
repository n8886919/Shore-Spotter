#if defined(ROLE_SERVER)
#include "uart_servo_mode.h"

namespace uart_servo_mode {
namespace {
constexpr size_t kMaximumSerialBytesPerPoll = 160;
}

Endpoint::Endpoint() : serial_(2) {}

void Endpoint::enter(uint32_t epoch) {
  leave();
  gate_.reset(epoch);
  serial_.setRxBufferSize(256);
  serial_.begin(kSerialBaud, SERIAL_8N1, kSerialRxPin, kSerialTxPin);
  active_ = true;
  lastServiceMs_ = millis();
}

void Endpoint::leave() {
  if (active_) serial_.end();
  active_ = false;
  mailbox_ = uart_target::Mailbox{};
  sequenced_ = false;
  parser_.reset();
  lastServiceMs_ = 0;
}

void Endpoint::poll() {
  if (!active_) return;
  const uint32_t now = millis();
  mailbox_.expire(now);
  if (now - lastServiceMs_ >= servo_profile::kWatchdogMs) {
    ++latePolls_;
    mailbox_.hold();
    const int queued = serial_.available();
    for (int i = 0; i < queued; ++i) {
      if (serial_.read() >= 0) ++discardedBytes_;
    }
    // The buffer may end mid-line. Require a boundary before accepting new SET.
    parser_.reset(true);
    lastServiceMs_ = now;
    return;
  }
  lastServiceMs_ = now;
  size_t serviced = 0;
  while (serial_.available() > 0 && serviced++ < kMaximumSerialBytesPerPoll) {
    const int raw = serial_.read();
    if (raw < 0) break;
    int32_t angleMdeg = 0;
    const uint32_t receivedMs = millis();
    const auto result = parser_.push(static_cast<char>(raw), receivedMs, angleMdeg);
    if (result == uart_set_parser::Result::Target) {
      const auto &frame = parser_.frame();
      if (frame.sequenced) {
        if (!gate_.accept(frame.epoch, frame.seq, frame.stamp, receivedMs,
                          servo_profile::kWatchdogMs)) { ++rejectedCommands_; continue; }
        sequenced_ = true;
        mailbox_.set(angleMdeg, frame.stamp);
      } else if (!sequenced_) {
        mailbox_.set(angleMdeg, receivedMs);
      } else ++rejectedCommands_;
    } else if (result == uart_set_parser::Result::Sync) {
      char reply[64];
      const int size = snprintf(reply, sizeof(reply), "SESSION %lu %lu %lu\n",
          static_cast<unsigned long>(gate_.epoch()), static_cast<unsigned long>(gate_.sequence()),
          static_cast<unsigned long>(receivedMs));
      if (size > 0 && size < static_cast<int>(sizeof(reply)) && serial_.availableForWrite() >= size)
        serial_.write(reinterpret_cast<const uint8_t *>(reply), size);
    } else if (result == uart_set_parser::Result::Rejected) {
      ++rejectedCommands_;
    }
  }
}

}  // namespace uart_servo_mode
#endif
