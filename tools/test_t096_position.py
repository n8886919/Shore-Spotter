#!/usr/bin/env python3
"""Actual T096 DATA gating/sequence and bounded CDC writer, without fake live GPS."""
from pathlib import Path
import subprocess,tempfile
from test_t096_gnss_uart import extract_function
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/t096_client.cpp').read_text()
body='\n'.join(extract_function(s,f) for f in ['bool queueRealPosition(uint32_t now)','void observeDataDeferral(uint32_t now)','void usbLog(const char *format, ...)'])
cpp=r'''
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include "protocol.h"
#include "gnss_snapshot.h"
#include "client_cadence.h"
#include "client_control.h"
#include "tracking_policy.h"
#include "t096_gnss_config.h"
struct Collector {gnss_snapshot::Snapshot value{};bool have=true;bool sample(uint32_t,gnss_snapshot::Snapshot &s){s=value;return have;}}gnss;
client_cadence::Scheduler cadence;
struct Control {client_control::State value=client_control::State::Tracking;auto state(){return value;}}control;
enum class AirState {Idle,Sending,Receiving};AirState air=AirState::Idle;
uint8_t out[MAX_PACKET_LEN];size_t outLength=0;bool outOpensReceiveWindow=false,outIsState=false,dataWaiting=false;
uint16_t thisClientId=5,dataSequence=0;uint32_t dataDeferred=0,nextTxStartMs=0,usbLogDropped=0;
struct Console {bool connected=true;int available=128;std::string output;operator bool(){return connected;}int availableForWrite(){return available;}
size_t write(const uint8_t*p,size_t n){assert(n<=size_t(available));output.append((char*)p,n);return n;}}Serial;
'''+body+r'''
int main(){
 auto &s=gnss.value;s.fix=true;s.haveRmc=s.haveGga=true;s.lat=24.1;s.lon=121.5;s.epochMsOfDay=1000;
 s.arrivalAgeMs=20;s.hdop=1.2;s.satellites=9;s.velocityValid=true;s.speedMps=2;s.courseDeg=359.98;
 assert(queueRealPosition(100));PacketHeader h{};PositionPayload p{};
 assert(protocol::decodeData(out,outLength,h,p)&&h.seq==0&&p.fix&&p.courseDeg10==0);
 outLength=0;assert(!queueRealPosition(600));
 s.epochMsOfDay=1500;air=AirState::Sending;observeDataDeferral(600);observeDataDeferral(601);
 assert(dataDeferred==1);air=AirState::Idle;assert(queueRealPosition(602)&&dataSequence==2&&!dataWaiting);
 outLength=0;s.arrivalAgeMs=2100;assert(queueRealPosition(2800));
 assert(protocol::decodeData(out,outLength,h,p)&&h.seq==2&&!p.fix);
 outLength=0;assert(!queueRealPosition(2900));
 s.epochMsOfDay=2000;s.arrivalAgeMs=0;s.haveGga=false;
 assert(!queueRealPosition(3000));assert(!queueRealPosition(3149));assert(queueRealPosition(3150));
 outLength=0;control.value=client_control::State::Ready;s.epochMsOfDay=2500;assert(!queueRealPosition(4000));
 Serial.available=2;usbLog("%s","abcd");assert(usbLogDropped==1&&Serial.output.empty());
 Serial.available=128;usbLog("%s","works");assert(Serial.output=="works");
 Serial.connected=false;usbLog("ignored");assert(Serial.output=="works");
 t096_gnss::Setup setup;setup.begin(0xfffffff0);assert(!setup.due(100));
 const char *line=setup.due(uint32_t(0xfffffff0u+1500u));assert(line&&strstr(line,"PDTINFO"));
 setup.stop();assert(!setup.due(5000));setup.begin(10000);assert(setup.due(11500));
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.cpp').write_text(cpp)
 subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS actual T096 latest-epoch DATA, separate sequence, invalid transition, pair wait, deferrals, bounded CDC and GNSS setup wrap')
