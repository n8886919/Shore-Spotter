// T096 standalone nRF52840 Client: no ESP Web/Wi-Fi and no fake GNSS.
#include <Arduino.h>
#include <stdarg.h>
#include <algorithm>
#include <RadioLib.h>
#include "t096_display.h"
#include "client_cadence.h"
#include "client_control.h"
#include "firmware_version.h"
#include "gnss_snapshot.h"
#include "gnss_rate.h"
#include "gnss_diagnostics.h"
#include "t096_gnss_config.h"
#include "protocol.h"
#include "radio_profile.h"
#include "t096_control.h"
#include "t096_pins.h"
#include "t096_radio_config.h"
#include "t096_power.h"
#include "t096_idle.h"
#include "geo_math.h"
#include "tracking_policy.h"

namespace {
constexpr float kFrequencyMhz = radio_profile::frequencyMhz, kBandwidthKhz = radio_profile::bandwidthKhz;
constexpr uint8_t kSf = radio_profile::spreadingFactor, kCr = radio_profile::codingRate, kSync = radio_profile::syncWord;
constexpr uint32_t kTxTimeoutMs = 3000;
Module module(t096_pins::kLoraNss, t096_pins::kLoraDio1, t096_pins::kLoraReset, t096_pins::kLoraBusy);
SX1262 radio(&module);
gnss_snapshot::Collector gnss(115200);
client_cadence::Scheduler cadence;
gnss_rate::Monitor gpsRate;
t096_gnss::Setup gnssSetup;
bool gnssEnabled = false, rxPreamble = false, dataWaiting = false;
uint32_t gpsLastServiceMs = 0, gpsBacklogDrops = 0, nextTxStartMs = 0, nextExtraMs = 0;
uint32_t txErrors = 0, dataDeferred = 0;
uint8_t extraKind = 0;
uint16_t dataSequence = 0;
volatile bool radioIrq = false;
TaskHandle_t loopTask = nullptr;
void notifyLoopFromIsr() {
  if (!loopTask) return;
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(loopTask, &woken);
  portYIELD_FROM_ISR(woken);
}
void onRadioIrq() { radioIrq = true; notifyLoopFromIsr(); }
void onUserEdge() { notifyLoopFromIsr(); }
enum class AirState : uint8_t { Idle, Sending, Receiving };
AirState air = AirState::Idle;
bool confirmationPending = false, outOpensReceiveWindow = false, outIsState = false;
bool radioSleeping = false;
uint8_t out[MAX_PACKET_LEN] = {};
size_t outLength = 0;
uint16_t txSequence = 0, rxErrors = 0, thisClientId = 0;
uint32_t txPackets = 0, probeCounter = 0, airStartedMs = 0, thisBoot = 1;
uint32_t nextRadioRetryMs = 0;
bool radioReady = false, usbIdentityAnnounced = false;
uint32_t idleWaitCalls = 0, idleBlockedMs = 0;
t096_control::Controller control(1);
SPIClass displaySpi(NRF_SPIM2, t096_pins::kTftUnusedMiso, t096_pins::kTftSck, t096_pins::kTftMosi);
T096Display tft(&displaySpi, t096_pins::kTftNss, t096_pins::kTftDc, t096_pins::kTftReset);
GFXcanvas16 displayLine(160,16); // 5 KiB row buffer: glyph drawing never performs SPI pixel-by-pixel
t096_power::Battery battery;
t096_power::Button userButton;
bool displayPowered = false, displaySleeping = false, displayWaking = false;
bool batterySampling = false, batteryProtection = false;
uint32_t screenUntilMs = 0, displayWakeMs = 0, nextDisplayMs = 0, displayMaxMs = 0;
uint32_t batterySampleMs = 0, nextBatteryMs = 0;
uint8_t displayRow = 0;

// TinyUSB CDC write() waits while a connected host stops draining its FIFO.
// A console must never stall GNSS/radio; drop the whole bounded line instead.
uint32_t usbLogDropped=0;
void usbLog(const char *format, ...) {
  if(!Serial)return;
  char line[240];va_list args;va_start(args,format);
  const int n=vsnprintf(line,sizeof(line),format,args);va_end(args);
  if(n<=0)return;
  if(size_t(n)>=sizeof(line) || Serial.availableForWrite()<n) { ++usbLogDropped;return; }
  Serial.write(reinterpret_cast<const uint8_t*>(line),size_t(n));
}
void setGnssEnabled(bool enabled);
void wakeClientDisplay(uint32_t now);
void printIdentity(const char *prefix) {
  usbLog("%s T096 v%s client=%04X boot=%08lX RF group=%u %.1fMHz SF%u drive=%ddBm\n",
                prefix, SHORE_SPOTTER_VERSION, thisClientId,
                static_cast<unsigned long>(thisBoot), radio_profile::group,
                kFrequencyMhz, unsigned(kSf), t096_pins::kFemDriveDbm);
  usbLog("BOOTLOADER raw=%08lX version=%u.%u.%u reset=%08lX\n",
         static_cast<unsigned long>(bootloaderVersion),
         unsigned((bootloaderVersion >> 16) & 0xff),
         unsigned((bootloaderVersion >> 8) & 0xff), unsigned(bootloaderVersion & 0xff),
         static_cast<unsigned long>(readResetReason()));
}
uint16_t clientIdFromFicr() {
  uint32_t v = NRF_FICR->DEVICEID[0] ^ (NRF_FICR->DEVICEID[1] * 0x9E3779B9UL); v ^= v >> 16;
  uint16_t id = uint16_t(v) ^ uint16_t(v >> 16); if (id == 0 || id == ID_BROADCAST) id ^= 0x5A5A;
  return (id == 0 || id == ID_BROADCAST) ? 1 : id;
}
uint32_t bootIdFromRng() {
  uint32_t v = 0; NRF_RNG->TASKS_START = 1;
  for (uint8_t i = 0; i < 4; ++i) { uint32_t guard = 100000; while (!NRF_RNG->EVENTS_VALRDY && --guard) {}
    if (!NRF_RNG->EVENTS_VALRDY) break;
    v = (v << 8) | NRF_RNG->VALUE;
    NRF_RNG->EVENTS_VALRDY = 0;
  }
  NRF_RNG->TASKS_STOP = 1; v ^= NRF_FICR->DEVICEID[0] ^ (NRF_FICR->DEVICEID[1] << 1) ^ micros(); return v ? v : 1;
}
bool usbVbusPresent() { return (NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0; }
void femTx() { digitalWrite(t096_pins::kFemPower,HIGH); digitalWrite(t096_pins::kFemCsd,HIGH); delay(1); digitalWrite(t096_pins::kFemCtx,HIGH); delay(2); }
void femRx() { digitalWrite(t096_pins::kFemPower,HIGH); digitalWrite(t096_pins::kFemCsd,HIGH); digitalWrite(t096_pins::kFemCtx,LOW); delay(1); }
void femOff() { digitalWrite(t096_pins::kFemCtx,LOW); digitalWrite(t096_pins::kFemCsd,LOW); digitalWrite(t096_pins::kFemPower,LOW); }
void startReceiveWindow() {
  radioSleeping = false; // SX126x startReceive() wakes the retained radio.
  femRx(); radioIrq = false;
  // Latch preamble IRQ without routing it to DIO1; only RX_DONE interrupts.
  rxPreamble = false;
  const int16_t r = radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF,
      RADIOLIB_IRQ_RX_DEFAULT_FLAGS | (1UL << RADIOLIB_IRQ_PREAMBLE_DETECTED), RADIOLIB_IRQ_RX_DEFAULT_MASK); if (r != RADIOLIB_ERR_NONE) ++rxErrors;
  air = r == RADIOLIB_ERR_NONE ? AirState::Receiving : AirState::Idle; airStartedMs = millis();
}
void sendBuffered() {
  if (!outLength || air != AirState::Idle) return;
  const uint32_t slotStarted=millis();
  // RadioLib 7.8 TX stage reads packet type before any standby/wakeup. RX
  // staging does wake itself; TX needs an explicit retained-sleep wake first.
  nextTxStartMs = slotStarted + 500; // failed attempts also respect the guard.
  int16_t r = radioSleeping ? radio.standby() : RADIOLIB_ERR_NONE;
  if(r == RADIOLIB_ERR_NONE) {
    radioSleeping = false;
    femTx();radioIrq = false;
    r = radio.startTransmit(out,outLength);
  } else {
    radioReady=false;nextRadioRetryMs=millis();
    usbLog("RADIO wake failed=%d; retry in 5s\n",r);
  }
  if (r != RADIOLIB_ERR_NONE) {
    ++txErrors;
    usbLog("TX start failed=%d; retry after guard\n",r);
    if (outIsState) confirmationPending = true;
    outLength=0; outOpensReceiveWindow=false; outIsState=false; femOff(); return;
  }
  air=AirState::Sending; airStartedMs=millis();
}
bool queueState(uint32_t now) {
  if (outLength || air != AirState::Idle) return false;
  const client_control::Status s{thisBoot,control.state(),control.station(),control.command(),now,battery.mv(),txPackets,rxErrors,usbVbusPresent()};
  PacketHeader h{thisClientId,++txSequence,MSG_CLIENT_STATE}; outLength=client_control::encodeStatus(out,sizeof(out),h,s);
  if (!outLength) return false;
  outOpensReceiveWindow=true; outIsState=true;
  usbLog("STATE id=%04X state=%s station=%u command=%u tx=%lu err=%u vbus=%u\n",
                thisClientId, client_control::name(control.state()), control.station(),
                control.command(), static_cast<unsigned long>(txPackets), rxErrors, usbVbusPresent());
  usbLog("IDLE waits=%lu blocked_ms=%lu radio_sleep=%u\n", static_cast<unsigned long>(idleWaitCalls),
         static_cast<unsigned long>(idleBlockedMs), radioSleeping);
  return true;
}
bool queueProbe(uint32_t now) {
  if (outLength || air != AirState::Idle) return false;
  PacketHeader h{thisClientId,++txSequence,MSG_LINK_TEST};
  outLength=client_control::encodeProbe(out,sizeof(out),h,{thisBoot,++probeCounter,now}); if (!outLength) return false;
  outOpensReceiveWindow=false; outIsState=false; control.notedProbe(now); return true;
}
void observeDataDeferral(uint32_t now) {
  if(control.state()!=client_control::State::Tracking)return;
  gnss_snapshot::Snapshot s{};
  const bool have=gnss.sample(now,s);
  const bool valid=have && s.fix && s.arrivalAgeMs<tracking_policy::kGpsFreshMs &&
      isfinite(s.lat) && isfinite(s.lon) && s.lat>=-90 && s.lat<=90 && s.lon>=-180 && s.lon<=180;
  const bool due=cadence.due(now,valid,s.epochMsOfDay,s.haveRmc && s.haveGga);
  if(!due) { dataWaiting=false;return; }
  if((air!=AirState::Idle || int32_t(now-nextTxStartMs)<0) && !dataWaiting) {
    ++dataDeferred;dataWaiting=true;
  }
}
bool queueRealPosition(uint32_t now) {
  if (control.state()!=client_control::State::Tracking || outLength || air!=AirState::Idle) return false;
  gnss_snapshot::Snapshot s{};
  const bool have = gnss.sample(now,s);
  const bool valid = have && s.fix && s.arrivalAgeMs < tracking_policy::kGpsFreshMs &&
      isfinite(s.lat) && isfinite(s.lon) && s.lat >= -90 && s.lat <= 90 && s.lon >= -180 && s.lon <= 180;
  if (!cadence.due(now,valid,s.epochMsOfDay,s.haveRmc && s.haveGga)) return false;
  const bool velocity=valid && s.velocityValid && s.speedMps>=0.3 && s.courseDeg>=0.0;
  PositionPayload p{};
  p.speedDmS=protocol::kUnknown; p.courseDeg10=protocol::kUnknownCourse; p.hdop10=protocol::kUnknown;
  p.fix=valid; p.velocityValid=velocity;
  if(valid) {
    p.latE7=int32_t(lround(s.lat*1e7)); p.lonE7=int32_t(lround(s.lon*1e7));
    p.satelliteClass=protocol::satClass(s.satellites); p.hdop10=protocol::quantizeHdop(s.hdop);
    if(velocity) { p.speedDmS=protocol::quantizeSpeed(s.speedMps); p.courseDeg10=uint16_t(lround(s.courseDeg*10.0))%3600; }
  }
  PacketHeader h{thisClientId,dataSequence++,MSG_DATA}; outLength=protocol::encodeData(out,sizeof(out),h,p);
  if (outLength) { dataWaiting=false; outOpensReceiveWindow=false; outIsState=false; cadence.attempted(now,valid,s.epochMsOfDay); }
  return outLength != 0;
}
bool queueDiagnostic(uint32_t now) {
  if (control.state()!=client_control::State::Tracking || int32_t(now-nextExtraMs)<0) return false;
  // One extra slot per 5 s, rotating the existing wire-v5 diagnostics. STATE
  // has its own 5 s control slot; neither implies a new positioning epoch.
  gnss_snapshot::Snapshot s{}; const bool have=gnss.sample(now,s);
  const uint8_t type = extraKind==0 ? MSG_TELEMETRY : extraKind==1 ? MSG_DIAGNOSTIC : MSG_GNSS_DIAGNOSTIC;
  PacketHeader h{thisClientId,++txSequence,type};
  if(type==MSG_TELEMETRY) {
    const TelemetryPayload p{battery.mv(),INT8_MIN,255,uint8_t(have && s.haveGga && s.arrivalAgeMs<2000 ? s.satellites : 255)};
    outLength=protocol::encodeTelemetry(out,sizeof(out),h,p);
  } else if(type==MSG_DIAGNOSTIC) {
    const auto &g=gnss.stats();
    DiagnosticPayload p{gnss_diagnostics::age(g.lastEpochIntervalMs),gnss_diagnostics::counter(gpsBacklogDrops),
      gnss_diagnostics::counter(g.rejectedSentences),gnss_diagnostics::counter(txErrors),gnss_diagnostics::counter(dataDeferred),0};
    if(have) p.status=1|(s.fix?2:0)|(s.velocityValid?4:0)|(s.haveGga?8:0)|(s.haveRmc?16:0)|32;
    if(gpsRate.ready())p.status|=64;
    if(!strcmp(gpsRate.state(),"observed_2hz"))p.status|=128;
    outLength=protocol::encodeDiagnostic(out,sizeof(out),h,p);
  } else outLength=gnss_diagnostics::encode(out,sizeof(out),h,gnss_diagnostics::capture(gnss,now));
  if(!outLength)return false;
  extraKind=(extraKind+1)%3; nextExtraMs=now+5000;
  outOpensReceiveWindow=false;outIsState=false;return true;
}
void serviceGnss(uint32_t now) {
  if(!gnssEnabled)return;
  if(uint32_t(now-gpsLastServiceMs)>gnss.backlogAllowanceMs()) {
    while(Serial2.available())Serial2.read();
    gnss.invalidate(now); ++gpsBacklogDrops;
  }
  gpsLastServiceMs=now;
  static char response[150]; static size_t used=0;
  unsigned budget=512;
  while(Serial2.available() && budget--) {
    const char c=char(Serial2.read()); gnss.feed(c,now);
    if(c=='$')used=0;
    if(c=='\n') {
      response[used]=0;
      if(Serial && (!strncmp(response,"$PDTINFO",8)||!strncmp(response,"$CFGNAV",7)||
          !strncmp(response,"$CFGMSG",7)||!strncmp(response,"$FAIL",5))) usbLog("GNSS %s\n",response);
      used=0;
    } else if(c!='\r' && used+1<sizeof(response))response[used++]=c;
  }
  if(const char *command=gnssSetup.due(now)) {
    Serial2.print(command);
    if(Serial)usbLog("GNSS command=%s",command);
  }
  const auto &g=gnss.stats();
  if(gpsRate.observe(now,g.snapshots,g.rmcSentences,g.ggaSentences) && Serial) {
    const auto r=gnss_diagnostics::capture(gnss,now);
    usbLog("GNSS rate=%s epoch=%.2f rmc=%.2f gga=%.2f state=%s sats=%u epoch_ms=%u backlog=%lu checksum=%lu usb_drop=%lu\n",
      gpsRate.state(),gpsRate.hz(),gpsRate.rmcHz(),gpsRate.ggaHz(),gnss_diagnostics::state(r),r.satellites,
      unsigned(g.lastEpochIntervalMs),static_cast<unsigned long>(gpsBacklogDrops),static_cast<unsigned long>(g.checksumErrors),static_cast<unsigned long>(usbLogDropped));
  }
}
void serviceReceive(uint32_t now) {
  if (air!=AirState::Receiving) return;
  const uint32_t age=uint32_t(now-airStartedMs);
  // No SPI polling until an IRQ or the short-listen boundary. A latched
  // preamble extends the receive deadline, but never beyond the hard cap.
  if(!radioIrq && age<client_control::kReceiveListenMs)return;
  const uint32_t flags=radio.getIrqFlags();
  rxPreamble=rxPreamble || (flags & (RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED|RADIOLIB_SX126X_IRQ_HEADER_VALID));
  if(!radioIrq && rxPreamble && age<client_control::kReceiveWindowMs)return;
  const bool irq=radioIrq; radioIrq=false;
  if (irq && (flags&RADIOLIB_SX126X_IRQ_RX_DONE)) { uint8_t frame[MAX_PACKET_LEN]={}; const size_t n=radio.getPacketLength();
    const int16_t r=n<=sizeof(frame)?radio.readData(frame,n):RADIOLIB_ERR_PACKET_TOO_LONG; PacketHeader h{}; client_control::Command c{};
    if (r==RADIOLIB_ERR_NONE && client_control::decodeCommand(frame,n,h,c)) { const auto a=control.apply(h,c,now);
      if (a==t096_control::CommandResult::Applied || a==t096_control::CommandResult::Duplicate) {
        if(a==t096_control::CommandResult::Applied) {
          setGnssEnabled(control.state()==client_control::State::Tracking);
          nextBatteryMs = now; // Sample immediately when changing load/mode.
        }
        confirmationPending=true;
      }
    } else ++rxErrors;
  } else if (irq && !(flags&RADIOLIB_SX126X_IRQ_TIMEOUT)) ++rxErrors;
  radio.finishReceive(); air=AirState::Idle; femOff();
}
void serviceTransmit(uint32_t now) {
  if (air!=AirState::Sending || (!radioIrq && !t096_control::elapsed(now,airStartedMs,kTxTimeoutMs))) return;
  const bool irq=radioIrq; radioIrq=false; const uint32_t flags=irq?radio.getIrqFlags():0;
  const bool sent=irq && (flags&RADIOLIB_SX126X_IRQ_TX_DONE), openWindow=outOpensReceiveWindow, wasState=outIsState;
  outOpensReceiveWindow=false; outIsState=false;
  radio.finishTransmit(); air=AirState::Idle; outLength=0;
  if(sent) {
    ++txPackets;
    if (wasState) { control.notedState(now); usbLog("STATE TX done count=%lu\n", static_cast<unsigned long>(txPackets)); }
  } else {
    ++txErrors;
    if (wasState) { confirmationPending=true; usbLog("%s\n","STATE TX failed; retrying"); }
  }
  if(sent && openWindow) startReceiveWindow(); else femOff();
}
void setGnssEnabled(bool enabled) {
  // T096 schematic: Vext feeds the GNSS load switch as well as the TFT.
  // GNSS_EN alone cannot power the receiver while Vext is off.
  pinMode(t096_pins::kVextControl,OUTPUT);
  if(enabled)digitalWrite(t096_pins::kVextControl,HIGH);
  pinMode(t096_pins::kGnssEnable,OUTPUT); digitalWrite(t096_pins::kGnssEnable,enabled?LOW:HIGH);
  if(enabled && !gnssEnabled) {
    gnss.reset(millis()); cadence.reset(millis()); dataWaiting=false; gpsRate=gnss_rate::Monitor{};
    gpsLastServiceMs=millis(); gnssSetup.begin(millis()); nextExtraMs=millis()+1500; extraKind=0;
    Serial2.begin(115200);
  } else if (!enabled && Serial2) {
    // Uart::end() waits for RX/TX stop events; calling it before begin() on
    // UARTE1 can wait forever and prevent the USB boot diagnostic.
    Serial2.end();
  }
  gnssEnabled=enabled;
  if(!enabled) {
    gnssSetup.stop();
    // Ready keeps the TFT controller asleep, allowing a short wake without a
    // blocking 650 ms reinitialization. GNSS has its separate load switch off.
    digitalWrite(t096_pins::kVextControl,displayPowered?HIGH:LOW);
  }
}
void wakeClientDisplay(uint32_t now) {
  if(!displayPowered)return;
  screenUntilMs=now+t096_power::kScreenMs;
  displayRow=0; nextDisplayMs=now;
  if(displaySleeping) {
    // SLPOUT needs 120 ms. Keep serving radio/GNSS during that delay.
    tft.sendCommand(ST77XX_SLPOUT);
    displaySleeping=false;displayWaking=true;displayWakeMs=now+120;
  }
  usbLog("DISPLAY wake until=%lu\n",static_cast<unsigned long>(screenUntilMs));
}
void initClientDisplay() {
  pinMode(t096_pins::kTftBacklight,OUTPUT);
  digitalWrite(t096_pins::kTftBacklight,HIGH);
  if(!displayLine.getBuffer()) { usbLog("%s\n","DISPLAY row buffer unavailable");return; }
  digitalWrite(t096_pins::kVextControl,HIGH);
  displaySpi.begin(); // Adafruit only begins SPI interfaces declared by the variant.
  tft.initPanel();
  tft.invertDisplay(true); // match Heltec's plugin-panel factory example
  tft.setSPISpeed(8000000);
  tft.fillScreen(ST77XX_BLACK);tft.setTextWrap(false);
  displayPowered=true;
  wakeClientDisplay(millis());
  usbLog("%s\n","DISPLAY init 160x80 SPI2 offset=24,0 backlight=44");
}
void serviceClientDisplay(uint32_t now) {
  if(!displayPowered)return;
  // Backlight timeout is cheap GPIO and must not wait for a long RF frame.
  if(int32_t(now-screenUntilMs)>=0 && !userButton.held())
    digitalWrite(t096_pins::kTftBacklight,HIGH);
  if(air!=AirState::Idle || radioIrq)return;
  if(displayWaking) {
    if(int32_t(now-displayWakeMs)<0)return;
    displayWaking=false; screenUntilMs=now+t096_power::kScreenMs;
  }
  if(int32_t(now-screenUntilMs)>=0 && !userButton.held()) {
    if(!displaySleeping) {
      digitalWrite(t096_pins::kTftBacklight,HIGH);
      tft.sendCommand(ST77XX_DISPOFF);tft.sendCommand(ST77XX_SLPIN);
      displaySleeping=true;
      usbLog("DISPLAY sleep gnss=%u max_ms=%lu\n",gnssEnabled,static_cast<unsigned long>(displayMaxMs));
    }
    return;
  }
  if(displaySleeping || int32_t(now-nextDisplayMs)<0)return;
  char line[28]={};uint16_t color=ST77XX_WHITE;
  switch(displayRow) {
    case 0:snprintf(line,sizeof(line),"Client %04X",thisClientId);break;
    case 1:
      snprintf(line,sizeof(line),"%s%s",batteryProtection?"LOW BATTERY":client_control::name(control.state()),usbVbusPresent()?"  USB":"");
      color=batteryProtection?ST77XX_RED:ST77XX_GREEN;break;
    case 2:
      if(battery.mv())snprintf(line,sizeof(line),"Batt %.2fV ~%u%%",battery.mv()/1000.0,t096_power::batteryPercent(battery.mv()));
      else snprintf(line,sizeof(line),"Batt unknown");
      if(battery.warning())color=ST77XX_YELLOW;
      break;
    case 3: {
      gnss_snapshot::Snapshot s{};const bool have=gnssEnabled&&gnss.sample(now,s);
      const char *quality=!gnssEnabled?"off":!have?"waiting":geo::sig4Text(geo::gpsSignal(s.fix && s.arrivalAgeMs<2000,s.satellites,s.hdop));
      snprintf(line,sizeof(line),"GPS %s",quality);break;
    }
    case 4:
      if(batteryProtection)snprintf(line,sizeof(line),"Low - charge to recover");
      else if(userButton.held())snprintf(line,sizeof(line),"Release USER: Ready");
      else snprintf(line,sizeof(line),"USER hold 2s: Ready");
      break;
  }
  const uint32_t started=millis();
  displayLine.fillScreen(ST77XX_BLACK);
  displayLine.setTextWrap(false);displayLine.setTextColor(color);
  displayLine.setTextSize(displayRow==0?2:1);
  displayLine.setCursor(0,displayRow==0?0:4);displayLine.print(line);
  tft.drawRGBBitmap(0,displayRow*16,displayLine.getBuffer(),160,16);
  displayMaxMs=std::max(displayMaxMs,uint32_t(millis()-started));
  if(++displayRow==5) {
    displayRow=0;nextDisplayMs=millis()+500;
    tft.sendCommand(ST77XX_DISPON);digitalWrite(t096_pins::kTftBacklight,LOW);
  }
}
void serviceBattery(uint32_t now) {
  if(air!=AirState::Idle || radioIrq)return;
  if(!batterySampling) {
    if(int32_t(now-nextBatteryMs)<0)return;
    digitalWrite(t096_pins::kBatteryAdcControl,HIGH);
    batterySampling=true;batterySampleMs=now;
    return;
  }
  if(uint32_t(now-batterySampleMs)<10)return;
  analogRead(t096_pins::kBatteryAdc); // discard first conversion after divider enable
  uint32_t sum=0;for(unsigned i=0;i<4;++i)sum+=analogRead(t096_pins::kBatteryAdc);
  digitalWrite(t096_pins::kBatteryAdcControl,LOW);
  batterySampling=false;
  const uint16_t raw=(sum+2)/4;
  battery.observe(t096_power::batteryMv(raw),usbVbusPresent());
  nextBatteryMs=now+(battery.protectedSleep()?t096_power::kBatteryProtectMs:
      control.state()==client_control::State::Ready && !battery.warning()?
      t096_power::kBatteryReadyMs:t096_power::kBatteryActiveMs);
  usbLog("BAT raw=%u mv=%u usb=%u low=%u\n",raw,battery.mv(),usbVbusPresent(),battery.shutdown());
}
void serviceUserButton(uint32_t now) {
  const auto event=userButton.poll(digitalRead(t096_pins::kUserButton)==LOW,now);
  if(event==t096_power::ButtonEvent::Short || event==t096_power::ButtonEvent::Hold) wakeClientDisplay(now);
  if(event==t096_power::ButtonEvent::LongReleased && !batteryProtection) {
    control.requestReady();setGnssEnabled(false);confirmationPending=true;
    nextBatteryMs=now;wakeClientDisplay(now);usbLog("%s\n","USER long released: Ready");
  }
}
void serviceLowBattery(uint32_t now) {
  if(batteryProtection==battery.protectedSleep())return;
  batteryProtection=battery.protectedSleep();
  control.requestReady();setGnssEnabled(false);
  // Sample completion is only serviced with an idle radio. Never start a PA
  // transmission in protection; recovery always returns to Ready, not GPS.
  outLength=0;outIsState=outOpensReceiveWindow=false;
  confirmationPending=!batteryProtection;
  femOff();nextTxStartMs=now;
  wakeClientDisplay(now);
  usbLog("POWER protection=%u mv=%u recover=3x3500mV sample_ms=%lu\n",batteryProtection,
         battery.mv(),static_cast<unsigned long>(batteryProtection?t096_power::kBatteryProtectMs:t096_power::kBatteryReadyMs));
}
bool configureRadio() {
  radioSleeping=false;
  const int16_t start = t096_radio::begin(radio, kFrequencyMhz, kBandwidthKhz,
                                          kSf, kCr, kSync, t096_pins::kFemDriveDbm);
  const int16_t dio2 = start == RADIOLIB_ERR_NONE ? radio.setDio2AsRfSwitch(true) : start;
  const int16_t current = dio2 == RADIOLIB_ERR_NONE ? radio.setCurrentLimit(140.0f) : dio2;
  radioReady = current == RADIOLIB_ERR_NONE;
  if (radioReady) {
    radio.setDio1Action(onRadioIrq);
    usbLog("%s\n","RADIO init ok");
  } else {
    ++rxErrors;
    usbLog("RADIO init failed=%d; retry in 5s outside battery protection\n", current);
  }
  return radioReady;
}
uint32_t idleWaitMs(uint32_t now) {
  t096_idle::Budget wait(now, batteryProtection?t096_power::kBatteryProtectMs:client_control::kReadyPollMs);
  // Keep the existing UART/GNSS/2 Hz pipeline responsive in Tracking/Test.
  if(control.state()!=client_control::State::Ready)wait.cap(1);
  if(usbVbusPresent())wait.cap(20); // CDC/USB handling while externally powered.
  if(userButton.busy())wait.cap(5); // debounce, held-button and release deadlines.
  if(radioIrq)wait.cap(0);
  if(air==AirState::Sending)wait.until(airStartedMs+kTxTimeoutMs);
  else if(air==AirState::Receiving)
    wait.until(airStartedMs+(rxPreamble?client_control::kReceiveWindowMs:client_control::kReceiveListenMs));
  else {
    wait.until(batterySampling?batterySampleMs+10:nextBatteryMs);
    if(radioReady && !batteryProtection) {
      const uint32_t slot=int32_t(nextTxStartMs-now)>0?uint32_t(nextTxStartMs-now):0;
      const uint32_t poll=(confirmationPending||outLength)?0:control.pollWaitMs(now);
      wait.cap(std::max(slot,poll));
    }
    if(displayPowered && !displaySleeping) {
      if(displayWaking)wait.until(displayWakeMs);
      else wait.until(nextDisplayMs);
    }
  }
  if(displayPowered && !displaySleeping && !userButton.held() && int32_t(screenUntilMs-now)>0)
    wait.until(screenUntilMs);
  if(!radioReady && !batteryProtection)wait.until(nextRadioRetryMs+5000);
  return wait.waitMs();
}
void waitForWork() {
  if(air==AirState::Idle && radioReady && !radioSleeping &&
     (control.state()==client_control::State::Ready || batteryProtection)) {
    const int16_t result=radio.sleep(); // warm-start sleep retains SX1262 configuration.
    femOff();
    if(result==RADIOLIB_ERR_NONE)radioSleeping=true;
    else { radioReady=false;nextRadioRetryMs=millis();++rxErrors;usbLog("RADIO sleep failed=%d\n",result); }
  }
  const uint32_t now=millis(),wait=idleWaitMs(now);
  // A notification sent before this call remains pending; there is no
  // check-then-WFE race when a USER/DIO1 edge arrives just before blocking.
  if(wait) {
    ++idleWaitCalls;
    ulTaskNotifyTake(pdTRUE,std::max(TickType_t(1),pdMS_TO_TICKS(wait)));
    idleBlockedMs+=uint32_t(millis()-now);
  } else { ulTaskNotifyTake(pdTRUE,0);yield(); }
}
} // namespace

void setup() {
  loopTask=xTaskGetCurrentTaskHandle();
  pinMode(t096_pins::kFemPower,OUTPUT); pinMode(t096_pins::kFemCsd,OUTPUT); pinMode(t096_pins::kFemCtx,OUTPUT); femOff(); setGnssEnabled(false);
  thisClientId=clientIdFromFicr(); thisBoot=bootIdFromRng(); control=t096_control::Controller(thisBoot); control.setClientId(thisClientId);
  Serial.begin(115200); // no wait for USB host
  usbLog("%s\n","BOOT stage=serial");
  pinMode(t096_pins::kVextControl, OUTPUT); digitalWrite(t096_pins::kVextControl, LOW);
  printIdentity("BOOT");
  pinMode(t096_pins::kUserButton,INPUT_PULLUP);
  userButton.begin(digitalRead(t096_pins::kUserButton)==LOW,millis());
  attachInterrupt(t096_pins::kUserButton,onUserEdge,CHANGE);
  pinMode(t096_pins::kBatteryAdcControl,OUTPUT);digitalWrite(t096_pins::kBatteryAdcControl,LOW);
  analogReference(AR_INTERNAL_3_0);analogReadResolution(12);
  initClientDisplay(); // boot only, before radio/GNSS; runtime screen updates are bounded rows
  // Sample before the first RF transmission. Radio initialization below only
  // configures standby; even a low-battery boot can then put SX1262 to sleep.
  for(unsigned i=0;i<3;++i) {
    nextBatteryMs=millis();serviceBattery(millis());delay(10);serviceBattery(millis());
  }
  SPI.begin();
  usbLog("%s\n","BOOT stage=radio");
  configureRadio();
  serviceLowBattery(millis());
  confirmationPending=true; // advertise immediately after a real reboot
}
void loop() {
  const uint32_t now=millis();
  if (Serial && !usbIdentityAnnounced) { printIdentity("USB"); usbIdentityAnnounced=true; }
  if (!Serial) usbIdentityAnnounced=false;
  if (!radioReady) {
    serviceUserButton(millis());serviceBattery(millis());serviceLowBattery(millis());
    serviceClientDisplay(millis());
    if (!batteryProtection && t096_control::elapsed(now, nextRadioRetryMs, 5000)) { nextRadioRetryMs=now; configureRadio(); }
    waitForWork(); return;
  }
  serviceGnss(millis());
  // TX completion can open RX after FEM switching advances millis(). Re-read
  // the clock so unsigned elapsed cannot treat that new window as expired.
  serviceTransmit(millis());
  serviceReceive(millis());
  serviceUserButton(millis());
  serviceBattery(millis());
  serviceLowBattery(millis());
  const uint32_t scheduleNow=millis();
  observeDataDeferral(scheduleNow);
  if(!batteryProtection && air==AirState::Idle && int32_t(scheduleNow-nextTxStartMs)>=0) {
    if(confirmationPending && queueState(scheduleNow)) confirmationPending=false;
    else if(control.pollDue(scheduleNow))queueState(scheduleNow);
    else if(control.probeDue(scheduleNow))queueProbe(scheduleNow);
    else if(!queueDiagnostic(scheduleNow))queueRealPosition(scheduleNow);
    sendBuffered();
  }
  serviceClientDisplay(millis());
  waitForWork(); // retained SystemON + FreeRTOS tickless RTC, interrupted by USER/DIO1.
}
