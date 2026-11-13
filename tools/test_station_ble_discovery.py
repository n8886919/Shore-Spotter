#!/usr/bin/env python3
"""Run production BLE discovery with timed ADV/queue/scan fakes; no hardware."""
from pathlib import Path
import subprocess
import tempfile
from test_t096_gnss_uart import extract_function

root = Path(__file__).resolve().parents[1]
source = (root / 'include/station_ble.h').read_text()
actual = source[source.index('class Advertisements'):source.index('static Advertisements callbacks;')]
actual += '\n' + extract_function(source, 'static Discovery discover(BLEScan *scan,Match &match)')
cpp = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "ble_control_protocol.h"
using String=std::string;
struct Target {uint16_t client,station;uint32_t stationBoot,generation;};
struct Match {char address[18];uint8_t type;uint32_t seenMs;};
Target callbackTarget{0xFB27,0x584C,1,1};
Match queued{};
int targets=1,matches=2;
bool haveMatch=false,haveTarget=false,injectMatch=false,injectTarget=false;
uint32_t now=0,base=0,injectAt=0;
uint32_t millis(){return now;}
constexpr int pdTRUE=1,pdFALSE=0;
#define pdMS_TO_TICKS(n) (n)
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
void strlcpy(char *dst,const char *src,size_t n){snprintf(dst,n,"%s",src);}
int xQueueOverwrite(int q,const Match *m){assert(q==matches);queued=*m;haveMatch=true;return pdTRUE;}
void xQueueReset(int q){assert(q==matches);haveMatch=false;}
int xQueuePeek(int q,Target*,int){assert(q==targets);return haveTarget?pdTRUE:pdFALSE;}
int xQueueReceive(int q,Match *m,uint32_t wait){
 assert(q==matches);
 if(!haveMatch){now+=wait;
  if(uint32_t(now-base)>=injectAt){if(injectMatch){haveMatch=true;queued.seenMs=base+injectAt;}if(injectTarget)haveTarget=true;}
 }
 if(!haveMatch)return pdFALSE;
 *m=queued;haveMatch=false;return pdTRUE;
}
struct BLEUUID {explicit BLEUUID(const char*){}};
struct BLEAdvertisedDevice {
 String data=String("\xff\xffSS\x01\x27\xfb",7);bool service=true,manufacturer=true;
 bool haveManufacturerData(){return manufacturer;}
 bool isAdvertisingService(BLEUUID){return service;}
 String getManufacturerData(){return data;}
 struct Address {String toString(){return "AA:BB:CC:DD:EE:FF";}};
 Address getAddress(){return {};}
 uint8_t getAddressType(){return 1;}
};
struct BLEAdvertisedDeviceCallbacks {virtual void onResult(BLEAdvertisedDevice)=0;};
struct BLEScan {
 bool startOk=true,stopOk=true,stuck=false;unsigned stops=0;uint32_t started=0;
 bool start(uint32_t seconds,void(*cb)(int),bool keep){assert(seconds==2&&!cb&&!keep);started=now;return startOk;}
 bool stop(){++stops;return stopOk;}
 bool isScanning(){return stuck||uint32_t(now-started)<2000;}
};
enum class Discovery {Matched,NoMatch,TargetChanged,StartFailed,StopFailed,TimedOut};
'''+actual+r'''
void reset(uint32_t initial=0){now=base=initial;haveTarget=haveMatch=injectMatch=injectTarget=false;injectAt=0;}
int main(){
 Advertisements receiver;BLEAdvertisedDeviceCallbacks &cb=receiver;BLEAdvertisedDevice adv;
 reset();cb.onResult(adv);assert(haveMatch&&queued.type==1&&!strcmp(queued.address,"AA:BB:CC:DD:EE:FF"));
 haveMatch=false;adv.data[5]=0x35;cb.onResult(adv);assert(!haveMatch);
 adv=BLEAdvertisedDevice{};adv.data.resize(6);cb.onResult(adv);assert(!haveMatch);
 adv=BLEAdvertisedDevice{};adv.service=false;cb.onResult(adv);assert(!haveMatch);
 adv=BLEAdvertisedDevice{};adv.manufacturer=false;cb.onResult(adv);assert(!haveMatch);
 callbackTarget.client=0;adv=BLEAdvertisedDevice{};cb.onResult(adv);assert(!haveMatch);callbackTarget.client=0xFB27;
 Match m{};
 for(uint32_t initial:{0u,0xfffffff0u}) {
  reset(initial);injectMatch=true;injectAt=100;BLEScan scan;
  assert(discover(&scan,m)==Discovery::Matched);assert(uint32_t(now-base)==100&&scan.stops==1);
  reset(initial);BLEScan none;assert(discover(&none,m)==Discovery::NoMatch);assert(uint32_t(now-base)==2000);
  reset(initial);injectTarget=true;injectAt=100;BLEScan changed;
  assert(discover(&changed,m)==Discovery::TargetChanged);assert(uint32_t(now-base)==100&&changed.stops==1);
  reset(initial);BLEScan stuck;stuck.stuck=true;
  assert(discover(&stuck,m)==Discovery::TimedOut);assert(uint32_t(now-base)==2200&&stuck.stops==1);
 }
 reset();BLEScan startFail;startFail.startOk=false;assert(discover(&startFail,m)==Discovery::StartFailed);assert(now==0);
 reset();injectMatch=true;BLEScan stopFail;stopFail.stopOk=false;assert(discover(&stopFail,m)==Discovery::StopFailed);
 reset();haveMatch=true;BLEScan stale;assert(discover(&stale,m)==Discovery::NoMatch); // discard earlier scan results
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'discovery.cpp'
    binary = Path(tmp) / 'discovery'
    path.write_text(cpp)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-I', str(root / 'include'), str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS production BLE discovery: identity filtering, immediate match, target change, timeout, scan failures, stale queue, clock wrap')
