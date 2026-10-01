#if defined(ARDUINO) && defined(BOARD_HELTEC_V4)
#include "sd_log.h"

// Heltec V4 has no SD hardware. Keep Station logging call sites intact, but
// report capability truthfully instead of presenting a failed mount as an error.
namespace sd_log {
void begin(uint32_t, bool, PowerSwitch) {}
bool start() { return false; }
void stop() {}
bool stopped() { return true; }
bool captureDue(uint32_t, bool) { return false; }
void submit(const axiom_log::Sample &, uint32_t) {}
void packet(const packet_diagnostics::Event &, const uint8_t *, size_t) {}
void text(const uint8_t *, size_t, uint32_t) {}
String statusJson() {
  return F("{\"supported\":false,\"present\":false,\"recording\":false,\"state\":\"unsupported\",\"errors\":0,\"error\":\"\"}");
}
const char *stateName() { return "unsupported"; }
void serviceUsb() {}
bool usbTransferActive() { return false; }
void clientGps(bool, const ClientRecord &) {}
void clientEvent(const ClientRecord &) {}
}  // namespace sd_log
#endif
