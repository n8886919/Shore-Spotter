#!/usr/bin/env python3
"""Exercise the real Station BLE and LoRa evidence handlers with host fakes."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
src = (ROOT / 'include/station_extensions.h').read_text()
def block(marker):
    start = src.index(marker)
    body = src.index('{', start)
    depth = 0
    for match in re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', src[body:]):
        if match.group() == '{': depth += 1
        elif match.group() == '}':
            depth -= 1
            if not depth: return src[start:body + match.end()]
    raise ValueError(marker)
cpp = r'''
#include <cassert>
#include <cstring>
#include "station_client_link.h"
#include "ble_control_protocol.h"
uint32_t nowMs=1000;
uint32_t millis(){return nowMs;}
uint16_t gpsClientId=0x1234, nodeId=0x4321;
uint32_t controlBootId=77;
bool isClientAllowed(uint16_t id){return id==gpsClientId;}
struct Preferences {bool fail=false; int writes=0; uint8_t saved=0; size_t putUChar(const char*,uint8_t c){++writes;if(fail)return 0;saved=c;return 1;}} prefs;
float receivedPacketRssi=0,receivedPacketSnr=0;
constexpr int RADIOLIB_ERR_NONE=0,RADIOLIB_CHANNEL_FREE=-15,RADIOLIB_LORA_DETECTED=-702,RADIOLIB_ERR_RX_TIMEOUT=-6;
bool stationRxReady=true,stationRadioIrq=false;
struct Radio { int result=RADIOLIB_CHANNEL_FREE; int standby(){return 0;} int setFrequency(float){return 0;} int startChannelScan(){return 0;} int getChannelScanResult(){return result;} } radio;
namespace runtime_radio_profile { void set(float,uint8_t){} }
namespace station_ble {
enum class State:uint8_t {Idle,Searching,Connecting,Connected,OwnedElsewhere,Error};
struct Event { uint32_t generation=0; State state=State::Idle; bool connected=false,haveStatus=false;ble_control::Status status{};const char*error="";};
Event next{}; bool pending=false;
bool poll(Event &e){if(!pending)return false;e=next;pending=false;return true;}
}
namespace station_extensions {
station_client_link::Link link;
bool txActive=false, rendezvous=false, commandViaBle=false;
uint32_t lastRfMs=0,rendezvousMs=0,nextSendMs=0,lastLoraStateMs=0;
bool haveRf=false,haveLoraState=false;
client_control::Status loraStatus{};
uint32_t generation=1;
station_ble::Event ble{};
uint32_t bleMs=0,bleUptime=0,bleBoot=0;bool haveBleClock=false;
uint8_t selectedChannel=1,savedChannel=0;
int8_t activeChannel=1;
bool verified=false;
bool scanning=false,cadActive=false;const char *scanState="idle",*scanError="";
uint32_t scanStarted=0;uint8_t scanChannel=0,scanAttempt=0;
uint16_t scanSamples[2]{},scanBusy[2]{},scanErrors[2]{};int restored=0;
bool canRadio(uint32_t){return true;}
void restoreRadio(){++restored;stationRxReady=true;}
void abortScan(const char *why){scanning=false;scanState="error";scanError=why;restoreRadio();}
'''
for marker in ('static bool bleFresh(', 'static bool claimed(', 'static bool activeLora()', 'static void serviceBle()', 'static void serviceScan()', 'static bool accept('):
    cpp += block(marker) + '\n'
cpp += r'''
}
using namespace station_extensions;
void event(ble_control::Status s, station_ble::State state=station_ble::State::Connected, bool connected=true) {
 station_ble::next={generation,state,connected,true,s,""};station_ble::pending=true;serviceBle();
}
int main(){
 ble_control::Status s{};s.client=gpsClientId;s.owner=nodeId;s.boot=10;s.stationBoot=controlBootId;s.uptime=100;s.command=10;
 event(s);assert(bleFresh(nowMs)&&claimed(nowMs));
 nowMs+=3100;event(s);assert(!bleFresh(nowMs)); // reading cached characteristic is not life
 ++s.uptime;event(s);assert(bleFresh(nowMs));
 // A delayed LoRa status cannot roll back the BLE-recovered sequence.
 client_control::Status old{s.boot,client_control::State::Ready,nodeId,9,0,0,0,0,false};
 link.status.state=client_control::State::Tracking;
 link.observe(gpsClientId,old,nowMs,nodeId);assert(link.commandId==10 && link.status.state==client_control::State::Tracking);
 assert(link.request(client_control::Action::Start,nowMs,nodeId));
 s.state=client_control::State::Tracking;s.command=link.commandId;s.replySeq=s.command;s.result=ble_control::Result::Accepted;++s.uptime;
 event(s);assert(link.pending()&&!verified&&prefs.writes==0); // BLE acknowledgement is not RF proof
 client_control::Status rf{s.boot,client_control::State::Tracking,nodeId,s.command,0,0,0,0,false};
 uint8_t bytes[CLIENT_STATE_PACKET_LEN];PacketHeader h{gpsClientId,0,MSG_CLIENT_STATE};
 assert(client_control::encodeStatus(bytes,sizeof(bytes),h,rf));
 assert(accept(bytes,sizeof(bytes),nowMs));assert(!strcmp(link.command,"confirmed")&&verified&&prefs.saved==1);
 // A later read of an older BLE snapshot cannot replace the newer RF state.
 loraStatus.state=client_control::State::Ready;assert(!activeLora());
 loraStatus.state=client_control::State::Tracking;assert(activeLora());
 loraStatus.state=client_control::State::Test;assert(activeLora());
 auto stale=s;--stale.command;stale.state=client_control::State::Ready;++stale.uptime;event(stale);
 assert(!bleFresh(nowMs)&&link.status.state==client_control::State::Tracking);
 ++s.command;s.state=client_control::State::Ready;s.uptime+=2;event(s);assert(!haveLoraState);
 accept(bytes,sizeof(bytes),nowMs);assert(!haveLoraState); // delayed old RF cannot restore stale fallback
 // Reboot evidence invalidates previous LoRa freshness.
 ++s.boot;++s.uptime;event(s);assert(!haveLoraState&&!verified);
 // Owner conflict remains visible even when the worker moves to searching.
 s.owner=0x2222;s.stationBoot=55;++s.uptime;event(s,station_ble::State::OwnedElsewhere,false);
 station_ble::next={generation,station_ble::State::Searching,false,false,{},""};station_ble::pending=true;serviceBle();
 assert(ble.state==station_ble::State::OwnedElsewhere&&ble.haveStatus&&ble.status.owner==0x2222&&!ble.connected);
 // CAD timeouts remain visible as errors and restore RX, not a healthy scan.
 s.owner=nodeId;s.stationBoot=controlBootId;s.state=client_control::State::Ready;++s.uptime;event(s);
 scanning=true;scanState="scanning";scanChannel=scanAttempt=0;cadActive=false;
 for(int i=0;i<10;++i){serviceScan();nowMs+=251;serviceScan();}
 assert(!scanning&&!strcmp(scanState,"error")&&!strcmp(scanError,"cad_no_samples"));
 assert(scanErrors[0]==5&&scanErrors[1]==5&&restored==1&&stationRxReady);
 // Failed persistent save does not report command confirmed.
 link=station_client_link::Link{};s.owner=nodeId;s.stationBoot=controlBootId;s.state=client_control::State::Ready;++s.uptime;event(s);
 assert(link.request(client_control::Action::Start,nowMs,nodeId));rf.boot=s.boot;rf.command=link.commandId;rf.station=nodeId;
 assert(client_control::encodeStatus(bytes,sizeof(bytes),h,rf));savedChannel=0;selectedChannel=1;activeChannel=1;prefs.fail=true;
 accept(bytes,sizeof(bytes),nowMs);assert(!strcmp(link.command,"error")&&savedChannel==0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / 'station.cpp'
    exe = Path(tmp) / 'station'
    source.write_text(cpp)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-I',str(ROOT / 'include'),str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS actual Station BLE and LoRa handlers: stale uptime, reboot, owner, RF proof and NVS failure')
