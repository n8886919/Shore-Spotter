#!/usr/bin/env python3
"""Actual V4 RF record codec, USB router, and NOR worker round-trip tests."""
from pathlib import Path
import json
import subprocess
import tempfile
from read_diagnostic_log import decode_frame
from test_diagnostic_store import STUB
from test_diagnostic_usb_router import block
ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix='shore-v4-flash-') as td:
    d=Path(td)
    source=d/'codec.cpp'; binary=d/'codec'
    source.write_text(r'''
#include <cassert>
#include <iostream>
#include "station_flash.h"
using namespace diagnostic_store::codec;
int main(){
 uint8_t frame[512], record[51];makeHeader(frame);std::cout.write((char*)frame,512);
 packet_diagnostics::Event e;e.kind=packet_diagnostics::Kind::LinkTest;e.id=42;e.ms=123;
 e.rssiDbm10=-753;e.snrQuarterDb=-13;e.code=-7;e.length=18;e.rawLength=18;
 for(unsigned i=0;i<18;++i)e.raw[i]=i;
 assert(!station_flash::encodePacket(record,32,e));
 const size_t n=station_flash::encodePacket(record,sizeof(record),e);assert(n==33);
 beginFrame(frame,77,0,123);assert(append(frame,9,record,n,e.ms));
 const char health[]="{\"phone\":true,\"position_valid\":false,\"flash_dropped\":3}";
 assert(append(frame,10,health,sizeof(health)-1,456));seal(frame);std::cout.write((char*)frame,512);
 e.rawLength=37;assert(!station_flash::encodePacket(record,sizeof(record),e));
}
''')
    flags=['g++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include')]
    subprocess.run(flags+['-DBOARD_HELTEC_V4',str(source),'-o',str(binary)],check=True)
    raw=subprocess.check_output([str(binary)])
    header=decode_frame(raw[:512],0)[0];assert header['partition_bytes']==0x360000
    packet,health=decode_frame(raw[512:],1)
    assert packet['event']=='link_test' and packet['event_id']==42 and packet['boot_id']==77
    assert packet['rssi_dbm']==-75.3 and packet['snr_db']==-3.25 and packet['error']==-7
    assert packet['wire_hex']==bytes(range(18)).hex()
    assert health['data']['flash_dropped']==3
    # Compile the real parser, including unrecognized input and queue backpressure.
    parser=block((ROOT/'src/station_flash.cpp').read_text(),'void serviceUsb()')
    source.write_text(r'''
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>
uint32_t now=0;uint32_t millis(){return now;}
struct Console{std::string input;int available(){return input.size();}int read(){char c=input[0];input.erase(0,1);return c;}}Serial;
namespace diagnostic_store{
 bool active=false,blocked=false;std::vector<std::string> lines;
 bool transferActive(){return active;}
 bool command(const char*s){if(blocked)return false;lines.emplace_back(s);return true;}
}
'''+parser+r'''
void pump(){for(int i=0;i<10;++i)serviceUsb();}
int main(){
 Serial.input="FOO\nDIAG STATUS\n";pump();assert(diagnostic_store::lines.size()==2);
 assert(diagnostic_store::lines[0]=="DIAG INVALID"&&diagnostic_store::lines[1]=="DIAG STATUS");
 diagnostic_store::blocked=true;Serial.input="DIAG READ 0 2\nDIAG STATUS\n";pump();
 now=2000;pump();assert(diagnostic_store::lines.size()==2);
 diagnostic_store::blocked=false;pump();assert(diagnostic_store::lines[2]=="DIAG READ 0 2");
 assert(diagnostic_store::lines[3]=="DIAG STATUS");
 Serial.input=std::string(100,'x')+"\nDIAG STATUS\n";pump();
 assert(diagnostic_store::lines[4]=="DIAG INVALID"&&diagnostic_store::lines[5]=="DIAG STATUS");
 Serial.input="DIAG READ";pump();now+=1001;pump();Serial.input="DIAG STATUS\n";pump();
 assert(diagnostic_store::lines.back()=="DIAG STATUS");
 diagnostic_store::active=true;Serial.input="DIAG STATUS\n";pump();
 auto n=diagnostic_store::lines.size();pump();assert(diagnostic_store::lines.size()==n);
 diagnostic_store::active=false;pump();assert(diagnostic_store::lines.size()==n+1);
}
''')
    subprocess.run(flags+[str(source),'-o',str(binary)],check=True);subprocess.run([str(binary)],check=True)
    # Existing worker's mock NOR, at the real V4 16 MB layout and 10 s flush.
    (d/'stub.h').write_text(STUB.replace('0x670000','0xc90000').replace('0x180000','0x360000'))
    for name in ['Arduino.h','esp_partition.h','freertos/FreeRTOS.h','freertos/queue.h','freertos/task.h']:
        p=d/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "stub.h"\n')
    source.write_text(r'''
#include "diagnostic_store.cpp"
using namespace diagnostic_store;
void run(uint32_t n){uint32_t until=clockMs+n;onDelay=[=](){if(clockMs>=until)throw Done{};};try{worker(nullptr);}catch(Done&){}onDelay=nullptr;}
int main(){
 begin(42);run(2000);assert(healthy()&&erases==0&&codec::validHeader(flash.data()));
 assert(submit(10,"{}",2,clockMs));run(5000);assert(shared.records==0);
 run(6000);assert(shared.records==1&&shared.validFrames==1&&shared.dropped==0);
 assert(command("DIAG READ 0 2"));run(3000);assert(!transferActive());
 assert(Serial.output.find("@DIAG END 0 2")!=std::string::npos);std::cout<<statusJson();
}
''')
    subprocess.run(flags+['-DARDUINO','-DBOARD_HELTEC_V4','-I'+str(d),'-I'+str(ROOT/'src'),str(source),'-o',str(binary)],check=True)
    status=json.loads(subprocess.check_output([str(binary)],text=True));assert status['capacity_frames']==6912
print('PASS V4 packet/health codec round-trip, bounded USB routing, 16 MB partition worker and 10 s flush')
