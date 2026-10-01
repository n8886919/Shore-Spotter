#!/usr/bin/env python3
"""Host checks of real diagnostic parsing, phase policy, codec and GPS integration.

The actual main.cpp UART service/configuration functions run against a fake UART.
This checks software contracts, not flash latency, RF interference or field accuracy.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/main.cpp").read_text()


def block(marker):
    start = SOURCE.index(marker)
    body = SOURCE.index("{", start)
    depth = 0
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    for match in re.finditer(tokens, SOURCE[body:]):
        token = match.group()
        if token == "{":
            depth += 1
        elif token == "}":
            depth -= 1
            if not depth:
                return SOURCE[start:body + match.end()]
    raise ValueError(f"Unterminated function: {marker}")


CPP = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <vector>
#include "field_diagnostic.h"
#include "gnss_rate.h"
#define F(x) x
uint32_t clockMs=0;
uint32_t millis(){return clockMs;}
void delay(uint32_t ms){clockMs+=ms;}
void pinMode(int,int){}
void digitalWrite(int,int){}
constexpr int GPS_EN_PIN=7,GPS_RX_PIN=9,GPS_TX_PIN=8,OUTPUT=1,HIGH=1,SERIAL_8N1=0;
constexpr uint32_t GPS_BAUD=gnss_rate::kBaud,GPS_RX_BUFFER_BYTES=1024,GPS_BACKLOG_GUARD_MS=200;
struct Uart {
  uint32_t baud=0,receiver=9600,period=0;bool buffer=false,pair=false;
  std::deque<char> bytes;
  std::vector<std::string> commands;
  void setRxBufferSize(size_t n){assert(n==1024);buffer=true;}
  void begin(uint32_t b,int,int rx,int tx){assert(buffer&&rx==9&&tx==8);baud=b;}
  void updateBaudRate(uint32_t b){baud=b;}
  void flush(){}
  void print(const char* text){
    const std::string wire=text;commands.push_back(wire);
    assert(wire.front()=='$'&&wire.substr(wire.size()-2)=="\r\n");
    const auto star=wire.find('*');assert(star!=std::string::npos);
    unsigned checksum=0;for(size_t i=1;i<star;++i)checksum^=uint8_t(wire[i]);
    assert(checksum==std::stoul(wire.substr(star+1,2),nullptr,16));
    if(baud!=receiver)return;
    if(wire.find("PCAS01,5")!=std::string::npos)receiver=115200;
    if(wire.find("PCAS02,1000")!=std::string::npos)period=1000;
    if(wire.find("PCAS02,500")!=std::string::npos)period=500;
    if(wire.find("PCAS03,1,0,0,0,1,0,0,0,0,0,,,0,0")!=std::string::npos)pair=true;
  }
  void queue(const std::string& s){for(char c:s)bytes.push_back(c);}
  int available()const{return bytes.size();}
  int read(){assert(!bytes.empty());const char c=bytes.front();bytes.pop_front();return uint8_t(c);}
} GPSSerial;
struct LogStub {template<class...T>void print(T...){}template<class...T>void println(T...){};}Log;
struct GpsStub {size_t bytes=0;void encode(char){++bytes;}}gps;
gnss_snapshot::Collector gnssCollector{GPS_BAUD};
gnss_rate::Monitor gpsRate;
bool gpsServiceStarted=false;
uint32_t gpsLastServiceMs=0,gpsBacklogDrops=0;
field_diagnostic::Utc diagnosticUtc;
uint8_t diagnosticRaw[128];size_t diagnosticRawLength=0;
uint16_t diagnosticRawKind=1;uint32_t diagnosticRawSplits=0;
namespace diagnostic_store {
struct Record{uint16_t kind;std::string bytes;uint32_t ms;};
std::vector<Record> records;
bool submit(uint16_t kind,const void* bytes,size_t n,uint32_t ms){records.push_back({kind,std::string(static_cast<const char*>(bytes),n),ms});return true;}
}
'''
for marker in ["static void diagnosticByte(", "static void configureGps()", "static void serviceGps()"]:
    CPP += block(marker) + "\n"

CPP += r'''
std::string wire(const std::string& body){
 unsigned checksum=0;for(char c:body)checksum^=uint8_t(c);
 char tail[8];snprintf(tail,sizeof(tail),"*%02X\r\n",checksum);return "$"+body+tail;
}
std::string rmc(const std::string& time="120000.125",const std::string& date="230926"){
 return "GNRMC,"+time+",A,2407.40740,N,12145.92592,E,10.00,90.00,"+date+",,,A";
}
std::string gga(const std::string& time="120000.125"){
 return "GNGGA,"+time+",2407.40740,N,12145.92592,E,1,08,0.9,10.0,M,0.0,M,,";
}
void feed(field_diagnostic::Utc& utc,const std::string& body,uint32_t now){for(char c:wire(body))utc.feed(c,now);}
void feed(gnss_snapshot::Collector& collector,const std::string& body,uint32_t now){for(char c:wire(body))collector.feed(c,now);}
uint64_t readLe(const uint8_t* bytes,size_t n){uint64_t v=0;for(size_t i=0;i<n;++i)v|=uint64_t(bytes[i])<<(8*i);return v;}
double readDouble(const uint8_t* bytes){uint64_t v=readLe(bytes,8);double d;memcpy(&d,&v,8);return d;}
float readFloat(const uint8_t* bytes){uint32_t v=readLe(bytes,4);float f;memcpy(&f,&v,4);return f;}

void utcTests(){
 field_diagnostic::Utc utc;
 assert(!utc.matches(43200125,0)&&utc.age(0)==UINT32_MAX);
 feed(utc,rmc(),1000);
 assert(utc.date()==20260923&&utc.matches(43200125,1000));
 assert(!utc.matches(43200126,1000)&&utc.matches(43200125,2999)&&!utc.matches(43200125,3000));
 assert(utc.age(1700)==700);
 // Unverified bytes cannot invent a date, and non-RMC sentences cannot change it.
 auto corrupt=wire(rmc("120001.000","240926"));const auto star=corrupt.find('*');corrupt[star+1]=corrupt[star+1]=='0'?'1':'0';
 for(char c:corrupt)utc.feed(c,1100);
 assert(utc.date()==20260923&&!utc.matches(43201000,1100));
 feed(utc,gga("120001.000"),1200);assert(utc.date()==20260923);
 for(const char* date:{"000926","320126","310426","290226","311126","231326","230026","23092x","2309260",""}){
  feed(utc,rmc(),1000);feed(utc,rmc("120000.125",date),1001);
  assert(!utc.matches(43200125,1001)&&utc.age(1001)==UINT32_MAX);
 }
 for(const char* date:{"290224","290200","310126","300426","311299"}){
  feed(utc,rmc("000000.000",date),1000);assert(utc.matches(0,1000));
 }
 for(const char* time:{"240000","126000","120060","12000","120000.","120000x","120000.12x","120000.1234"}){
  feed(utc,rmc(),1000);feed(utc,rmc(time),1001);assert(!utc.matches(43200125,1001));
 }
 feed(utc,rmc(),1000);feed(utc,"GNRMC,120000.125,A",1001);assert(!utc.matches(43200125,1001));
 feed(utc,rmc("000000.1","240926"),2000);assert(utc.matches(100,2000));
 feed(utc,rmc("000000.12","240926"),2010);assert(utc.matches(120,2010));
 // Midnight never attaches yesterday's date to a new GGA-only epoch.
 feed(utc,rmc("235959.000","230926"),3000);assert(utc.matches(86399000,3000));
 feed(utc,gga("000000.000"),4000);assert(!utc.matches(0,4000));
 feed(utc,rmc("000000.000","240926"),4010);assert(utc.matches(0,4010)&&utc.date()==20260924);
 feed(utc,rmc(),0xfffffff0u);assert(utc.matches(43200125,100)&&utc.age(100)==116);
 // Invalidating UART backlog must also discard a partial sentence.
 const auto partial=wire(rmc());for(size_t i=0;i<20;++i)utc.feed(partial[i],2000);
 utc.invalidate();for(size_t i=20;i<partial.size();++i)utc.feed(partial[i],2001);
 assert(!utc.matches(43200125,2001));
 feed(utc,rmc(),2002);assert(utc.matches(43200125,2002));
 std::cout<<"PASS UTC checksum, invalid dates/leap years, millisecond grammar, midnight epoch pairing, invalidation and millis wrap\n";
}

void codecTests(){
 gnss_snapshot::Collector c(115200);field_diagnostic::Utc utc;
 feed(c,rmc(),1000);feed(utc,rmc(),1000);feed(c,gga(),1010);
 field_diagnostic::Metrics m;m.phase=3;m.rf=true;m.sd=true;m.battery=4100;m.heap=0x10203040;
 m.backlog=7;m.tx=123;m.txErrors=4;m.rawSplits=5;m.loopGap=250;
 std::array<uint8_t,82> b;b.fill(0xa5);field_diagnostic::encode(b.data()+1,c,utc,1200,m);const auto* p=b.data()+1;
 assert(b.front()==0xa5&&b.back()==0xa5&&p[0]==1&&p[1]==223&&p[2]==8&&p[3]==3);
 gnss_snapshot::Snapshot s;assert(c.sample(1200,s));
 assert(readLe(p+4,4)==43200125&&readLe(p+8,4)==s.sourceAgeMs&&readLe(p+12,4)==200&&readLe(p+16,4)==20260923);
 assert(std::abs(readDouble(p+20)-(24+7.4074/60))<1e-10&&std::abs(readDouble(p+28)-(121+45.92592/60))<1e-10);
 assert(std::abs(readFloat(p+36)-10*1852.0/3600)<1e-6&&readFloat(p+40)==90&&std::abs(readFloat(p+44)-0.9)<1e-6);
 assert(readLe(p+48,2)==4100&&readLe(p+50,2)==0&&readLe(p+52,4)==m.heap&&readLe(p+56,4)==7);
 assert(readLe(p+60,4)==123&&readLe(p+64,4)==4&&readLe(p+68,4)==5&&readLe(p+72,4)==250&&readLe(p+76,4)==200);
 // Fresh GGA without same-epoch RMC cannot reuse a previous UTC date.
 feed(c,gga("120001.125"),2000);field_diagnostic::encode(b.data()+1,c,utc,2000,m);
 assert(!(p[1]&16)&&!(p[1]&4)&&readLe(p+16,4)==0);
 c.invalidate(2100);utc.invalidate();field_diagnostic::encode(b.data()+1,c,utc,2100,m);
 assert((p[1]&31)==0&&p[2]==255&&readLe(p+8,4)==UINT32_MAX&&readLe(p+12,4)==UINT32_MAX);
 assert(std::isnan(readFloat(p+44))&&readLe(p+76,4)==UINT32_MAX);
 std::cout<<"PASS guarded 80-byte little-endian diagnostic snapshot, full coordinates, UTC validity and unknown-value encoding\n";
}

void phaseTests(){
 for(uint32_t base:{0u,0xffff0000u}){
  field_diagnostic::Plan plan;assert(plan.phase()==0&&!plan.rf()&&!plan.sd());
  for(uint32_t t=0;t<30000;t+=100){assert(!plan.observe(base+t,t%1000>=100));}
  // RMC and GGA have a brief gap every epoch; it must not restart the 30s warmup.
  assert(!plan.observe(base+30000,false));assert(plan.observe(base+30100,true));
  assert(plan.phase()==1&&!plan.rf()&&!plan.sd());
  uint32_t phaseStart=base+30100;
  for(unsigned phase=2;phase<=5;++phase){
   assert(!plan.observe(phaseStart+field_diagnostic::kPhaseMs-1,false));
   assert(plan.observe(phaseStart+field_diagnostic::kPhaseMs,false));phaseStart+=field_diagnostic::kPhaseMs;
   assert(plan.phase()==phase&&plan.rf()&&plan.sd()==(phase==3));
  }
  assert(!plan.observe(phaseStart+field_diagnostic::kPhaseMs,true)&&plan.phase()==5);
 }
 field_diagnostic::Plan lost;
 for(uint32_t t=0;t<=10000;t+=100)assert(!lost.observe(t,true));
 assert(!lost.observe(12001,false));assert(!lost.observe(13000,true));
 for(uint32_t t=13100;t<43000;t+=100)assert(!lost.observe(t,true));
 assert(lost.observe(43000,true)&&lost.phase()==1);
 for(unsigned phase=0;phase<=5;++phase){
  field_diagnostic::Plan plan;
  for(uint32_t t=0;t<=30000;t+=100)plan.observe(t,true);
  if(phase==0)plan={};else for(unsigned i=1;i<phase;++i)plan.observe(30000+i*field_diagnostic::kPhaseMs,true);
  assert(plan.phase()==phase);plan.abort();assert(plan.phase()==6&&plan.rf()&&!plan.sd());
  assert(!plan.observe(10000000,true)&&plan.phase()==6);
 }
 std::cout<<"PASS 30s warmup tolerates epoch pairing, >2s unusable resets it; all RF/SD phases, terminal abort and millis wrap\n";
}

void rateAndCommands(){
 constexpr uint32_t period=gnss_rate::kTargetIntervalMs;
#if defined(FIELD_DIAGNOSTIC)
 static_assert(period==1000,"diagnostic baseline is 1 Hz");
#else
 static_assert(period==500,"normal configuration stays 2 Hz");
#endif
 for(uint32_t base:{0u,0xfffff000u}){
  gnss_rate::Monitor monitor;const uint32_t n=5000/period;
  assert(!monitor.observe(base,0xfffffffcu,0xfffffffcu,0xfffffffcu)&&!monitor.ready());
  assert(!monitor.observe(base+4999,1,1,1));
  assert(monitor.observe(base+5000,0xfffffffcu+n,0xfffffffcu+n,0xfffffffcu+n));
  assert(monitor.hz()==1000.0f/period&&monitor.rmcHz()==monitor.hz()&&monitor.ggaHz()==monitor.hz());
  assert(std::string(monitor.state())==(period==1000?"observed_1hz":"observed_2hz"));
  assert(monitor.observe(base+10000,0xfffffffcu+2*n,0xfffffffcu+2*n,0xfffffffcu+n));
  assert(std::string(monitor.state())=="missing_gga");
 }
 for(int baud:{9600,115200}){
  GPSSerial={};GPSSerial.receiver=baud;gpsServiceStarted=true;configureGps();
  assert(GPSSerial.commands.size()==4&&GPSSerial.receiver==115200&&GPSSerial.baud==115200);
  assert(GPSSerial.pair&&GPSSerial.period==period&&!gpsServiceStarted);
 }
 std::cout<<"PASS actual PCAS checksums/cold and retained-baud boot, "<<1000/period<<" Hz rate monitor and counter/time wrap\n";
}

void uartIntegration(){
 GPSSerial={};gnssCollector=gnss_snapshot::Collector(115200);diagnosticUtc={};gps={};gpsRate={};
 gpsServiceStarted=false;gpsBacklogDrops=0;diagnosticRawLength=0;diagnosticRawSplits=0;diagnostic_store::records.clear();
 clockMs=100;GPSSerial.queue(wire(rmc())+wire(gga()));serviceGps();
 gnss_snapshot::Snapshot s;assert(!gnssCollector.sample(clockMs,s)&&gps.bytes==0&&gpsBacklogDrops==0);
 clockMs=200;const auto position=wire(rmc())+wire(gga());GPSSerial.queue(position);serviceGps();
 assert(gnssCollector.sample(clockMs,s)&&s.haveRmc&&s.haveGga&&s.fix&&gps.bytes==position.size());
#if defined(FIELD_DIAGNOSTIC)
 std::string raw;bool discarded=false;
 for(const auto& r:diagnostic_store::records){if(r.kind==1)raw+=r.bytes;if(r.kind==6)discarded=true;}
 assert(raw==position&&discarded&&diagnosticUtc.matches(s.epochMsOfDay,clockMs));
#else
 assert(diagnostic_store::records.empty());
#endif
 clockMs=401;GPSSerial.queue(wire(rmc("120001.125")));serviceGps();
 assert(!gnssCollector.sample(clockMs,s)&&gpsBacklogDrops==1&&!diagnosticUtc.matches(43200125,clockMs));
 // The bounded raw path retains ordering and labels all discarded bytes.
 diagnostic_store::records.clear();diagnosticRawLength=0;
 const std::string longLine="$"+std::string(199,'x')+"\n";
 for(char c:longLine)diagnosticByte(c,500);
 assert(diagnostic_store::records.size()==2&&diagnostic_store::records[0].bytes.size()==128&&diagnosticRawSplits==1);
 assert(diagnostic_store::records[0].bytes+diagnostic_store::records[1].bytes==longLine);
 diagnostic_store::records.clear();for(char c:std::string("$partial"))diagnosticByte(c,501);
 diagnosticByte('x',502,true);diagnosticByte('\n',502,true);
 assert(diagnostic_store::records.size()==2&&diagnostic_store::records[0].kind==1&&diagnostic_store::records[0].bytes=="$partial");
 assert(diagnostic_store::records[1].kind==6&&diagnostic_store::records[1].bytes=="x\n");
 std::cout<<"PASS actual UART service preserves raw accepted/discarded bytes, bounded splitting and backlog invalidation\n";
}
int main(){utcTests();codecTests();phaseTests();rateAndCommands();uartIntegration();}
'''

with tempfile.TemporaryDirectory(prefix="shore-field-diagnostic-") as tmp:
    work = Path(tmp)
    source = work / "test.cpp"
    source.write_text(CPP)
    for diagnostic in [False, True]:
        binary = work / ("diagnostic" if diagnostic else "normal")
        flags = ["-DFIELD_DIAGNOSTIC"] if diagnostic else []
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
                        *flags, "-I", str(ROOT / "include"), str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
