#pragma once
#include "packet_diagnostics.h"
#include "diagnostic_store_codec.h"

namespace station_flash {
// Record kind 9: version, event kind, RX length, RSSI/10, SNR/4, error,
// event id, raw length, then exact bounded RF bytes. Explicit endian layout.
inline size_t encodePacket(uint8_t *out, size_t cap, const packet_diagnostics::Event &e) {
  if(e.rawLength>sizeof(e.raw) || cap<15+size_t(e.rawLength))return 0;
  using namespace diagnostic_store::codec;
  out[0]=1;out[1]=uint8_t(e.kind);put16(out+2,e.length);
  put16(out+4,uint16_t(e.rssiDbm10));put16(out+6,uint16_t(e.snrQuarterDb));
  put16(out+8,uint16_t(e.code));put32(out+10,e.id);out[14]=e.rawLength;
  memcpy(out+15,e.raw,e.rawLength);return 15+e.rawLength;
}
#if defined(ARDUINO) && defined(BOARD_HELTEC_V4)
void begin(uint32_t boot);
void packet(const packet_diagnostics::Event &event);
void serviceUsb();
#endif
}
