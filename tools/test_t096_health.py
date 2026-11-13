#!/usr/bin/env python3
"""Actual TEMP/USB code plus real parser/evidence: no device I/O."""
from pathlib import Path
import subprocess,tempfile
from test_t096_gnss_uart import extract_function
root=Path(__file__).resolve().parents[1]
s=(root/'src/t096_client.cpp').read_text()
actual='\n'.join(extract_function(s,f) for f in ['void serviceMcuTemperature(uint32_t now)','void serviceDiagnosticUsb(uint32_t now)'])
cpp=r'''
#include <cassert>
#include <string>
#include <algorithm>
#include <iostream>
#include "nmea_evidence.h"
#include "mcu_temperature.h"
#include "protocol.h"
#include "firmware_version.h"
mcu_temperature::Sample mcuTemperature;
nmea_evidence::Store nmeaEvidence;
nmea_evidence::Export nmeaExport;
uint32_t thisBoot=42;
struct TempRegs{int32_t TEMP=0;uint32_t EVENTS_DATARDY=0,TASKS_START=0,TASKS_STOP=0;}regs;
auto NRF_TEMP=&regs;
constexpr int NRF_SUCCESS=0;
uint8_t sdEnabled=0;int sdStatus=0,tempStatus=0;int32_t sdValue=100;
int sd_softdevice_is_enabled(uint8_t *p){*p=sdEnabled;return sdStatus;}
int sd_temp_get(int32_t *p){*p=sdValue;return tempStatus;}
enum class AirState{Idle,Sending};AirState air=AirState::Idle;bool radioIrq=false;
struct Usb{
 bool connected=true;int capacity=256;size_t partial=999;std::string input,output;
 explicit operator bool()const{return connected;}
 int available(){return input.size();}
 char read(){char c=input[0];input.erase(0,1);return c;}
 int availableForWrite(){return capacity;}
 size_t write(const uint8_t*p,size_t n){n=std::min(n,partial);output.append((const char*)p,n);return n;}
}Serial;
'''+actual+r'''
std::string sentence(const std::string &body){unsigned c=0;for(char b:body)c^=(unsigned char)b;char suffix[8];snprintf(suffix,sizeof(suffix),"*%02X\r\n",c);return "$"+body+suffix;}
void feed(gnss_snapshot::Collector &p,const std::string &raw,uint32_t ms){for(char c:raw)nmeaEvidence.feed(p,c,ms);}
int main(){
 // Real parser: valid no-fix is not an error; preserve corrupt bytes verbatim.
 gnss_snapshot::Collector p(115200);
 auto valid=sentence("GNRMC,120000.000,V,,,,,,,221026,,,N");feed(p,valid,1);assert(!nmeaEvidence.total());
 auto bad=valid;bad[bad.size()-4]='Z';feed(p,bad,2);
 assert(nmeaEvidence.count()==1&&nmeaEvidence.at(0).reason==gnss_snapshot::RejectReason::Checksum);
 assert(std::string((const char*)nmeaEvidence.at(0).raw,nmeaEvidence.at(0).length)==bad);
 feed(p,sentence("GNRMC,,V,,,,,,,221026,,,N"),3);assert(nmeaEvidence.at(1).reason==gnss_snapshot::RejectReason::Time);
 std::string control="$GNRMC,";control+=char(0);feed(p,control,4);
 assert(nmeaEvidence.at(2).reason==gnss_snapshot::RejectReason::NonAscii&&nmeaEvidence.at(2).raw[7]==0);
 feed(p,"$"+std::string(170,'A'),5);assert(nmeaEvidence.at(3).reason==gnss_snapshot::RejectReason::Overflow&&nmeaEvidence.at(3).truncated);
 feed(p,"$partial$",6);assert(nmeaEvidence.total()==5&&nmeaEvidence.overwritten()==1&&nmeaEvidence.at(3).reason==gnss_snapshot::RejectReason::Resync);
 nmeaEvidence.interrupted(7);assert(nmeaEvidence.at(3).reason==gnss_snapshot::RejectReason::Backlog);
 // USB command captures a stable snapshot while the live ring wraps.
 Serial.input="DIAG NMEA\n";serviceDiagnosticUsb(8);assert(nmeaExport.busy());
 Serial.capacity=0;serviceDiagnosticUsb(8);for(int i=0;i<20;++i)feed(p,bad,9);
 for(int i=0;i<10;++i) { serviceDiagnosticUsb(10); }
 assert(Serial.output.empty());
 Serial.capacity=256;Serial.partial=7;
 for(int i=0;i<400;++i)serviceDiagnosticUsb(11);
 assert(Serial.output.find("total=6 overwritten=2")!=std::string::npos);
 assert(Serial.output.find("@NMEA END count=4")!=std::string::npos);
 assert(Serial.output.find("reason=non_ascii")!=std::string::npos);
 assert(Serial.output.find("24474e524d432c00")!=std::string::npos);
 assert(Serial.output.find("id=26")==std::string::npos);
 Serial.output.clear();Serial.input=std::string(80,'X')+"\nDIAG STATUS\n";
 for(int i=0;i<200;++i)serviceDiagnosticUsb(12);
 assert(Serial.output.find("@DIAG ERROR")!=std::string::npos&&Serial.output.find("@DIAG boot=")!=std::string::npos);
 // No periodic wake timer, no conversion during RF, timeout shuts peripheral down.
 air=AirState::Sending;serviceMcuTemperature(0);assert(!mcuTemperature.attempted);air=AirState::Idle;
 serviceMcuTemperature(0);assert(mcuTemperature.pending&&regs.TASKS_START);
 serviceMcuTemperature(9);assert(mcuTemperature.pending);
 serviceMcuTemperature(10);assert(!mcuTemperature.pending&&mcuTemperature.failures==1&&regs.TASKS_STOP);
 assert(mcuTemperature.wire(10)==INT8_MIN);
 serviceMcuTemperature(29999);assert(!mcuTemperature.pending);
 serviceMcuTemperature(30000);regs.TEMP=101;regs.EVENTS_DATARDY=1;serviceMcuTemperature(30001);
 assert(mcuTemperature.wire(30001)==25&&mcuTemperature.maximumQuarterC==101&&!regs.EVENTS_DATARDY);
 assert(mcuTemperature.wire(90001)==INT8_MIN);
 sdEnabled=1;sdValue=-10;serviceMcuTemperature(60000);assert(mcuTemperature.wire(60000)==-3);
 tempStatus=1;serviceMcuTemperature(90000);assert(mcuTemperature.wire(90000)==INT8_MIN);
 assert(mcuTemperature.maximumQuarterC==101&&mcuTemperature.failures==2);
 mcuTemperature={};sdStatus=1;serviceMcuTemperature(0xfffffff0);assert(mcuTemperature.failures==1);
 sdStatus=0;tempStatus=0;sdValue=341;serviceMcuTemperature(uint32_t(0xfffffff0+30000));assert(mcuTemperature.failures==2);
 sdValue=120;serviceMcuTemperature(uint32_t(0xfffffff0+60000));assert(mcuTemperature.wire(59984)==30);
 std::cout<<"PASS actual TEMP success/failure/timeout/wrap, raw NMEA reasons, ring overwrite, immutable USB export/backpressure/partial writes\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(cpp)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(root/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

# Reader rejects partial/misaligned transfers and retains exact raw bytes.
from read_t096_diagnostics import NmeaExport
x=NmeaExport()
for line in ['@NMEA BEGIN boot=0000002A count=1 total=5 overwritten=4',
             '@NMEA META id=5 ms=100 reason=non_ascii len=3 truncated=1',
             '@NMEA DATA id=5 offset=0 hex=24000a', '@NMEA END count=1']:
    x.accept(line)
assert x.done and x.records[0]['raw_hex']=='24000a'
for malformed in ['@NMEA DATA id=5 offset=1 hex=24', '@NMEA END count=1']:
    x=NmeaExport();x.accept('@NMEA BEGIN boot=2A count=1 total=1 overwritten=0')
    try:x.accept(malformed)
    except ValueError:pass
    else:raise AssertionError('partial export accepted')
print('PASS USB reader exact bytes and partial transfer rejection')
