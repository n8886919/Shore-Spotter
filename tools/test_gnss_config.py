"""Exercise actual GNSS boot commands and rate diagnostics without hardware."""
from pathlib import Path
import re, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'src/main.cpp').read_text()
start = source.index('static void configureGps()')
end = source.index('\nstatic void serviceGps()', start)
function = source[start:end]
commands = re.findall(r'GPSSerial.print\("(\$[^"\n]+)', function)
assert len(commands) == 5  # Both preprocessor branches; each firmware sends four.
for wire in commands:
    body, checksum = wire[1:].split('*')
    actual = 0
    for c in body: actual ^= ord(c)
    assert actual == int(checksum[:2], 16), (body, actual, checksum)
assert source.count('  configureGps();') == 2
cpp = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include "gnss_snapshot.h"
#include "gnss_rate.h"
#define F(x) x
constexpr int GPS_EN_PIN=7,GPS_RX_PIN=9,GPS_TX_PIN=8,OUTPUT=1,HIGH=1,SERIAL_8N1=0;
constexpr uint32_t GPS_BAUD=gnss_rate::kBaud,GPS_RX_BUFFER_BYTES=1024;
uint32_t now=0;uint32_t millis(){return now;}void delay(uint32_t n){now+=n;}
void pinMode(int,int){}void digitalWrite(int,int){}
struct Uart {
  int baud=0,receiver=9600,rate=1000;bool pair=false,buffer=false;
  std::vector<std::string> accepted;
  void setRxBufferSize(int n){assert(n>=1024);buffer=true;}
  void begin(int b,int,int,int){assert(buffer);baud=b;}
  void updateBaudRate(int b){baud=b;}
  void flush(){}
  void print(const char *s){
    if(baud!=receiver)return; // wrong baud does not configure the module
    accepted.emplace_back(s);const std::string line=s;
    if(line.find("PCAS01,5")!=std::string::npos)receiver=115200;
    if(line.find("PCAS02,500")!=std::string::npos)rate=500;
    if(line.find("PCAS02,1000")!=std::string::npos)rate=1000;
    if(line.find("PCAS03,1,0,0,0,1")!=std::string::npos)pair=true;
  }
} GPSSerial;
struct LogStub{void println(const char*){}}Log;
gnss_snapshot::Collector gnssCollector{GPS_BAUD};
bool gpsServiceStarted=true;
'''
cpp += function
cpp += r'''
int main(){
 for(int initial:{9600,115200}){
  GPSSerial={};GPSSerial.receiver=initial;gpsServiceStarted=true;
  configureGps();
  assert(GPSSerial.receiver==115200&&GPSSerial.baud==115200&&GPSSerial.rate==gnss_rate::kTargetIntervalMs&&GPSSerial.pair);
  assert(!gpsServiceStarted);
 }
}
'''
with tempfile.TemporaryDirectory(prefix='shore-gps-config-') as td:
    path=Path(td); (path/'test.cpp').write_text(cpp)
    for flags in [[], ['-DFIELD_DIAGNOSTIC']]:
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',*flags,'-I',str(root/'include'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
        subprocess.run([str(path/'test')],check=True)
print('PASS actual PCAS command checksums, normal 2Hz/diagnostic 1Hz, both-role boot wiring, 9600 cold boot and retained-115200 MCU reboot; hardware acceptance remains unverified')
