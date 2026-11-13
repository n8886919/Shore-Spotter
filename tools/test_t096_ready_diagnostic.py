#!/usr/bin/env python3
"""Run the actual diagnostic queue with Ready and Tracking against host fakes."""
from pathlib import Path
import subprocess
import tempfile
from test_t096_gnss_uart import extract_function
root = Path(__file__).resolve().parents[1]
source = (root/'src/t096_client.cpp').read_text()
body = extract_function(source, 'bool queueDiagnostic(uint32_t now)')
cpp = r'''
#include <cassert>
#include <cstring>
#include "protocol.h"
#include "gnss_snapshot.h"
#include "client_control.h"
struct Control { client_control::State value=client_control::State::Ready; auto state(){return value;} } control;
struct Collector { struct Stats {uint32_t lastEpochIntervalMs=0,rejectedSentences=0;} statsValue;
  bool sample(uint32_t,gnss_snapshot::Snapshot&){return false;} const Stats& stats(){return statsValue;} } gnss;
struct Battery { uint16_t mv(){return 3800;} } battery;
struct Temperature { int8_t wire(uint32_t){return 23;} } mcuTemperature;
struct Rate { bool ready(){return false;} const char* state(){return "unknown";} } gpsRate;
namespace gnss_diagnostics {
uint16_t age(uint32_t v){return v;} uint16_t counter(uint32_t v){return v;}
int capture(Collector&,uint32_t){return 0;}
size_t encode(uint8_t*,size_t,PacketHeader,int){return 0;}
}
uint32_t nextExtraMs=0,gpsBacklogDrops=0,txErrors=0,dataDeferred=0;
uint8_t extraKind=0,out[MAX_PACKET_LEN]{}; uint16_t thisClientId=42,txSequence=0;
size_t outLength=0; bool outOpensReceiveWindow=false,outIsState=false;
''' + body + r'''
int main(){
 assert(!queueDiagnostic(10000)); assert(outLength==0 && txSequence==0 && extraKind==0);
 control.value=client_control::State::Test;assert(!queueDiagnostic(10000));
 control.value=client_control::State::Tracking;assert(queueDiagnostic(10000));
 assert(outLength>0 && txSequence==1 && extraKind==1 && nextExtraMs==15000);
 control.value=client_control::State::Ready;outLength=0;
 assert(!queueDiagnostic(20000) && outLength==0 && txSequence==1);
}
'''
with tempfile.TemporaryDirectory() as td:
    p=Path(td)/'test.cpp'; p.write_text(cpp); exe=Path(td)/'test'
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-I',str(root/'include'),str(p),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS actual diagnostic queue suppresses Ready/Test and sends Tracking telemetry')
