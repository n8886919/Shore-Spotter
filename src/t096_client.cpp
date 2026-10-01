// T096 standalone nRF52840 Client: no ESP Web/Wi-Fi and no fake GNSS.
#include <Arduino.h>
#include <stdarg.h>
#include <RadioLib.h>
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
#include "tracking_policy.h"

namespace {
constexpr float kFrequencyMhz = radio_profile::frequencyMhz, kBandwidthKhz = radio_profile::bandwidthKhz;
constexpr uint8_t kSf = radio_profile::spreadingFactor, kCr = radio_profile::codingRate, kSync = radio_profile::syncWord;
constexpr uint32_t kTxTimeoutMs = 3000;
constexpr uint32_t kFinalStateAttemptMs = 3000;
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
void onRadioIrq() { radioIrq = true; }
enum class AirState : uint8_t { Idle, Sending, Receiving };
AirState air = AirState::Idle;
bool confirmationPending = false, outOpensReceiveWindow = false, outIsState = false, sleepAfterStateTx = false;
uint8_t out[MAX_PACKET_LEN] = {};
size_t outLength = 0;
uint16_t txSequence = 0, rxErrors = 0, thisClientId = 0;
uint32_t txPackets = 0, probeCounter = 0, airStartedMs = 0, thisBoot = 1;
uint32_t nextRadioRetryMs = 0;
uint32_t finalStateStartedMs = 0;
bool vbusWasPresent = false, radioReady = false, usbIdentityAnnounced = false;
t096_control::Controller control(1);

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
void enterSystemOff();
void armSystemOff(uint32_t now) {
  if (sleepAfterStateTx) return;
  control.requestStorage();
  setGnssEnabled(false);
  confirmationPending = true;
  sleepAfterStateTx = true;
  finalStateStartedMs = now;
}
void printIdentity(const char *prefix) {
  usbLog("%s T096 v%s client=%04X boot=%08lX RF group=%u %.1fMHz drive=%ddBm\n",
                prefix, SHORE_SPOTTER_VERSION, thisClientId,
                static_cast<unsigned long>(thisBoot), radio_profile::group,
                kFrequencyMhz, t096_pins::kFemDriveDbm);
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
  femTx();
  radioIrq = false;
  const int16_t r = radio.startTransmit(out,outLength); if (r != RADIOLIB_ERR_NONE) {
    ++txErrors;
    if (outIsState) confirmationPending = true;
    outLength=0; outOpensReceiveWindow=false; outIsState=false; femOff(); return;
  }
  air=AirState::Sending; airStartedMs=millis();
  nextTxStartMs = slotStarted + 500;
}
bool queueState(uint32_t now) {
  if (outLength || air != AirState::Idle) return false;
  const client_control::Status s{thisBoot,control.state(),control.station(),control.command(),now,0,txPackets,rxErrors,usbVbusPresent()};
  PacketHeader h{thisClientId,++txSequence,MSG_CLIENT_STATE}; outLength=client_control::encodeStatus(out,sizeof(out),h,s);
  if (!outLength) return false;
  outOpensReceiveWindow=true; outIsState=true;
  usbLog("STATE id=%04X state=%s station=%u command=%u tx=%lu err=%u vbus=%u\n",
                thisClientId, client_control::name(control.state()), control.station(),
                control.command(), static_cast<unsigned long>(txPackets), rxErrors, usbVbusPresent());
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
    const TelemetryPayload p{0,INT8_MIN,255,uint8_t(have && s.haveGga && s.arrivalAgeMs<2000 ? s.satellites : 255)};
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
          if (control.state()==client_control::State::Storage) armSystemOff(now);
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
  if(sent && sleepAfterStateTx) {
    // Keep externally powered hardware awake. nRF52840 supports wake on a
    // later VBUS rise; the actual wireless receiver path still needs testing.
    if (!usbVbusPresent()) enterSystemOff();
    sleepAfterStateTx=false; control.cancelStorage(now);
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
  if(!enabled) { gnssSetup.stop(); digitalWrite(t096_pins::kVextControl,LOW); }
}
void enterSystemOff() {
  radio.sleep(); femOff(); setGnssEnabled(false);
  // Heltec's HT-n5262G variant marks VEXT_ENABLE as HIGH, so LOW cuts Vext.
  digitalWrite(t096_pins::kVextControl, LOW);
  systemOff(t096_pins::kUserButton,LOW);
}
bool configureRadio() {
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
    usbLog("RADIO init failed=%d; retry in 5s\n", current);
  }
  return radioReady;
}
} // namespace

void setup() {
  pinMode(t096_pins::kFemPower,OUTPUT); pinMode(t096_pins::kFemCsd,OUTPUT); pinMode(t096_pins::kFemCtx,OUTPUT); femOff(); setGnssEnabled(false);
  thisClientId=clientIdFromFicr(); thisBoot=bootIdFromRng(); control=t096_control::Controller(thisBoot); control.setClientId(thisClientId); vbusWasPresent=usbVbusPresent();
  Serial.begin(115200); // no wait for USB host
  usbLog("%s\n","BOOT stage=serial");
  pinMode(t096_pins::kVextControl, OUTPUT); digitalWrite(t096_pins::kVextControl, LOW);
  printIdentity("BOOT");
  SPI.begin();
  usbLog("%s\n","BOOT stage=radio");
  configureRadio();
  confirmationPending=true; // advertise immediately after a real reboot
}
void loop() {
  const uint32_t now=millis();
  if (Serial && !usbIdentityAnnounced) { printIdentity("USB"); usbIdentityAnnounced=true; }
  if (!Serial) usbIdentityAnnounced=false;
  const bool vbus=usbVbusPresent();
  if(vbusWasPresent && !vbus) control.resetReadyTimer(now);
  vbusWasPresent=vbus;
  if (sleepAfterStateTx && vbus) {
    sleepAfterStateTx=false;
    control.cancelStorage(now);
  }
  if(control.storageDue(now,vbus)) armSystemOff(now);
  // The final STATE is attempted first. A dead radio may not extend the
  // configured 12-hour READY budget indefinitely.
  if(sleepAfterStateTx && !vbus && t096_control::elapsed(now, finalStateStartedMs, kFinalStateAttemptMs)) enterSystemOff();
  if (!radioReady) {
    if (t096_control::elapsed(now, nextRadioRetryMs, 5000)) { nextRadioRetryMs=now; configureRadio(); }
    delay(1); return;
  }
  serviceGnss(millis());
  // TX completion can open RX after FEM switching advances millis(). Re-read
  // the clock so unsigned elapsed cannot treat that new window as expired.
  serviceTransmit(millis());
  serviceReceive(millis());
  const uint32_t scheduleNow=millis();
  observeDataDeferral(scheduleNow);
  if(air==AirState::Idle && int32_t(scheduleNow-nextTxStartMs)>=0) {
    if(confirmationPending && queueState(scheduleNow)) confirmationPending=false;
    else if(control.pollDue(scheduleNow))queueState(scheduleNow);
    else if(control.probeDue(scheduleNow))queueProbe(scheduleNow);
    else if(!queueDiagnostic(scheduleNow))queueRealPosition(scheduleNow);
    sendBuffered();
  }
  delay(1); // yields on nRF Arduino core; READY is not a full-speed spin.
}
