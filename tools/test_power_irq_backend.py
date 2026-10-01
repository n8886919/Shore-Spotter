#!/usr/bin/env python3
"""Host fault injection using actual XPowersLib and extracted firmware handlers.

No serial/device access. Register transport, screen, clock and queues are fakes;
this proves software behavior and ordering, not an outdoor failure's cause.
"""
from pathlib import Path
import json
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/main.cpp").read_text()
XP = ROOT / ".pio/libdeps/tbeam-client-diagnostic/XPowersLib/src"
if not XP.exists():
    XP = ROOT / ".pio/libdeps/tbeam-client/XPowersLib/src"


def block(marker, after=0):
    start = SOURCE.index(marker, after)
    body = SOURCE.index("{", start)
    depth = 0
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    for match in re.finditer(tokens, SOURCE[body:]):
        if match.group() == "{":
            depth += 1
        elif match.group() == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start:body + match.end()]
    raise ValueError(marker)


CPP = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>
#include "XPowersAXP2101.tpp"
#include "power_irq.h"
#include "loop_metrics.h"
#include "diagnostic_store_codec.h"
const char *u8g2_font_5x7_tr="small", *u8g2_font_7x13B_tr="wordmark";
#include "client_boot_animation.h"
#define ROLE_CLIENT 1
#define FIELD_DIAGNOSTIC 1
#define F(x) x
static_assert(power_irq::kStatus1 == XPOWERS_AXP2101_INTSTS1, "IRQ address");
static_assert(power_irq::kShort == XPOWERS_AXP2101_PKEY_SHORT_IRQ >> 8, "short IRQ");
static_assert(power_irq::kLong == XPOWERS_AXP2101_PKEY_LONG_IRQ >> 8, "long IRQ");
static_assert(power_irq::kNegative == XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ >> 8, "press edge");
static_assert(power_irq::kPositive == XPOWERS_AXP2101_PKEY_POSITIVE_IRQ >> 8, "release edge");
static uint8_t regs[256];
static int faultRead=-1, faultWrite=-1;
static unsigned writes=0, oledBegin=0, powerOff=0, lowShutdown=0;
static uint32_t nowMs=1000;
static std::vector<std::string> events, records, trace;
static bool bootScreenTest=false;
static std::vector<uint32_t> gpsTimes, usbTimes, diagTimes, infoTimes, sleepTimes, clearTimes, oledReadyTimes;
static unsigned bootFrames=0;
static unsigned displayCallsAfterPowerOff=0;
static bool oledAvailable=true;
struct TimedIrq { uint32_t ms; uint8_t bits; };
static std::vector<TimedIrq> timedIrqs;
static size_t nextTimedIrq=0;
int readReg(uint8_t, uint8_t reg, uint8_t* data, uint8_t len) {
  if (faultRead == -2 || faultRead == reg) return -1;
  for (unsigned i=0;i<len;++i) data[i]=regs[reg+i];
  return 0;
}
int writeReg(uint8_t, uint8_t reg, uint8_t* data, uint8_t len) {
  ++writes;
  if (reg>=XPOWERS_AXP2101_INTSTS1 && reg<=XPOWERS_AXP2101_INTSTS3) trace.push_back("clear_irq");
  if (faultWrite == -2 || faultWrite == reg) return -1;
  for (unsigned i=0;i<len;++i) {
    if (reg+i>=XPOWERS_AXP2101_INTSTS1 && reg+i<=XPOWERS_AXP2101_INTSTS3)
      regs[reg+i] &= ~data[i];
    else regs[reg+i]=data[i];
  }
  if (reg==XPOWERS_AXP2101_COMMON_CONFIG && (*data&1)) { ++powerOff; events.push_back("power_off"); }
  return 0;
}
XPowersAXP2101 pmu(AXP2101_SLAVE_ADDRESS, readReg, writeReg);
uint32_t millis() { return nowMs; }
void delay(unsigned ms) {
  nowMs+=ms;
  while(nextTimedIrq<timedIrqs.size() && timedIrqs[nextTimedIrq].ms<=nowMs)
    regs[XPOWERS_AXP2101_INTSTS2] |= timedIrqs[nextTimedIrq++].bits;
}
struct ClientRailGuard { bool held=true; explicit ClientRailGuard(bool=false) {} };
static bool pmuOnline=true;
static uint32_t nextPmuKeyMs=0, clientOledOffMs=0, nextClientOledRefreshMs=0;
static bool clientOledAwake=false;
static constexpr uint32_t PMU_KEY_POLL_MS=100, CLIENT_SCREEN_WAKE_MS=10000, DISPLAY_REFRESH_MS=1000;
static constexpr int OLED_SDA_PIN=17, OLED_SCL_PIN=18, I2C_TRANSACTION_TIMEOUT_MS=10;
static uint8_t oledI2CAddr=0x3d;
static uint16_t cachedBatteryMv=4136;
static constexpr uint16_t BATT_PRESENT_MIN_MV=2500, BATT_SHUTDOWN_MV=3200;
static uint32_t powerBootId=560532669;
static power_irq::State powerIrqState;
static power_irq::Sample powerIrqSample;
static power_irq::ClientBootGuard clientPowerKeys;
static uint32_t powerIrqMs=0, powerVbusReadErrors=0;
static int powerVbusRaw[2]={-1,-1};
static uint8_t diagnosticRaw[128];
static size_t diagnosticRawLength=0;
static uint16_t diagnosticRawKind=1;
struct FakeWire { void begin(int,int){} void setTimeOut(int){} } Wire;
void detectOledAddress() {}
const char *u8g2_font_6x12_tr="font";
struct FakeDisplay {
  void setI2CAddress(int){}
  bool begin() {
    ++oledBegin; events.push_back("oled_begin"); delay(300);
    if (bootScreenTest && oledAvailable) oledReadyTimes.push_back(nowMs);
    return oledAvailable;
  }
  void clearBuffer(){ if (powerOff) ++displayCallsAfterPowerOff; if (bootScreenTest) clearTimes.push_back(nowMs); }
  void setFont(const char*){} void drawStr(int,int,const char*){}
  void sendBuffer(){ if (bootScreenTest) { ++bootFrames; delay(90); } }
  void setPowerSave(int){ if (bootScreenTest) sleepTimes.push_back(nowMs); }
  void setDrawColor(int){} void drawHLine(int,int,int){} void drawVLine(int,int,int){}
  void drawLine(int,int,int,int){} void drawPixel(int,int){} void drawCircle(int,int,int){}
  void drawDisc(int,int,int){} void drawBox(int,int,int,int){}
  int getStrWidth(const char *text){ return std::strlen(text)*5; }
} display;
namespace sd_log {
  void stop(){events.push_back("sd_stop");} bool stopped(){return true;}
  void serviceUsb(){ if (bootScreenTest) usbTimes.push_back(nowMs); }
}
void serviceGps(){ if (bootScreenTest) gpsTimes.push_back(nowMs); }
void serviceFieldDiagnostic(){ if (bootScreenTest) diagTimes.push_back(nowMs); }
static void drawClientInfoScreen(){ if (bootScreenTest) infoTimes.push_back(nowMs); display.sendBuffer(); }
namespace diagnostic_store {
bool submit(uint16_t kind, const void* bytes, size_t length, uint32_t) {
  assert(length<=codec::kMaxRecordBytes);
  uint8_t frame[codec::kFrameBytes];
  codec::beginFrame(frame, powerBootId, 0, nowMs);
  assert(codec::append(frame, kind, bytes, length, nowMs));
  codec::seal(frame);
  assert(codec::validFrame(frame));
  assert(!std::memcmp(frame+codec::kHeaderBytes+codec::kEnvelopeBytes, bytes, length));
  if (kind==1) { events.push_back("raw_tail"); return true; }
  assert(kind==2);
  std::string line(static_cast<const char*>(bytes),length);
  assert(line.front()=='{' && line.back()=='}');
  records.push_back(line);
  if (line.find("captured_before_clear")!=std::string::npos) trace.push_back("startup_capture");
  if (line.find("\"event\":\"shutdown\"")!=std::string::npos) events.push_back("shutdown_intent");
  return true;
}
}
struct FakeLog { void println(const char*){} } Log;
void showLowBatteryAndPowerOff() { ++lowShutdown; }
'''
for marker in (
    "static void recordPowerEvent(", "static void recordPowerIrq()",
    "static void showShutdownAndPowerOff()", "static bool batteryCriticallyLow()",
    "static void checkLowBatteryAndMaybeShutdown()", "static bool enableClientOled()",
    "static void wakeClientScreen()", "static void serviceClientPowerKey() {",
    "static void sleepClientOled()", "static void showClientBootScreen()",
):
    CPP += block(marker) + "\n"
CPP += "void poll() { serviceClientPowerKey(); }\n"
CPP += "void serviceWokenScreen() {\n" + block(
    "  if (clientOledAwake) {", SOURCE.index("// Keep the woken screen refreshed")
) + "\n}\n"
CPP += r'''
void resetFixture() {
  bootScreenTest=false;
  gpsTimes.clear(); usbTimes.clear(); diagTimes.clear(); infoTimes.clear(); sleepTimes.clear();
  clearTimes.clear(); oledReadyTimes.clear(); bootFrames=0; oledAvailable=true;
  displayCallsAfterPowerOff=0;
  timedIrqs.clear(); nextTimedIrq=0;
  faultRead=faultWrite=-1;
  std::memset(regs,0,sizeof(regs));
  writes=oledBegin=powerOff=lowShutdown=0;
  nowMs=1000; nextPmuKeyMs=0; clientOledAwake=false;
  cachedBatteryMv=4136; powerBootId=560532669;
  powerIrqState={}; powerIrqSample={}; powerVbusReadErrors=0;
  clientPowerKeys={}; clientPowerKeys.baselineReady=clientPowerKeys.armed=true;
  checkLowBatteryAndMaybeShutdown(); // clear the actual static low debounce
  recordPowerIrq(); // reset error-log state by observing a healthy snapshot
  poll(); // observe zero IRQs to reset startup capture change detection
  nextPmuKeyMs=0;
  events.clear(); records.clear(); trace.clear();
}
void tick() { nowMs+=100; poll(); }
void startup(uint8_t irq) {
  regs[XPOWERS_AXP2101_INTSTS2]=irq; nowMs+=100; serviceClientPowerKey();
}
void report(const char* name) { std::printf("PASS %s\n",name); }
int main() {
  resetFixture(); clientPowerKeys={}; bootScreenTest=true;
  const uint32_t screenStarted=millis();
  regs[XPOWERS_AXP2101_INTSTS2]=0x47;
  showClientBootScreen();
  assert(!powerOff && clientPowerKeys.baselineReady && !clientPowerKeys.armed);
  assert(millis()-screenStarted>=13400 && millis()-screenStarted<13600);
  assert(bootFrames>=30 && infoTimes.size()>=9 && sleepTimes.size()==1);
  assert(oledReadyTimes.size()==1 && oledReadyTimes[0]-screenStarted==400);
  assert(clearTimes.front()==oledReadyTimes[0]); // first animation frame elapsed=0
  assert(clearTimes[clearTimes.size()-2]-oledReadyTimes[0]>=2900); // final entry is sleep clear
  assert(clearTimes[clearTimes.size()-2]-oledReadyTimes[0]<3000);
  assert(infoTimes.front()-oledReadyTimes[0]>=3000 && infoTimes.front()-oledReadyTimes[0]<3200);
  assert(infoTimes.back()-oledReadyTimes[0]>=12000);
  assert(gpsTimes.size()>1000 && usbTimes.size()==gpsTimes.size() && diagTimes.size()==gpsTimes.size());
  // OLED init contains the established 400 ms delay. After that, 90 ms fake
  // full-frame writes must never starve GPS/USB/diagnostic service for 200 ms.
  for(size_t i=2;i<gpsTimes.size();++i) assert(gpsTimes[i]-gpsTimes[i-1]<200);
  assert(trace[0]=="startup_capture" && trace[1]=="clear_irq");
  bootScreenTest=false;
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kNegative; tick();
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kLong; tick(); assert(powerOff==1);
  report("actual boot loop services GPS USB diagnostics and startup keys through 3 s animation plus 10 s status; next long works");

  resetFixture(); clientPowerKeys={}; bootScreenTest=true; oledAvailable=false;
  const uint32_t failedScreenStarted=millis(); showClientBootScreen();
  assert(millis()-failedScreenStarted>=13000 && millis()-failedScreenStarted<13400);
  assert(oledBegin>1 && oledReadyTimes.empty() && clearTimes.empty() && infoTimes.empty() && sleepTimes.empty());
  assert(gpsTimes.size()>1000 && usbTimes.size()==gpsTimes.size() && diagTimes.size()==gpsTimes.size());
  assert(clientPowerKeys.baselineReady && !clientPowerKeys.armed && !powerOff);
  report("unavailable OLED retries remain bounded while GPS USB diagnostics and startup key capture continue");

  resetFixture(); clientPowerKeys={}; bootScreenTest=true;
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kNegative;
  timedIrqs={{1700,power_irq::kLong},{5000,power_irq::kLong}};
  const uint32_t heldStarted=millis(); showClientBootScreen();
  assert(!powerOff && !clientPowerKeys.armed && clientPowerKeys.baselineReady);
  assert(millis()-heldStarted>=13400 && infoTimes.size()>=9);
  report("initial key held through animation and info never arms from quiet snapshots or long-only latches");

  for (uint32_t newPressMs : {1800U,5000U}) {
    resetFixture(); clientPowerKeys={}; bootScreenTest=true;
    regs[XPOWERS_AXP2101_INTSTS2]=0x47; // observed initial boot latch is not a command
    timedIrqs={{1500,power_irq::kPositive},{newPressMs,power_irq::kNegative},
               {newPressMs+1000,power_irq::kLong}};
    const uint32_t intentionalStarted=millis(); showClientBootScreen();
    assert(powerOff==1 && clientPowerKeys.armed);
    assert(millis()-intentionalStarted<10000 && displayCallsAfterPowerOff==0);
    assert(nextTimedIrq==timedIrqs.size());
    if (newPressMs==1800) assert(infoTimes.empty());
    else assert(!infoTimes.empty() && infoTimes.back()<newPressMs+1200);
    assert(records.size()>0 && events.back()=="power_off");
  }
  report("released startup key then fresh long works during animation and info; PMU return cannot redraw boot page");

  resetFixture(); clientPowerKeys={}; bootScreenTest=true;
  timedIrqs={{1800,power_irq::kNegative},{2800,power_irq::kLong}};
  showClientBootScreen();
  assert(powerOff==1 && clientPowerKeys.armed && displayCallsAfterPowerOff==0);
  report("USB auto-start with quiet initial baseline accepts first actual new press without guessing release from silence");

  resetFixture(); clientPowerKeys={}; startup(0x07);
  startup(power_irq::kPositive); startup(power_irq::kNegative); startup(0x09);
  assert(clientPowerKeys.armed && oledBegin==1 && clientOledAwake && !powerOff);
  report("startup release then new short is actionable before boot display completes");

  for (uint32_t shortPressMs : {1900U,12100U}) {
    resetFixture(); clientPowerKeys={}; bootScreenTest=true;
    timedIrqs={{shortPressMs-100,power_irq::kNegative},{shortPressMs,uint8_t(0x09)}};
    showClientBootScreen();
    assert(!powerOff && clientPowerKeys.armed && nextTimedIrq==timedIrqs.size());
    const uint32_t wakeDeadline=clientOledOffMs;
    if (shortPressMs==12100) {
      assert(clientOledAwake && sleepTimes.empty() && millis()<wakeDeadline);
      serviceWokenScreen();
      nowMs=wakeDeadline-1;
      serviceWokenScreen();
      assert(clientOledAwake && sleepTimes.empty());
      if (nowMs<wakeDeadline) nowMs=wakeDeadline;
      serviceWokenScreen();
      assert(!clientOledAwake && sleepTimes.size()==1);
      assert(sleepTimes.front()>=wakeDeadline && sleepTimes.front()<=wakeDeadline+200);
    } else {
      assert(!clientOledAwake && sleepTimes.size()==1 && millis()>=wakeDeadline);
    }
    nowMs+=10000; serviceWokenScreen();
    assert(!clientOledAwake && sleepTimes.size()==1); // no duplicate or indefinite wake
  }
  report("boot short press retains its ten-second deadline; late press sleeps once at runtime expiry and early expiry stays asleep");

  for (uint8_t bootLatch : {uint8_t(0x47), uint8_t(0x07)}) {
    resetFixture(); clientPowerKeys={}; startup(bootLatch);
    assert(!powerOff && !oledBegin && !clientPowerKeys.armed);
    assert(trace.size()>=2 && trace[0]=="startup_capture" && trace[1]=="clear_irq");
    assert(records[0].find("\"irq_raw\":[0,"+std::to_string(bootLatch)+",0]")!=std::string::npos);
    assert(records[0].find("\"irq_clear\":[-2,-2,-2]")!=std::string::npos);
    for (int i=0;i<100;++i) startup(0);
    tick(); assert(clientPowerKeys.baselineReady && !clientPowerKeys.armed && !powerOff);
    regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kLong; tick();
    assert(!powerOff && !clientPowerKeys.armed); // original key can still be held
    regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kPositive; tick();
    assert(!clientPowerKeys.armed);
    regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kNegative; tick();
    assert(clientPowerKeys.armed && !powerOff);
    regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kLong; tick();
    assert(powerOff==1);
  }
  report("both observed battery boot latches are recorded before clear and cannot shut down; release then new long works");

  resetFixture(); clientPowerKeys={}; startup(power_irq::kNegative);
  startup(power_irq::kLong); tick(); // baseline while original key remains held
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kLong; tick(); assert(!powerOff);
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kPositive; tick();
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kNegative; tick();
  regs[XPOWERS_AXP2101_INTSTS2]=0x09; tick(); // short+release arrive separately from press
  assert(clientPowerKeys.armed && clientOledAwake && oledBegin==1 && !powerOff);
  report("key held across boot stays guarded; next separate press then raw 0x09 short wakes normally");

  resetFixture(); clientPowerKeys={};
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kNegative|power_irq::kLong;
  tick(); assert(clientPowerKeys.baselineReady && !clientPowerKeys.armed && !powerOff);
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kPositive; tick();
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kNegative|power_irq::kLong; tick();
  assert(clientPowerKeys.armed && powerOff==1);
  report("first successful snapshot suppresses every old key bit; later combined new press and long is accepted");

  for (int failingReg=XPOWERS_AXP2101_INTSTS1; failingReg<=XPOWERS_AXP2101_INTSTS3; ++failingReg) {
    resetFixture(); clientPowerKeys={}; faultRead=failingReg; startup(0x07);
    assert(!writes && !powerOff && !clientPowerKeys.baselineReady);
    tick(); assert(!clientPowerKeys.baselineReady && !clientPowerKeys.armed);
    faultRead=-1; tick();
    assert(clientPowerKeys.baselineReady && !clientPowerKeys.armed && !powerOff);
    regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kNegative; tick();
    regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kLong; tick(); assert(powerOff==1);
  }
  report("each failed startup read keeps guard; recovery captures baseline before accepting a later new gesture");

  resetFixture(); clientPowerKeys={}; faultWrite=XPOWERS_AXP2101_INTSTS2;
  startup(0x07); tick(); assert(!clientPowerKeys.baselineReady && !powerOff);
  for (int i=0;i<20;++i) tick();
  assert(!clientPowerKeys.armed && !powerOff);
  faultWrite=-1; tick(); assert(clientPowerKeys.baselineReady && !clientPowerKeys.armed);
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kNegative; tick();
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kLong; tick(); assert(powerOff==1);
  report("startup clear failure cannot arm keys; successful retry and a new press recover");

  resetFixture(); clientPowerKeys={}; startup(0); tick();
  assert(clientPowerKeys.baselineReady);
  faultRead=XPOWERS_AXP2101_INTSTS2;
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kNegative; tick();
  assert(!clientPowerKeys.baselineReady && !clientPowerKeys.armed);
  faultRead=-1; tick(); assert(clientPowerKeys.baselineReady && !clientPowerKeys.armed);
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kPositive; tick();
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kNegative; tick();
  assert(clientPowerKeys.armed);
  faultRead=XPOWERS_AXP2101_INTSTS1; tick(); assert(clientPowerKeys.armed);
  faultRead=-1; regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kLong; tick(); assert(powerOff==1);
  report("waiting guard re-baselines after bus error; already armed runtime remains recoverable without another edge");

  resetFixture(); regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kLong;
  auto stationSample=power_irq::poll(pmu,powerIrqState);
  assert(stationSample.longPress && !stationSample.shortPress);
  report("Station generic IRQ poll still accepts a normal long latch without Client startup policy");

  resetFixture(); poll(); assert(!powerOff && !oledBegin && !writes); report("quiet snapshot does not write IRQs");
  for (int reg=XPOWERS_AXP2101_INTSTS1;reg<=XPOWERS_AXP2101_INTSTS3;++reg) {
    resetFixture(); faultRead=reg; poll();
    assert(!powerOff && !oledBegin && !writes && powerIrqState.readErrors==1);
    assert(powerIrqSample.raw[reg-XPOWERS_AXP2101_INTSTS1]==-1);
  }
  report("each signed IRQ read failure rejected without clear or key action");
  resetFixture(); faultRead=-2; poll();
  assert(!powerOff && !oledBegin && powerIrqSample.readFailed==7); report("all IRQ reads failed");

  resetFixture(); faultRead=XPOWERS_AXP2101_INTSTS1;
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kLong; poll();
  assert(!powerOff && regs[XPOWERS_AXP2101_INTSTS2]==power_irq::kLong);
  faultRead=-1; tick(); assert(powerOff==1); report("incomplete snapshot preserves a real long key for retry");

  resetFixture(); regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kShort; poll();
  assert(oledBegin==1 && clientOledAwake && !powerOff && nowMs==1400); report("normal short key still wakes OLED");

  resetFixture(); regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kLong; poll();
  assert(powerOff==1 && !clientOledAwake && oledBegin==1);
  assert(events[0]=="shutdown_intent" && events[1]=="sd_stop" && events[2]=="oled_begin");
  report("normal long key records intent before drain and OLED");

  resetFixture(); diagnosticRaw[0]='$'; diagnosticRawLength=1;
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kLong; poll();
  assert(events[0]=="shutdown_intent" && events[1]=="raw_tail" && events[2]=="sd_stop");
  assert(diagnosticRawLength==0); report("shutdown preserves pending raw tail after intent and before SD drain");

  resetFixture(); regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kShort|power_irq::kLong; poll();
  assert(powerOff==1 && !clientOledAwake && oledBegin==1 && nowMs==2900);
  assert(events[0]=="shutdown_intent"); report("combined short and long never wakes OLED before intent");

  resetFixture(); faultWrite=XPOWERS_AXP2101_INTSTS2;
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kShort; poll();
  for (int i=0;i<20;++i) tick();
  assert(oledBegin==1 && !powerOff && powerIrqState.clearErrors==21 && powerIrqState.suppressedKeys==20);
  faultRead=XPOWERS_AXP2101_INTSTS2; tick(); faultRead=-1; tick();
  assert(oledBegin==1); // an intervening read error cannot forget the consumed key
  faultWrite=-1; tick(); assert(oledBegin==1 && regs[XPOWERS_AXP2101_INTSTS2]==0);
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kShort; tick(); assert(oledBegin==2);
  report("clear failure retries without replay; recovery permits a later new key");

  resetFixture(); faultWrite=XPOWERS_AXP2101_INTSTS2;
  regs[XPOWERS_AXP2101_INTSTS2]=power_irq::kShort; poll();
  regs[XPOWERS_AXP2101_INTSTS2]|=power_irq::kLong; tick();
  assert(powerOff==1); tick(); assert(powerOff==1);
  report("new long bit during failed short clear is delivered once");

  resetFixture(); faultRead=XPOWERS_AXP2101_INTSTS2; poll();
  const size_t before=records.size();
  for(int i=0;i<90;++i) tick();
  assert(records.size()==before && powerIrqState.readErrors==91);
  for(int i=0;i<10;++i) tick();
  assert(records.size()==before+1 && powerIrqState.readErrors==101);
  faultRead=-1; tick(); assert(records.size()==before+2);
  report("stable IRQ error storm reports first, ten-second summary and recovery");

  resetFixture(); cachedBatteryMv=3199;
  regs[XPOWERS_AXP2101_STATUS1]=(1<<5)|(1<<3);
  checkLowBatteryAndMaybeShutdown(); checkLowBatteryAndMaybeShutdown(); assert(!lowShutdown);
  faultRead=XPOWERS_AXP2101_STATUS1;
  checkLowBatteryAndMaybeShutdown(); checkLowBatteryAndMaybeShutdown(); assert(!lowShutdown);
  faultRead=XPOWERS_AXP2101_STATUS2;
  checkLowBatteryAndMaybeShutdown(); checkLowBatteryAndMaybeShutdown(); assert(!lowShutdown);
  report("VBUS read failures cannot turn plugged-in low cached voltage into shutdown");
  faultRead=-1; regs[XPOWERS_AXP2101_STATUS1]=1<<3;
  checkLowBatteryAndMaybeShutdown(); assert(!lowShutdown);
  checkLowBatteryAndMaybeShutdown(); assert(lowShutdown==1); report("known battery-only low voltage keeps debounce behavior");

  resetFixture(); powerBootId=nowMs=powerIrqMs=UINT32_MAX;
  powerIrqState.readErrors=powerIrqState.clearErrors=powerIrqState.suppressedKeys=powerVbusReadErrors=UINT32_MAX;
  cachedBatteryMv=UINT16_MAX;
  // Even arbitrary signed transport failures must not truncate the evidence.
  for(int i=0;i<3;++i) powerIrqSample.raw[i]=powerIrqSample.clear[i]=INT32_MIN;
  powerVbusRaw[0]=powerVbusRaw[1]=INT32_MIN;
  powerIrqSample.readFailed=powerIrqSample.clearFailed=powerIrqSample.suppressed=UINT8_MAX;
  recordPowerEvent("shutdown", "pwr_long_press");
  assert(records.back().find("4294967295")!=std::string::npos);
  std::printf("POWER_EVIDENCE_JSON %s\n",records.back().c_str());
  std::printf("PASS power evidence complete JSON and Flash codec round trip, max-width length=%zu bytes, record limit=%zu\n",
      records.back().size(),diagnostic_store::codec::kMaxRecordBytes);
}
'''

with tempfile.TemporaryDirectory(prefix="shore-power-regression-") as directory:
    source = Path(directory) / "probe.cpp"
    binary = Path(directory) / "probe"
    source.write_text(CPP)
    subprocess.run([
        "g++", "-std=c++17", "-Dlinux", "-Wall", "-Wextra", "-I", str(ROOT / "include"),
        "-I", str(XP), str(source), str(XP / "XPowersLibInterface.cpp"), "-o", str(binary),
    ], check=True)
    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
    for line in result.stdout.splitlines():
        if line.startswith("POWER_EVIDENCE_JSON "):
            evidence = json.loads(line.removeprefix("POWER_EVIDENCE_JSON "))
            assert evidence["boot_id"] == evidence["vbus_read_errors"] == 4294967295
            assert evidence["irq_raw"] == evidence["irq_clear"] == [-2147483648] * 3
            assert evidence["battery_mv"] == 65535
        else:
            print(line)
