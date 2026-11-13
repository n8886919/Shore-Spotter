#!/usr/bin/env python3
"""Exercise actual T096 power/display functions with ADC, GPIO and TFT fakes."""
from pathlib import Path
import subprocess
import tempfile
from test_t096_gnss_uart import extract_function

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/t096_client.cpp').read_text()
names = ['void captureClientMac()', 'void setGnssEnabled(bool enabled)',
         'void wakeClientDisplay(uint32_t now)', 'void initClientDisplay()',
         'void formatMaintenanceLine(char *line, size_t size, uint8_t page, uint8_t row, uint32_t now)',
         'void serviceClientDisplay(uint32_t now)', 'void serviceBattery(uint32_t now)',
         'void serviceUserButton(uint32_t now)', 'void serviceLowBattery(uint32_t now)']
actual = '\n'.join(extract_function(source, name) for name in names)
cpp = r'''
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include "t096_power.h"
#include "t096_maintenance.h"
#include "firmware_version.h"
#include "tracking_policy.h"
#include "t096_control.h"
#include "t096_pins.h"
#include "gnss_snapshot.h"
#include "gnss_rate.h"
#include "t096_gnss_config.h"
#include "geo_math.h"
constexpr int LOW=0,HIGH=1,OUTPUT=1;
constexpr uint8_t INITR_MINI160x80_PLUGIN=6,ST77XX_SLPOUT=17,ST77XX_DISPOFF=40,ST77XX_SLPIN=16,ST77XX_DISPON=41;
constexpr uint16_t ST77XX_BLACK=0,ST77XX_WHITE=65535,ST77XX_RED=1,ST77XX_GREEN=2,ST77XX_YELLOW=3;
uint32_t clockMs=100,screenUntilMs=0,displayWakeMs=0,nextDisplayMs=0,displayMaxMs=0;
uint32_t batterySampleMs=0,nextBatteryMs=0,nextTxStartMs=0,nextBleStatusMs=0,thisBoot=1;
bool userModePending=false,userModeActive=false;
int16_t lastRadioInitError=-23;
uint32_t millis(){return clockMs;}
int pins[48]={};unsigned adcReads=0;
uint16_t adcRaw=1143;bool usb=true;
void pinMode(int,int){}
void digitalWrite(int pin,int value){pins[pin]=value;}
int digitalRead(int pin){return pins[pin];}
uint16_t analogRead(int pin){assert(pin==3&&pins[47]==HIGH);++adcReads;return adcRaw;}
bool usbVbusPresent(){return usb;}
template<class... T>void usbLog(const char*,T...){}
bool gnssEnabled=false,displayPowered=false,displaySleeping=false,displayWaking=false;
bool batterySampling=false,batteryProtection=false;
bool dataWaiting=false,radioIrq=false,confirmationPending=false,outIsState=false,outOpensReceiveWindow=false;
uint32_t gpsLastServiceMs=0,nextExtraMs=0;uint8_t extraKind=0,displayRow=0;
uint16_t thisClientId=0xF035;size_t outLength=0;
enum class AirState{Idle,Sending,Receiving};AirState air=AirState::Idle;
struct Radio{void sleep(){}}radio;
unsigned femOffCalls=0,resetCalls=0;
void femOff(){++femOffCalls;pins[t096_pins::kFemPower]=LOW;}
void NVIC_SystemReset(){assert(!gnssEnabled && pins[t096_pins::kFemPower]==LOW);++resetCalls;}
bool bleReady=true,radioReady=true,radioSleeping=false;
bool radioInitSucceeds=true;unsigned radioInitCalls=0;
bool configureRadio(){++radioInitCalls;radioReady=radioInitSucceeds;return radioReady;}
uint32_t txPackets=0,txErrors=0;uint16_t rxErrors=0;
uint8_t displayPage=0;
const char *clientNotice=nullptr;uint32_t clientNoticeUntilMs=0;
char clientMacText[18]="MAC UNKNOWN";
constexpr uint32_t NRF_SUCCESS=0;
struct ble_gap_addr_t {uint8_t addr[6]{};};
uint32_t macResult=0;
uint32_t sd_ble_gap_addr_get(ble_gap_addr_t *p) {
 const uint8_t bytes[]={0xFF,0xEE,0xDD,0xCC,0xBB,0xAA};
 if(!macResult)memcpy(p->addr,bytes,6);
 return macResult;
}
t096_maintenance::Fault maintenanceFault;
struct BluefruitFake {
 struct {bool value=false;bool connected(){return value;}}Periph;
 struct {bool value=true;bool isRunning(){return value;}}Advertising;
}Bluefruit;
struct Collector{
 gnss_snapshot::Snapshot value{};bool have=true;
 void reset(uint32_t){}bool sample(uint32_t,gnss_snapshot::Snapshot &s){s=value;return have;}
}gnss;
struct Cadence{void reset(uint32_t){}}cadence;
struct Uart{bool begun=false;operator bool()const{return begun;}void begin(unsigned){begun=true;}void end(){assert(begun);begun=false;}}Serial2;
gnss_rate::Monitor gpsRate;t096_gnss::Setup gnssSetup;
t096_power::Battery battery;t096_power::Button userButton;
t096_control::Controller control(1);
struct DisplaySpi{bool begun=false;void begin(){begun=true;}}displaySpi;
struct Tft{
 std::vector<uint8_t> commands;std::string bootText;
 void setTextSize(unsigned size){assert(size==1);}
 void setTextColor(uint16_t){}void setCursor(int x,int y){assert(x==0 && y==0);}
 void print(const char *s){assert(strlen(s)<=26);bootText=s;}
 void initPanel(){assert(displaySpi.begun);}
 void invertDisplay(bool value){assert(value);}void setSPISpeed(unsigned hz){assert(hz==8000000);}
 void setTextWrap(bool v){assert(!v);}void fillScreen(uint16_t){}
 void fillRect(int x,int y,int w,int h,uint16_t c){assert(x==0&&y==78&&w==160&&h==2&&c==ST77XX_BLACK);}
 void sendCommand(uint8_t c){commands.push_back(c);}
 void drawRGBBitmap(int x,int y,uint16_t*,int w,int h){assert(x==0&&y>=0&&y+h<=80&&w==160);++clockMs;}
}tft;
struct Canvas{
 unsigned size=1;uint16_t color=0,buffer[2560];std::vector<std::string> lines;
 std::vector<uint16_t> colors;
 struct Rect {int x,y,w,h;};std::vector<Rect> rects;
 struct VLine {int x,y,h;};std::vector<VLine> vlines;
 void drawFastVLine(int x,int y,int h,uint16_t){
  assert(x==45 && y==1 && h==14);vlines.push_back({x,y,h});
 }
 void drawRect(int x,int y,int w,int h,uint16_t){
  assert(x>=0 && y>=0 && x+w<=160 && y+h<=16);rects.push_back({x,y,w,h});
 }
 uint16_t *getBuffer(){return buffer;}void fillScreen(uint16_t){}void setTextWrap(bool v){assert(!v);}
 void setTextSize(unsigned v){size=v;}void setTextColor(uint16_t v){color=v;}
 void setCursor(int x,int y){assert(x==0&&y>=0&&y+8*size<=16);}
 void print(const char *s){assert(strlen(s)*6*size<=160);lines.emplace_back(s);colors.push_back(color);}
}displayLine;
'''+actual+r'''
void sample(uint32_t now){clockMs=now;nextBatteryMs=now;serviceBattery(now);assert(pins[47]==HIGH);
 serviceBattery(now+9);clockMs=now+10;serviceBattery(clockMs);assert(pins[47]==LOW);}
std::string line(uint8_t page,uint8_t row,uint32_t now=10000) {
 char text[27];formatMaintenanceLine(text,sizeof(text),page,row,now);
 assert(strlen(text)<=26);return text;
}
void drawPage(uint8_t page,uint32_t now) {
 displayPage=page;displaySleeping=displayWaking=false;air=AirState::Idle;radioIrq=false;
 clockMs=now;wakeClientDisplay(now);const size_t first=displayLine.lines.size();
 const unsigned rows=page==1 && !userButton.previewing()?6:5;
 for(unsigned row=0;row<rows;++row){
  const auto expected=line(page,row,clockMs);serviceClientDisplay(clockMs);
  assert(displayLine.lines.back()==expected);
 }
 assert(displayLine.lines.size()==first+rows && pins[44]==LOW);
}
void edge(bool pressed,uint32_t now){clockMs=now;pins[42]=pressed?LOW:HIGH;serviceUserButton(now);}
void shortPress(uint32_t now){edge(true,now);edge(true,now+30);edge(false,now+31);edge(false,now+61);}
void claimAndStart() {
 control=t096_control::Controller(1);control.setClientId(thisClientId);
 assert(control.applyBle({ble_control::Op::Claim,1,thisClientId,65534,1,1,123},false)==t096_control::CommandResult::Applied);
 assert(control.applyBle({ble_control::Op::Start,1,thisClientId,65534,1,2,123},false)==t096_control::CommandResult::Applied);
 setGnssEnabled(true);
}
void assertTracking(){
 assert(control.state()==client_control::State::Tracking && control.station()==65534);
 assert(control.stationBoot()==123 && control.command()==2 && control.channel()==1 && gnssEnabled);
}
int main(){
 assert(line(0,0)=="CLIENT ID F035");assert(!strcmp(clientMacText,"MAC UNKNOWN"));
 macResult=7;captureClientMac();assert(line(0,0)=="CLIENT ID F035");assert(!strcmp(clientMacText,"MAC UNKNOWN"));
 assert(maintenanceFault.hasCode && maintenanceFault.code==7);
 macResult=0;captureClientMac();assert(!strcmp(clientMacText,"AA:BB:CC:DD:EE:FF"));
 thisClientId=0xFB27;assert(line(0,0)=="CLIENT ID FB27");thisClientId=0xF035;
 maintenanceFault={};
 // Independent voltage conversion and unknown/fault rejection.
 assert(t096_power::batteryMv(1143)==4102);
 assert(!t096_power::batteryMv(0)&&!t096_power::batteryMv(4095)&&!t096_power::batteryMv(4096));
 assert(t096_power::batteryPercent(4150)==100&&t096_power::batteryPercent(3200)==0);
 initClientDisplay();assert(displayPowered&&pins[26]==HIGH&&pins[44]==LOW);
 assert(tft.bootText=="CLIENT BOOTING" && line(0,0,clockMs)=="CLIENT BOOTING");
 assert(line(0,0,clientNoticeUntilMs)=="CLIENT ID F035");
 pins[42]=HIGH;userButton.begin(false,clockMs);
 for(unsigned i=0;i<5;++i)serviceClientDisplay(clockMs++);
 assert(pins[44]==LOW&&displayLine.lines.front()=="CLIENT BOOTING");
 sample(1000);assert(adcReads==5&&battery.mv()==4102);
 air=AirState::Sending;nextBatteryMs=2000;serviceBattery(2000);assert(adcReads==5);
 clockMs=screenUntilMs;serviceClientDisplay(clockMs);assert(pins[44]==HIGH&&!displaySleeping);
 air=AirState::Idle;serviceClientDisplay(clockMs);assert(displaySleeping&&tft.commands.back()==ST77XX_SLPIN);
 setGnssEnabled(true);wakeClientDisplay(clockMs);assert(gnssEnabled&&pins[26]==HIGH&&displayWaking);
 serviceClientDisplay(clockMs+119);assert(displayWaking&&pins[44]==HIGH);
 clockMs+=120;serviceClientDisplay(clockMs);assert(!displayWaking);
 for(unsigned i=0;i<5;++i)serviceClientDisplay(++clockMs);
 assert(pins[44]==LOW);clockMs=screenUntilMs;serviceClientDisplay(clockMs);
 assert(displaySleeping&&gnssEnabled&&pins[26]==HIGH);
 setGnssEnabled(false);assert(!gnssEnabled&&pins[6]==HIGH&&pins[26]==HIGH);
 // Short presses wake the home page for 60 s, then cycle exactly two pages.
 userButton.begin(false,10000);displayPage=1;
 shortPress(10000);assert(displayPage==0 && screenUntilMs==70061 && displayWaking);
 clockMs=10181;serviceClientDisplay(clockMs);
 shortPress(11000);assert(displayPage==1 && screenUntilMs==71061);
 shortPress(12000);assert(displayPage==0 && screenUntilMs==72061);
 // Deadline has expired even if RF delayed the controller's sleep command.
 displayPage=1;displaySleeping=false;screenUntilMs=13000;
 shortPress(14000);assert(displayPage==0 && screenUntilMs==74061);

 // Both power sources use the same preview/cancel/release-only mode-switch path.
 for(bool supplied:{true,false}) {
  usb=supplied;claimAndStart();confirmationPending=true;outLength=12;
  userButton.begin(false,16000);displaySleeping=false;screenUntilMs=100000;displayPage=0;
  shortPress(16000);assert(displayPage==1);assertTracking();
  edge(true,17000);edge(true,17030);edge(true,18030);
  assert(userButton.previewing());edge(false,18031);edge(false,18061);
  assert(!userButton.previewing() && displayPage==1);assertTracking();
  userButton.begin(false,20000);displayPage=1;
  const auto resets=resetCalls,offCalls=femOffCalls;
  edge(true,20000);edge(true,20030);edge(true,21029);
  assert(!userButton.previewing());assertTracking();
  edge(true,21030);assert(userButton.previewing() && !userButton.held());
  assert(line(0,0)=="SWITCH TO SLP" && line(0,1,21030)=="Hold to switch: 4s");
  assert(line(1,4)=="Release early to cancel");
  edge(true,24030);assert(line(0,1,24030)=="Hold to switch: 1s");assertTracking();
  // A 4999 ms hold must never gain the final millisecond during release debounce.
  edge(false,25029);edge(false,25059);
  assert(resetCalls==resets && femOffCalls==offCalls && !userButton.previewing());
  assertTracking();assert(confirmationPending && outLength==12 && displayPage==1);
  edge(true,26000);edge(true,26030);edge(true,31030);
  assert(userButton.held() && line(0,1)=="Release to switch");
  assert(line(1,4)=="Apply on release" && resetCalls==resets);assertTracking();
  // Keep showing the warning beyond the ordinary screen deadline.
  screenUntilMs=31000;displaySleeping=displayWaking=false;displayRow=0;nextDisplayMs=0;
  clockMs=100000;for(unsigned row=0;row<5;++row)serviceClientDisplay(clockMs);
  assert(!displaySleeping && pins[44]==LOW && displayLine.lines.back()=="Apply on release");
  assert(displayLine.colors.back()==ST77XX_YELLOW && resetCalls==resets);assertTracking();
  edge(false,100001);edge(false,100030);assert(resetCalls==resets);
  edge(false,100031);assert(resetCalls==resets && femOffCalls==offCalls+1 && !gnssEnabled);
  assert(control.state()==client_control::State::Ready && control.station()==65534);
  assert(control.stationBoot()==123 && control.command()==2 && control.channel()==1);
  assert(!confirmationPending && !outLength && !outIsState && !outOpensReceiveWindow);
  assert(nextBleStatusMs==100031 && displayPage==0);
  assert(line(0,0,100031)=="ENTERED SLP");
  assert(line(0,0,102031)=="CLIENT ID F035");
  edge(false,100100);assert(resetCalls==resets && control.state()==client_control::State::Ready);
  // Start again with the same owner/channel/session; no reboot or new claim.
  edge(true,101000);edge(true,101030);edge(true,102030);
  assert(line(0,0)=="SWITCH TO ACT" && line(0,2)=="GPS + LoRa ON");
  edge(false,106030);edge(false,106060);assertTracking();assert(confirmationPending);
  assert(resetCalls==resets && thisBoot==1);
  assert(line(0,0,106060)=="ACT STARTED");
 }
 // Initially pressed at boot cannot switch; release must arm a fresh press.
 userButton.begin(true,0xfffffff0u);const auto bootResets=resetCalls;
 edge(true,6000);assert(!userButton.previewing() && resetCalls==bootResets);
 edge(false,6001);edge(false,6031);assert(resetCalls==bootResets);
 shortPress(7000);assert(resetCalls==bootResets);
 // Wrap-safe debounce, preview, cancellation and exact-threshold release.
 using E=t096_power::ButtonEvent;
 for(uint32_t start:{100u,0xfffffff0u}) {
  t096_power::Button b;b.begin(false,start-100);
  assert(b.poll(true,start)==E::None);assert(b.poll(true,start+30)==E::None);
  assert(b.poll(true,start+1029)==E::None);
  assert(b.poll(true,start+1030)==E::Preview && b.remainingSeconds(start+1030)==4);
  assert(b.poll(false,start+5029)==E::None);
  assert(b.poll(false,start+5059)==E::Cancelled && !b.busy());
  b.begin(false,start-100);b.poll(true,start);b.poll(true,start+30);
  // The exact release threshold also works without a poll at the Hold deadline.
  assert(b.poll(false,start+5030)==E::None);
  assert(b.poll(false,start+5060)==E::LongReleased);
  assert(b.poll(false,start+5090)==E::None);
 }
 // Press/release bounce cannot manufacture an action or duplicate one.
 t096_power::Button b;b.begin(false,0);
 assert(b.poll(true,10)==E::None);assert(b.poll(false,25)==E::None);
 assert(b.poll(false,60)==E::None && !b.busy());
 b.poll(true,100);b.poll(true,130);
 b.poll(false,200);b.poll(true,210);b.poll(false,220);
 assert(b.poll(false,249)==E::None);assert(b.poll(false,250)==E::Short);
 assert(b.poll(false,300)==E::None);

 // Unclaimed local ACT, RF-busy deferral, Test -> SLP, battery gate and radio errors.
 auto longPress=[](uint32_t now){userButton.begin(false,now);edge(true,now);
   edge(true,now+30);edge(false,now+5030);edge(false,now+5060);};
 control=t096_control::Controller(1);control.setClientId(thisClientId);setGnssEnabled(false);
 air=AirState::Idle;radioReady=true;batteryProtection=false;
 longPress(110000);
 assert(control.state()==client_control::State::Tracking && !control.hasStation());
 assert(control.channel()==0 && control.command()==0 && gnssEnabled && confirmationPending);
 // First Claim during local ACT preserves Tracking and current channel.
 assert(control.applyBle({ble_control::Op::Claim,1,thisClientId,65534,1,1,123},false)==t096_control::CommandResult::Applied);
 assert(control.state()==client_control::State::Tracking && control.channel()==0);
 for(auto busy:{AirState::Sending,AirState::Receiving}) {
   air=busy;outLength=18;outIsState=outOpensReceiveWindow=true;
   const auto off=femOffCalls;longPress(120000);
   assert(userModePending && gnssEnabled && outLength==18 && femOffCalls==off);
   air=AirState::Idle;edge(false,126000);
   assert(!userModePending && !gnssEnabled && !outLength && !confirmationPending);
   assert(control.state()==client_control::State::Ready && control.station()==65534);
   edge(false,126100);assert(control.state()==client_control::State::Ready);
   longPress(130000);assert(gnssEnabled);
 }
 control.requestReady();
 assert(control.applyBle({ble_control::Op::Test,1,thisClientId,65534,1,2,123},false)==t096_control::CommandResult::Applied);
 setGnssEnabled(false);longPress(140000);
 assert(control.state()==client_control::State::Ready && !gnssEnabled);
 batteryProtection=true;longPress(150000);
 assert(control.state()==client_control::State::Ready && !gnssEnabled && !confirmationPending);
 assert(!strcmp(maintenanceFault.source,"USER BAT"));
 assert(line(0,0,155060)=="ACT BLOCKED: LOW BAT");
 batteryProtection=false;radioReady=false;radioInitSucceeds=false;
 const auto initBefore=radioInitCalls;longPress(160000);
 assert(radioInitCalls==initBefore+1 && control.state()==client_control::State::Ready && !gnssEnabled);
 assert(line(0,0,165060)=="ACT FAILED: RF ERROR");
 radioInitSucceeds=true;longPress(170000);
 assert(radioInitCalls==initBefore+2 && control.state()==client_control::State::Tracking && gnssEnabled);
 maintenanceFault={};assert(resetCalls==0);
 // Notice deadlines wrap, and short press restores the requested page immediately.
 clientNotice="ENTERED SLP";clientNoticeUntilMs=20;
 assert(line(0,0,0xfffffff0u)=="ENTERED SLP");
 assert(line(0,0,20)=="CLIENT ID F035");
 shortPress(180000);assert(clientNotice==nullptr);

 // Exercise actual formatter and renderer: full BLE address, both mode labels,
 // hex owner, single GPS detail row, and no USB or duplicated GPS on diagnostics.
 userButton.begin(false,0);control=t096_control::Controller(1);setGnssEnabled(false);
 battery=t096_power::Battery{};usb=false;
 assert(line(0,1)==" sleep   active" && line(0,2)=="OWNER NONE, BLE ADV");
 assert(line(0,3)=="GPS OFF RF ON" && line(0,4)=="BAT UNKNOWN UP 10s");
 displayLine.rects.clear();displayLine.vlines.clear();drawPage(0,120000);
 assert(displayLine.vlines.size()==1);
 assert(displayLine.rects.size()==1 && displayLine.rects.back().x==3 && displayLine.rects.back().w==36);
 claimAndStart();assert(line(0,2)=="OWNER FFFE, BLE ADV");
 displayLine.rects.clear();drawPage(0,120100);
 assert(displayLine.rects.size()==1 && displayLine.rects.back().x==51 && displayLine.rects.back().w==42);
 assert(control.applyBle({ble_control::Op::Test,1,thisClientId,65534,1,3,123},false)==t096_control::CommandResult::Applied);
 setGnssEnabled(false);displayLine.rects.clear();drawPage(0,120200);
 assert(displayLine.rects.size()==1 && displayLine.rects.back().x==51 && line(0,3)=="GPS OFF RF ON");
 displayLine.rects.clear();displayLine.vlines.clear();drawPage(1,120300);assert(displayLine.rects.empty() && displayLine.vlines.empty());
 for(unsigned i=0;i<6;++i)assert(line(1,i).find("GPS")==std::string::npos);
 claimAndStart();Bluefruit.Periph.value=true;assert(line(0,2)=="OWNER FFFE, BLE LINK");
 Bluefruit.Periph.value=false;assert(line(0,2)=="OWNER FFFE, BLE ADV");
 Bluefruit.Advertising.value=false;assert(line(0,2)=="OWNER FFFE, BLE OFF");
 bleReady=false;drawPage(0,120400);assert(line(0,2)=="OWNER FFFE, BLE ERR");
 assert(displayLine.colors[displayLine.colors.size()-3]==ST77XX_RED);
 bleReady=true;Bluefruit.Advertising.value=true;
 gnss.have=false;assert(line(0,3)=="GPS NO EPOCH RF ON");
 gnss.have=true;gnss.value={};gnss.value.haveGga=true;gnss.value.arrivalAgeMs=123;
 gnss.value.satellites=12;gnss.value.hdop=1.25;
 assert(line(0,3)=="GPS NOFIX RF ON" && line(0,4)=="BAT UNKNOWN UP 10s" && line(1,3)=="SAT 12" && line(1,4)=="AGE 0.1s  HDOP 1.2");
 gnss.value.fix=true;assert(line(0,3)=="GPS FIX RF ON" && line(0,4)=="BAT UNKNOWN UP 10s" && line(1,3)=="SAT 12" && line(1,4)=="AGE 0.1s  HDOP 1.2");
 gnss.value.satellites=gnss_snapshot::kUnknownSatellites;
 assert(line(1,3)=="SAT --" && line(1,4)=="AGE 0.1s  HDOP 1.2");
 gnss.value.haveGga=false;assert(line(1,3)=="SAT --" && line(1,4)=="AGE 0.1s  HDOP --");
 gnss.value.haveGga=true;
 for(double hdop:{double(NAN),-1.0,999.95,1000.0,double(INFINITY)}) {
  gnss.value.hdop=hdop;assert(line(1,3)=="SAT --" && line(1,4)=="AGE 0.1s  HDOP --");
 }
 gnss.value.hdop=0;assert(line(1,4)=="AGE 0.1s  HDOP 0.0");
 gnss.value.fix=false;gnss.value.satellites=254;gnss.value.hdop=999.94;gnss.value.arrivalAgeMs=1999;
 assert(line(0,3)=="GPS NOFIX RF ON" && line(1,3)=="SAT 254" && line(1,4)=="AGE 2.0s  HDOP 999.9");
 battery.observe(4500,false);assert(line(0,4)=="BAT 4.50V UP 10s" && line(0,4).size()<=26);
 battery=t096_power::Battery{};
 gnss.value.arrivalAgeMs=tracking_policy::kGpsFreshMs;
 assert(line(0,3)=="GPS STALE RF ON" && line(1,3)=="SAT --" && line(1,4)=="AGE 2s  HDOP --");
 setGnssEnabled(false);assert(line(0,3)=="GPS OFF RF ON");
 assert(line(1,3)=="SAT --" && line(1,4)=="AGE --  HDOP --");
 assert(line(1,5)=="LAST NONE");maintenanceFault.record("RF RX",-7);
 assert(line(1,5)=="LAST RF RX -7");
 radioReady=false;assert(line(0,3)=="GPS OFF RF ERR");
 radioReady=true;radioSleeping=true;assert(line(0,3)=="GPS OFF RF OFF");
 radioSleeping=false;air=AirState::Sending;assert(line(0,3)=="GPS OFF RF ON");
 air=AirState::Receiving;assert(line(0,3)=="GPS OFF RF ON");air=AirState::Idle;
 drawPage(1,121000);assert(line(1,5)=="LAST RF RX -7");
 assert(displayLine.colors.back()==ST77XX_RED);
 maintenanceFault.record("BLE QUE");assert(line(1,5)=="LAST BLE QUE FAIL");
 maintenanceFault.record("RF TXTO",0);assert(line(1,5)=="LAST RF TXTO 0");
 maintenanceFault.record("BLE GATT",INT32_MIN);assert(line(1,5)=="LAST BLE GATT -2147483648");
 txPackets=UINT32_MAX;txErrors=UINT32_MAX;rxErrors=UINT16_MAX;
 assert(line(1,1)=="TX 999999+" && line(1,2)=="ERR TX99999+ RX65535");
 assert(txPackets==UINT32_MAX && txErrors==UINT32_MAX && rxErrors==UINT16_MAX);
 setGnssEnabled(true);gnss.value.arrivalAgeMs=UINT32_MAX;
 for(const char *fault:{"RF RXST","RF TXST","RF FRAME","RF RXIRQ","RF TXIRQ",
      "RF TXTO","RF INIT","RF SLEEP","BLE QUE","BLE INIT","BLE SVC",
      "BLE GATT","BLE STAT","BLE DATA","BLE ADV","BLE CMD","BLE MAC"}) {
  for(int32_t code:{INT32_MIN,INT32_MAX,0,-7}) {
   maintenanceFault.record(fault,code);drawPage(0,UINT32_MAX-1000);drawPage(1,UINT32_MAX-1000);
  }
 }
 for(bool powered:{false,true}) {
  usb=powered;for(unsigned page=0;page<2;++page)for(unsigned row=0;row<(page?6:5);++row)
   assert(line(page,row).find("USB")==std::string::npos);
 }
 maintenanceFault={};setGnssEnabled(false);control.requestReady();
 confirmationPending=false;outLength=0;
 // Healthy Ready samples every 30 s; warning/active samples every second.
 usb=false;adcRaw=1143;sample(10000);assert(nextBatteryMs==40010);
 adcRaw=960;sample(11000);assert(nextBatteryMs==12010);
 drawPage(0,11010);assert(displayLine.colors.back()==ST77XX_YELLOW);
 assert(line(0,4)=="BAT 3.44V UP 10s");usb=true;assert(line(0,4)=="BAT 3.44V UP 10s");usb=false;
 adcRaw=860;sample(13000);sample(14000);assert(!battery.protectedSleep());
 sample(15000);assert(battery.protectedSleep()&&nextBatteryMs==75010);
 clockMs=15010;serviceLowBattery(clockMs);
 assert(line(0,4)=="LOW3.09V UP10s CHARGE");
 assert(line(0,4,UINT32_MAX)=="LOW3.09V UP4294967s CHARGE");
 assert(screenUntilMs==clockMs+t096_power::kAutomaticScreenMs);
 assert(batteryProtection&&!gnssEnabled&&!confirmationPending&&pins[30]==LOW);
 clockMs+=120;serviceClientDisplay(clockMs);
 clockMs=screenUntilMs;serviceClientDisplay(clockMs);
 assert(displaySleeping&&pins[44]==HIGH); // low battery does not hold backlight on.
 // Short USER still displays the cause, but cannot resume RF/GPS in protection.
 userButton.begin(false,clockMs);pins[42]=LOW;serviceUserButton(clockMs);
 serviceUserButton(clockMs+30);pins[42]=HIGH;serviceUserButton(clockMs+31);serviceUserButton(clockMs+61);
 assert(!displaySleeping&&batteryProtection&&!gnssEnabled&&!confirmationPending);
 // Unknown samples and USB presence alone do not clear the protected latch.
 adcRaw=0;usb=true;sample(80000);serviceLowBattery(clockMs);
 assert(batteryProtection&&!battery.mv());
 adcRaw=980;sample(140000);serviceLowBattery(clockMs);assert(batteryProtection);
 adcRaw=1000;sample(200000);serviceLowBattery(clockMs);assert(batteryProtection);
 adcRaw=960;sample(260000);serviceLowBattery(clockMs);assert(batteryProtection);
 adcRaw=1000;sample(320000);sample(380000);assert(battery.protectedSleep());
 sample(440000);serviceLowBattery(clockMs);
 assert(!batteryProtection&&!gnssEnabled&&!confirmationPending&&control.state()==client_control::State::Ready);
 assert(nextBatteryMs==470010); // recovery remains Ready and does not auto-track.

}
'''
with tempfile.TemporaryDirectory() as td:
    p=Path(td);(p/'test.cpp').write_text(cpp)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS actual T096 maintenance pages/USER release-only ACT/SLP including unclaimed/busy/recovery, ADC and low-battery recovery')
