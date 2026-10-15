#!/usr/bin/env python3
"""Exercise actual T096 power/display functions with ADC, GPIO and TFT fakes."""
from pathlib import Path
import subprocess
import tempfile
from test_t096_gnss_uart import extract_function

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/t096_client.cpp').read_text()
names = ['void setGnssEnabled(bool enabled)',
         'void wakeClientDisplay(uint32_t now)', 'void initClientDisplay()',
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
uint32_t batterySampleMs=0,nextBatteryMs=0,nextTxStartMs=0;
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
struct Radio{void sleep(){}}radio;void femOff(){}
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
 std::vector<uint8_t> commands;
 void initPanel(){assert(displaySpi.begun);}
 void invertDisplay(bool value){assert(value);}void setSPISpeed(unsigned hz){assert(hz==8000000);}
 void setTextWrap(bool v){assert(!v);}void fillScreen(uint16_t){}
 void sendCommand(uint8_t c){commands.push_back(c);}
 void drawRGBBitmap(int x,int y,uint16_t*,int w,int h){assert(x==0&&y>=0&&y+h<=80&&w==160);++clockMs;}
}tft;
struct Canvas{
 unsigned size=1;uint16_t buffer[2560];std::vector<std::string> lines;
 uint16_t *getBuffer(){return buffer;}void fillScreen(uint16_t){}void setTextWrap(bool v){assert(!v);}
 void setTextSize(unsigned v){size=v;}void setTextColor(uint16_t){}
 void setCursor(int x,int y){assert(x==0&&y>=0&&y+8*size<=16);}
 void print(const char *s){assert(strlen(s)*6*size<=160);lines.emplace_back(s);}
}displayLine;
'''+actual+r'''
void sample(uint32_t now){clockMs=now;nextBatteryMs=now;serviceBattery(now);assert(pins[47]==HIGH);
 serviceBattery(now+9);clockMs=now+10;serviceBattery(clockMs);assert(pins[47]==LOW);}
int main(){
 // Independent voltage conversion and unknown/fault rejection.
 assert(t096_power::batteryMv(1143)==4102);
 assert(!t096_power::batteryMv(0)&&!t096_power::batteryMv(4095)&&!t096_power::batteryMv(4096));
 assert(t096_power::batteryPercent(4150)==100&&t096_power::batteryPercent(3200)==0);
 initClientDisplay();assert(displayPowered&&pins[26]==HIGH&&pins[44]==HIGH);
 pins[42]=HIGH;userButton.begin(false,clockMs);
 for(unsigned i=0;i<5;++i)serviceClientDisplay(clockMs++);
 assert(pins[44]==LOW&&displayLine.lines.front()=="Client F035");
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
 // Held wake button cannot immediately put the new boot back to sleep.
 t096_power::Button button;button.begin(true,0xfffffff0u);
 assert(button.poll(true,3000)==t096_power::ButtonEvent::None);
 button.poll(false,3001);assert(button.poll(false,3031)==t096_power::ButtonEvent::None);
 button.poll(true,4000);button.poll(true,4030);
 assert(button.poll(true,6030)==t096_power::ButtonEvent::Hold);
 button.poll(false,6031);assert(button.poll(false,6061)==t096_power::ButtonEvent::LongReleased);
 button.begin(false,0xfffffff0u);button.poll(true,0xfffffff5u);button.poll(true,30);
 button.poll(false,40);assert(button.poll(false,70)==t096_power::ButtonEvent::Short);
 // USER release returns Ready with either power source and never resets boot.
 for(bool supplied:{true,false}) {
  usb=supplied;control.setClientId(0xF035);
  control.apply({0xF035,uint16_t(supplied?1:2),MSG_CLIENT_CONTROL},
    {1,client_control::Action::Start,10},7000);
  setGnssEnabled(true);userButton.begin(false,7000);pins[42]=LOW;
  serviceUserButton(7000);serviceUserButton(7030);serviceUserButton(9030);
  assert(userButton.held()&&gnssEnabled);
  pins[42]=HIGH;serviceUserButton(9031);serviceUserButton(9061);
  assert(control.state()==client_control::State::Ready&&!gnssEnabled&&confirmationPending);
 }
 // Healthy Ready samples every 30 s; warning/active samples every second.
 usb=false;adcRaw=1143;sample(10000);assert(nextBatteryMs==40010);
 adcRaw=960;sample(11000);assert(nextBatteryMs==12010);
 adcRaw=860;sample(13000);sample(14000);assert(!battery.protectedSleep());
 sample(15000);assert(battery.protectedSleep()&&nextBatteryMs==75010);
 clockMs=15010;serviceLowBattery(clockMs);
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
 assert(!batteryProtection&&!gnssEnabled&&confirmationPending&&control.state()==client_control::State::Ready);
 assert(nextBatteryMs==470010); // recovery remains Ready and does not auto-track.

}
'''
with tempfile.TemporaryDirectory() as td:
    p=Path(td);(p/'test.cpp').write_text(cpp)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS actual T096 ADC/display/USER, retained Ready and low-battery hysteresis/recovery')
