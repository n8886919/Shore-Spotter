#pragma once
#include <stddef.h>
#include <stdint.h>
#include "axiom_log.h"
#include "client_sd_record.h"
#if defined(ARDUINO)
#include <Arduino.h>
#endif

// SD owns a separate SPI bus and worker. Producers only copy into bounded,
// zero-wait queues; no filesystem or JSON work runs in the radio/control loop.
namespace sd_log {
constexpr size_t kPacketQueue = 32, kTextQueue = 16, kSampleQueue = 2;
constexpr size_t kBatchBytes = 4096, kEncodeBytes = 8192;
constexpr uint32_t kSyncMs = 5000, kRotateBytes = 32 * 1024 * 1024;
struct Packet {
  packet_diagnostics::Event event;
  uint16_t rawLength = 0;
  uint8_t raw[255]{};
};
struct Text { uint32_t ms = 0; uint16_t length = 0; char bytes[256]{}; };
size_t encodePacket(char *out, size_t capacity, uint32_t boot, const Packet &packet);
size_t encodeText(char *out, size_t capacity, uint32_t boot, const Text &text);

#if defined(ARDUINO)
using PowerSwitch = bool (*)(bool on);
void begin(uint32_t boot, bool powerReady, PowerSwitch power = nullptr);
bool start();
void stop();
bool stopped();
bool captureDue(uint32_t now, bool busy);
void submit(const axiom_log::Sample &sample, uint32_t captureStartedUs);
void packet(const packet_diagnostics::Event &event, const uint8_t *raw, size_t length);
void text(const uint8_t *bytes, size_t length, uint32_t ms);
String statusJson();
const char *stateName();
void serviceUsb();
bool usbTransferActive();
// Main-loop producers: bounded copies / zero-wait queues only.
void clientGps(bool allowed, const ClientRecord &record);
void clientEvent(const ClientRecord &record);
#endif
}
