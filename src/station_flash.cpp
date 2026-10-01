#if defined(ARDUINO) && defined(BOARD_HELTEC_V4)
#include <Arduino.h>
#include "station_flash.h"
#include "diagnostic_store.h"
#include "firmware_version.h"

namespace station_flash {
void begin(uint32_t boot) {
  diagnostic_store::begin(boot);
  const char record[]="{\"event\":\"station_flash_boot\",\"firmware\":\"" SHORE_SPOTTER_VERSION "\",\"board\":\"heltec-v4\",\"wire\":5}";
  diagnostic_store::submit(2,record,sizeof(record)-1,millis());
}
void packet(const packet_diagnostics::Event &e) {
  uint8_t bytes[15+sizeof(e.raw)];
  const size_t n=encodePacket(bytes,sizeof(bytes),e);
  if(n)diagnostic_store::submit(9,bytes,n,e.ms);
}
void serviceUsb() {
  static char line[96]; static size_t length=0;
  static bool ready=false,overflow=false;
  static uint32_t lastByte=0;
  if(ready) {
    if(diagnostic_store::transferActive())return;
    // Retain a completed command until the worker accepts it. Never let a
    // new USB line overwrite a pending read or erase.
    const bool recognized=!strncmp(line,"DIAG",4) && (line[4]==' ' || !line[4]);
    if(!diagnostic_store::command(overflow || !recognized?"DIAG INVALID":line))return;
    ready=false;overflow=false;length=0;
  }
  unsigned budget=128;
  while(!ready && Serial.available() && budget--) {
    const char c=char(Serial.read());lastByte=millis();
    if(c=='\r')continue;
    if(c=='\n') {
      line[length]=0;
      if(length || overflow)ready=true;
    } else if(!c || length+1>=sizeof(line))overflow=true;
    else line[length++]=c;
  }
  if(length && uint32_t(millis()-lastByte)>1000) { length=0;overflow=false; }
}
}
#endif
