#!/usr/bin/env python3
"""Exercise actual retained-loop deadlines/ISR notification against host fakes."""
from pathlib import Path
import subprocess
import tempfile
from test_t096_gnss_uart import extract_function

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/t096_client.cpp').read_text()
actual = '\n'.join(extract_function(source, name) for name in
    ['void notifyLoopFromIsr()', 'uint32_t idleWaitMs(uint32_t now)', 'void waitForWork()', 'void loop()'])
assert 'systemOff(' not in source, 'normal firmware must remain RTC-wakeable'
cpp = r'''
#include <cassert>
#include <algorithm>
#include "t096_control.h"
#include "t096_power.h"
#include "t096_idle.h"
using TickType_t=uint32_t;using BaseType_t=int;using TaskHandle_t=void*;
constexpr int pdTRUE=1,pdFALSE=0,RADIOLIB_ERR_NONE=0;
constexpr uint32_t kTxTimeoutMs=3000;
#define pdMS_TO_TICKS(x) ((x)*1024U/1000U)
#define portYIELD_FROM_ISR(x) ((void)(x))
uint32_t clockMs=1000;uint32_t millis(){return clockMs;}
TaskHandle_t loopTask=reinterpret_cast<void*>(1);
unsigned notifications=0;uint32_t lastTicks=0;
bool irqBeforeBlock=false;
void vTaskNotifyGiveFromISR(TaskHandle_t,BaseType_t *woken){++notifications;*woken=pdTRUE;}
void yield(){}
uint32_t ulTaskNotifyTake(int clear,TickType_t ticks){
 assert(clear==pdTRUE);lastTicks=ticks;
 if(irqBeforeBlock){irqBeforeBlock=false;++notifications;}
 if(notifications){unsigned n=notifications;notifications=0;return n;}
 clockMs+=ticks*1000U/1024U;return 0;
}
enum class AirState{Idle,Sending,Receiving};AirState air=AirState::Idle;
bool usb=false;bool usbVbusPresent(){return usb;}
bool batteryProtection=false,radioReady=true,radioSleeping=false,radioIrq=false,rxPreamble=false;
bool confirmationPending=false,batterySampling=false,displayPowered=true,displaySleeping=true;
bool displayWaking=false,usbIdentityAnnounced=false;
uint32_t idleWaitCalls=0,idleBlockedMs=0,airStartedMs=0,nextTxStartMs=0;
uint32_t nextBatteryMs=31000,batterySampleMs=0,displayWakeMs=0,nextDisplayMs=0,screenUntilMs=0,nextRadioRetryMs=0;
uint16_t rxErrors=0;size_t outLength=0;
t096_power::Button userButton;
t096_control::Controller control(55);
struct Radio{unsigned sleeps=0;int16_t result=0;int16_t sleep(){++sleeps;return result;}}radio;
unsigned femOffCalls=0;void femOff(){++femOffCalls;}
template<class... T>void usbLog(const char*,T...){}
bool Serial=false;void printIdentity(const char*){}
void serviceUserButton(uint32_t){}void serviceBattery(uint32_t){}void serviceLowBattery(uint32_t){}
void serviceClientDisplay(uint32_t){}void serviceGnss(uint32_t){}
void serviceTransmit(uint32_t){}void serviceReceive(uint32_t){}void observeDataDeferral(uint32_t){}
bool configureRadio(){radioReady=true;return true;}
unsigned sends=0;
bool queueState(uint32_t){outLength=1;return true;}bool queueProbe(uint32_t){return false;}
bool queueDiagnostic(uint32_t){return false;}bool queueRealPosition(uint32_t){return false;}
void sendBuffered(){if(outLength){++sends;outLength=0;control.notedState(millis());}}
'''+actual+r'''
int main(){
 control.notedState(1000);
 assert(idleWaitMs(1000)==30000);
 const auto bootState=control.state();
 waitForWork();assert(clockMs==31000&&idleBlockedMs==30000&&radio.sleeps==1&&radioSleeping);
 assert(control.state()==bootState); // state remains in RAM across RTC task blocking.
 clockMs=1000;idleBlockedMs=0;irqBeforeBlock=true;
 waitForWork();assert(clockMs==1000&&!idleBlockedMs&&radio.sleeps==1);
 notifyLoopFromIsr();waitForWork();assert(clockMs==1000); // queued ISR before wait is consumed.
 loopTask=nullptr;notifyLoopFromIsr();assert(!notifications);loopTask=reinterpret_cast<void*>(1);
 air=AirState::Receiving;airStartedMs=1000;assert(idleWaitMs(1050)==35);
 rxPreamble=true;assert(idleWaitMs(1085)==715);assert(idleWaitMs(1800)==0);
 radioIrq=true;assert(idleWaitMs(1085)==0);radioIrq=false;rxPreamble=false;
 air=AirState::Sending;assert(idleWaitMs(1500)==2500);
 unsigned previous=radio.sleeps;waitForWork();assert(radio.sleeps==previous); // no sleep during TX/RX.
 air=AirState::Idle;clockMs=1000;batterySampling=true;batterySampleMs=1000;
 assert(idleWaitMs(1005)==5);batterySampling=false;
 displaySleeping=false;displayWaking=true;displayWakeMs=1120;
 assert(idleWaitMs(1000)==120);displayWaking=false;nextDisplayMs=1000;assert(!idleWaitMs(1000));
 nextDisplayMs=1500;screenUntilMs=1250;assert(idleWaitMs(1000)==250);displaySleeping=true;
 userButton.begin(false,1000);userButton.poll(true,1000);assert(idleWaitMs(1000)==5);
 userButton.begin(false,1000);usb=true;assert(idleWaitMs(1000)==20);usb=false;
 confirmationPending=true;nextTxStartMs=1300;assert(idleWaitMs(1000)==300);
 nextTxStartMs=1000;assert(!idleWaitMs(1000));confirmationPending=false;
 control.setClientId(0xF035);control.apply({0xF035,1,MSG_CLIENT_CONTROL},{55,client_control::Action::Start,10},1000);
 assert(idleWaitMs(1000)==1);control.requestReady();
 batteryProtection=true;nextBatteryMs=61000;assert(idleWaitMs(1000)==60000);
 loop();assert(!sends);batteryProtection=false;confirmationPending=true;loop();assert(sends==1);
 // Missing radio retries are bounded; sleep failure remains visible and retryable.
 clockMs=1000;radioReady=false;nextRadioRetryMs=1000;nextBatteryMs=31000;
 assert(idleWaitMs(1000)==5000);radioReady=true;radioSleeping=false;radio.result=-7;
 waitForWork();assert(!radioReady&&rxErrors==1&&lastTicks<=pdMS_TO_TICKS(5000));
 // Relative periodic timers plus absolute future deadlines work through uint32 wrap.
 control.notedState(0xFFFFFFF0U);nextBatteryMs=0xFFFFFFF0U+30000;radioReady=true;radioSleeping=true;
 assert(idleWaitMs(16)==29968);
}
'''
with tempfile.TemporaryDirectory() as td:
    p=Path(td);(p/'test.cpp').write_text(cpp)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS actual RTC task wait/ISR race, radio/ADC/display/USER deadlines, TX inhibition, retries and wrap')
