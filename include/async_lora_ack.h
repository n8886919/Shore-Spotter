#pragma once
#include <stddef.h>
#include <stdint.h>

namespace async_lora_ack {
enum class Event : uint8_t { None, Started, Sent, Failed, Timeout, Cancelled };
struct Result {
  Event event = Event::None;
  int16_t txStatus = 0;
  int16_t rxStatus = 0;
};

// Radio uses the RadioLib startTransmit/getIrqFlags/finishTransmit/startReceive
// interface. Keeping the lifecycle here lets native tests drive the real logic
// with a fake radio, including IRQ ordering and failure recovery.
template <typename Radio>
class Transmitter {
 public:
  Transmitter(Radio &radio, volatile bool &irq, uint32_t txDoneMask,
              uint32_t timeoutMask, int16_t timeoutError)
      : radio_(radio), irq_(irq), txDoneMask_(txDoneMask),
        timeoutMask_(timeoutMask), timeoutError_(timeoutError) {}

  bool active() const { return active_; }

  Result start(const uint8_t *data, size_t length, uint32_t nowMs,
               uint32_t timeoutMs) {
    if (active_) return {};  // no ACK queue and no overwriting a transmission
    irq_ = false;
    startedMs_ = nowMs;
    timeoutMs_ = timeoutMs;
    active_ = true;
    const int16_t status = radio_.startTransmit(data, length);
    if (status != 0) return finish(Event::Failed, status);
    Result result;
    result.event = Event::Started;
    return result;
  }

  Result service(uint32_t nowMs) {
    if (!active_) return {};
    const bool expired = nowMs - startedMs_ >= timeoutMs_;
    if (!irq_ && !expired) return {};
    irq_ = false;
    const uint32_t flags = radio_.getIrqFlags();
    // TxDone wins if loop service was delayed past the software timeout.
    if (flags & txDoneMask_) return finish(Event::Sent, 0);
    if (expired || (flags & timeoutMask_)) return finish(Event::Timeout, timeoutError_);
    return {};  // an unrelated IRQ must never be parsed as a received packet
  }

  Result cancel() {
    return active_ ? finish(Event::Cancelled, 0) : Result{};
  }

 private:
  Result finish(Event event, int16_t status) {
    active_ = false;
    const int16_t finished = radio_.finishTransmit();
    if (status == 0 && finished != 0) {
      event = Event::Failed;
      status = finished;
    }
    // Clear the old TX interrupt BEFORE restarting RX. A fresh RxDone raised
    // during startReceive must remain available for the main loop to consume.
    irq_ = false;
    Result result;
    result.event = event;
    result.txStatus = status;
    result.rxStatus = radio_.startReceive();
    return result;
  }

  Radio &radio_;
  volatile bool &irq_;
  uint32_t txDoneMask_;
  uint32_t timeoutMask_;
  int16_t timeoutError_;
  uint32_t startedMs_ = 0;
  uint32_t timeoutMs_ = 0;
  bool active_ = false;
};
}  // namespace async_lora_ack
