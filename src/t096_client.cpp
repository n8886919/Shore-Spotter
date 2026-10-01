// T096 standalone nRF52840 Client: no ESP Web/Wi-Fi and no fake GNSS.
#include <Arduino.h>
#include <RadioLib.h>
#include "client_cadence.h"
#include "client_control.h"
#include "firmware_version.h"
#include "gnss_snapshot.h"
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
volatile bool radioIrq = false;
void onRadioIrq() { radioIrq = true; }
enum class AirState : uint8_t { Idle, Sending, Receiving };
AirState air = AirState::Idle;
bool confirmationPending = false, outOpensReceiveWindow = false, outIsState = false, sleepAfterStateTx = false;
uint8_t out[CLIENT_STATE_PACKET_LEN] = {};
size_t outLength = 0;
uint16_t txSequence = 0, rxErrors = 0, thisClientId = 0;
uint32_t txPackets = 0, probeCounter = 0, airStartedMs = 0, thisBoot = 1;
uint32_t nextRadioRetryMs = 0;
uint32_t finalStateStartedMs = 0;
bool vbusWasPresent = false, radioReady = false, usbIdentityAnnounced = false;
t096_control::Controller control(1);

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
  Serial.printf("%s T096 v%s client=%04X boot=%08lX RF group=%u %.1fMHz drive=%ddBm\n",
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
  // Async SX126x startReceive() uses radio ticks; host clock limits this to 800 ms.
  const int16_t r = radio.startReceive(); if (r != RADIOLIB_ERR_NONE) ++rxErrors;
  air = r == RADIOLIB_ERR_NONE ? AirState::Receiving : AirState::Idle; airStartedMs = millis();
}
void sendBuffered() {
  if (!outLength || air != AirState::Idle) return;
  femTx();
  radioIrq = false;
  const int16_t r = radio.startTransmit(out,outLength); if (r != RADIOLIB_ERR_NONE) {
    ++rxErrors;
    if (outIsState) confirmationPending = true;
    outLength=0; outOpensReceiveWindow=false; outIsState=false; femOff(); return;
  }
  air=AirState::Sending; airStartedMs=millis();
}
bool queueState(uint32_t now) {
  if (outLength || air != AirState::Idle) return false;
  const client_control::Status s{thisBoot,control.state(),control.station(),control.command(),now,0,txPackets,rxErrors,usbVbusPresent()};
  PacketHeader h{thisClientId,++txSequence,MSG_CLIENT_STATE}; outLength=client_control::encodeStatus(out,sizeof(out),h,s);
  if (!outLength) return false;
  outOpensReceiveWindow=true; outIsState=true;
  Serial.printf("STATE id=%04X state=%s station=%u command=%u tx=%lu err=%u vbus=%u\n",
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
void queueRealPosition(uint32_t now) {
  gnss_snapshot::Snapshot s{};
  if (control.state()!=client_control::State::Tracking || outLength || air!=AirState::Idle || !gnss.sample(now,s) ||
      !tracking_policy::usableGps(s.fix,s.satellites,float(s.hdop),s.sourceAgeMs) || !cadence.due(now,s.fix,s.epochMsOfDay,control.hasStation())) return;
  const bool velocity=s.velocityValid && s.speedMps>=0.3 && s.courseDeg>=0.0;
  PositionPayload p{int32_t(lround(s.lat*1e7)),int32_t(lround(s.lon*1e7)),protocol::quantizeSpeed(s.speedMps),
    velocity?uint16_t(lround(s.courseDeg*10.0)):protocol::kUnknownCourse,protocol::satClass(s.satellites),true,velocity,
    isfinite(s.hdop)?protocol::quantizeHdop(s.hdop):protocol::kUnknown};
  PacketHeader h{thisClientId,++txSequence,MSG_DATA}; outLength=protocol::encodeData(out,sizeof(out),h,p);
  if (outLength) { outOpensReceiveWindow=false; outIsState=false; cadence.attempted(now,s.fix,s.epochMsOfDay); }
}
void serviceReceive(uint32_t now) {
  if (air!=AirState::Receiving || (!radioIrq && !t096_control::elapsed(now,airStartedMs,client_control::kReceiveWindowMs))) return;
  const bool irq=radioIrq; radioIrq=false; const uint32_t flags=irq?radio.getIrqFlags():0;
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
    if (wasState) { control.notedState(now); Serial.printf("STATE TX done count=%lu\n", static_cast<unsigned long>(txPackets)); }
  } else {
    ++rxErrors;
    if (wasState) { confirmationPending=true; Serial.println("STATE TX failed; retrying"); }
  }
  if(sent && sleepAfterStateTx) {
    // VBUS prevents the timer-driven SystemOFF. VBUS insertion cannot wake
    // SystemOFF on this unverified charge-board path, so check it again here.
    if (!usbVbusPresent()) enterSystemOff();
    sleepAfterStateTx=false; control.cancelStorage(now);
  }
  if(sent && openWindow) startReceiveWindow(); else femOff();
}
void setGnssEnabled(bool enabled) { pinMode(t096_pins::kGnssEnable,OUTPUT); digitalWrite(t096_pins::kGnssEnable,enabled?LOW:HIGH);
  if(enabled) {
    gnss.reset(millis()); cadence.reset(millis());
    Serial2.begin(115200);
  } else if (Serial2) {
    // Uart::end() waits for RX/TX stop events; calling it before begin() on
    // UARTE1 can wait forever and prevent the USB boot diagnostic.
    Serial2.end();
  }
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
    Serial.println("RADIO init ok");
  } else {
    ++rxErrors;
    Serial.printf("RADIO init failed=%d; retry in 5s\n", current);
  }
  return radioReady;
}
} // namespace

void setup() {
  pinMode(t096_pins::kFemPower,OUTPUT); pinMode(t096_pins::kFemCsd,OUTPUT); pinMode(t096_pins::kFemCtx,OUTPUT); femOff(); setGnssEnabled(false);
  thisClientId=clientIdFromFicr(); thisBoot=bootIdFromRng(); control=t096_control::Controller(thisBoot); control.setClientId(thisClientId); vbusWasPresent=usbVbusPresent();
  Serial.begin(115200); // no wait for USB host
  Serial.println("BOOT stage=serial");
  pinMode(t096_pins::kVextControl, OUTPUT); digitalWrite(t096_pins::kVextControl, LOW);
  printIdentity("BOOT");
  SPI.begin();
  Serial.println("BOOT stage=radio");
  configureRadio();
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
  while(Serial2.available()) gnss.feed(char(Serial2.read()),now);
  // TX completion can open RX after FEM switching advances millis(). Re-read
  // the clock so unsigned elapsed cannot treat that new window as expired.
  serviceTransmit(millis());
  serviceReceive(millis());
  const uint32_t scheduleNow=millis();
  if(air==AirState::Idle) { if(confirmationPending && queueState(scheduleNow)) confirmationPending=false; else if(control.pollDue(scheduleNow)) queueState(scheduleNow); else if(control.probeDue(scheduleNow)) queueProbe(scheduleNow); else queueRealPosition(scheduleNow); sendBuffered(); }
  delay(1); // yields on nRF Arduino core; READY is not a full-speed spin.
}
