#!/usr/bin/env python3
"""Real SD worker/encoders + Client producers, fake media and power (no hardware)."""
import json
from pathlib import Path
import re
import subprocess
import tempfile
from test_sd_backend import STUB, ROOT

SOURCE = (ROOT / 'src/main.cpp').read_text()
def block(marker):
    start = SOURCE.index(marker)
    body = SOURCE.index('{', start)
    depth = 0
    for match in re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', SOURCE[body:]):
        if match.group() == '{': depth += 1
        elif match.group() == '}':
            depth -= 1
            if not depth: return SOURCE[start:body + match.end()]
    raise ValueError(marker)

CPP = r'''
#include "sd_encode.cpp"
#include "axiom_encode.cpp"
#include "sd_log.cpp"
#include "client_sd_policy.h"
#include "gnss_rate.h"
#include "async_lora_tx.h"
gnss_snapshot::Collector gnssCollector;
gnss_rate::Monitor gpsRate;
client_sd::Gate clientLogGate;
loop_metrics::Gap clientLoopGap;
uint16_t nodeId=0xe91c,cachedBatteryMv=4000;
uint32_t gpsBacklogDrops=0,clientTxCount=0,clientTxErrors=0,dataSkippedSlots=0,radioRecoverCount=0;
uint32_t clientLogTxStarted=0;uint8_t clientLogTxLength=0,clientTxBuffer[MAX_PACKET_LEN]{};
'''
CPP += block('static void serviceClientSd(') + '\n' + block('static void recordClientTx(')
CPP += r'''
bool rail=false, powerOk=true,partialPower=false;unsigned powerOns=0,powerOffs=0;
bool power(bool on){if(!powerOk)return false;rail=on;if(on)++powerOns;else ++powerOffs;return !(on&&partialPower);}
void run(unsigned ms){const uint32_t until=clockMs+ms;onDelay=[=](){if(clockMs>=until)throw Done{};};try{sd_log::worker(nullptr);}catch(Done&){}onDelay=nullptr;}
gnss_snapshot::Snapshot good(){gnss_snapshot::Snapshot s;s.haveEpoch=s.fix=s.haveRmc=s.haveGga=true;s.satellites=9;s.hdop=1.1;s.lat=24.1;s.lon=121.1;s.arrivalAgeMs=0;s.epochMsOfDay=1000;return s;}
std::string all(){std::string out;for(auto &f:data)out+=f.second;return out;}
void add(const char*name,const std::string&body,uint64_t size=0){std::string p="/sd/logs/";p+=name;int f=fakeOpen(p.c_str(),O_CREAT|O_EXCL|O_RDWR);data[f]=body;fileSizes[f]=size?size:body.size();}
void checkPolicy(){
 client_sd::Gate gate;auto s=good();assert(gate.observe(0,s));
 s.epochMsOfDay+=500;s.haveGga=false;s.satellites=255;s.hdop=NAN;assert(gate.observe(500,s));assert(gate.observe(649,s));assert(!gate.observe(650,s));
 s=good();s.satellites=6;s.hdop=3;assert(gate.observe(700,s));s.hdop=3.01;assert(!gate.observe(710,s));
 s=good();s.satellites=5;assert(!gate.observe(720,s));s.satellites=255;assert(!gate.observe(720,s));
 s=good();s.arrivalAgeMs=2000;assert(!gate.observe(800,s));s=good();s.fix=false;assert(!gate.observe(900,s));
 s=good();s.lat=NAN;assert(!gate.observe(900,s));s=good();s.hdop=-1;assert(!gate.observe(900,s));
 s=good();assert(gate.observe(UINT32_MAX-100,s));s.epochMsOfDay+=500;s.haveGga=false;assert(gate.observe(UINT32_MAX-50,s));assert(!gate.observe(100,s));
 uint64_t seq;bool legacy;assert(sd_retention::name("000000000000000a-0000.ndjson",seq,legacy)&&seq==10&&!legacy);
 assert(sd_retention::name("abcdef12-0001.ndjson",seq,legacy)&&legacy);
 assert(!sd_retention::name("../000000000000000a-0000.ndjson",seq,legacy));
 assert(!sd_retention::name("000000000000000g-0000.ndjson",seq,legacy));
}
int main(int argc,char**argv){assert(argc==2);std::string scenario=argv[1];checkPolicy();
 if(scenario.rfind("retention",0)==0){
   const std::string own="{\"kind\":\"session\",\"app\":\"shore-spotter\"}\n";
   if(scenario!="retention_foreign") {
     add("abcdef12-0000.ndjson","{\"kind\":\"session\",\"schema_version\":1,\"firmware\":\"0.6-dev\",\"frequency_mhz\":923.2,\"sync_ms\":5000}\n",300000000);
     add("0000000000000002-0000.ndjson",own,300000000);
   }
   add("0000000000000099-0000.ndjson","{\"kind\":\"private\"}\n");
   add("holiday.ndjson",own);diskUsed=7800000000ULL;
   if(scenario=="retention_delete")deleteOk=false;
   if(scenario=="retention_space")spaceOk=false;
   sd_log::begin(123,true);run(1500);
   if(scenario=="retention"){
     assert(sd_log::shared.deletedFiles==1&&sd_log::shared.deletedBytes==300000000);
     assert(paths[0]=="/sd/logs/deleted"&&paths[1]!="/sd/logs/deleted");
     assert(paths.back()=="/sd/logs/000000000000009a-0000.ndjson");
     sd_log::stop();run(1000);sd_log::start();run(1000);assert(paths.back()=="/sd/logs/000000000000009b-0000.ndjson");
     assert(all().find("private")!=std::string::npos);sd_log::stop();run(1000);
   } else {assert(sd_log::shared.state==sd_log::State::Error&&sd_log::shared.deletedFiles==0);}
   std::cout<<sd_log::statusJson()<<'\n';return 0;
 }
 sd_log::begin(123,true,power);run(1000);
 assert(!rail&&powerOns==0&&paths.empty());
 if(scenario=="usb_empty") {
   directoryError=ENOENT;Serial.input="SD LIST\n";sd_log::serviceUsb();run(1000);
   assert(Serial.output=="@SD LIST BEGIN\n@SD LIST END\n"&&!rail&&formats==0&&paths.empty());
   directoryError=EIO;Serial.output.clear();Serial.input="SD LIST\n";sd_log::serviceUsb();run(1000);
   assert(Serial.output.find("@SD ERROR logs_directory_unavailable")!=std::string::npos&&!rail);
   std::cout<<sd_log::statusJson()<<'\n';return 0;
 }
 auto s=good();const auto before=clockMs;serviceClientSd(s);assert(clockMs==before&&!rail);
 if(scenario=="cancel") {s.fix=false;serviceClientSd(s);run(1000);assert(paths.empty()&&!rail);std::cout<<sd_log::statusJson()<<'\n';return 0;}
 if(scenario=="power")powerOk=false;
 if(scenario=="partial_power")partialPower=true;
 if(scenario=="mount")mountOk=false;
 run(1000);
 if(scenario=="power"||scenario=="mount"||scenario=="partial_power") {
   assert(sd_log::shared.state==sd_log::State::Error&&!sd_log::autoEnabled.load());
   if(scenario=="partial_power")assert(!rail&&powerOns==1&&powerOffs==1);
   auto attempts=powerOns;run(10000);assert(powerOns==attempts);
   std::cout<<sd_log::statusJson()<<'\n';return 0;
 }
 assert(rail&&powerOns==1);
 if(scenario=="usb") {
   Serial.input="SD READ LAST\n";sd_log::serviceUsb();run(1500);
   assert(!rail&&!sd_log::autoEnabled.load()&&Serial.output.find("@SD END ")!=std::string::npos);
   s.fix=false;serviceClientSd(s);Serial.input="SD START\n";sd_log::serviceUsb();run(500);
   assert(!rail&&sd_log::autoEnabled.load()&&!sd_log::gpsAllowed.load());
   std::cout<<sd_log::statusJson()<<'\n';return 0;
 }
 clientLogTxLength=protocol::encodeData(clientTxBuffer,sizeof(clientTxBuffer),PacketHeader{nodeId,42,MSG_DATA},PositionPayload{});
 clientLogTxStarted=clockMs;recordClientTx({async_lora_tx::Event::Started,0});
 clockMs+=330;recordClientTx({async_lora_tx::Event::Sent,0});
 recordClientTx({async_lora_tx::Event::Failed,-7});recordClientTx({async_lora_tx::Event::Timeout,-5});
 if(scenario=="queue")for(int i=0;i<80;++i)recordClientTx({async_lora_tx::Event::Sent,0});
 if(scenario=="sync")syncOk=false;
 s.fix=false;serviceClientSd(s);run(1500);
 assert(!rail&&powerOffs==1&&sd_log::stopped());
 if(scenario=="sync") {assert(sd_log::shared.errors==1);std::cout<<sd_log::statusJson()<<'\n';return 0;}
 assert(all().find("gps_pause")!=std::string::npos);
 auto bytes=sd_log::shared.bytes;run(15000);assert(bytes==sd_log::shared.bytes&&!rail&&powerOns==1);
 if(scenario=="queue")assert(sd_log::shared.clientDrops>0);
 s=good();serviceClientSd(s);run(1000);assert(rail&&powerOns==2);
 sd_log::stop();run(1000);assert(!rail);
 serviceClientSd(s);run(1000);assert(!rail); // explicit STOP remains stopped despite good GPS
 assert(sd_log::start());run(1000);assert(rail);
 sd_log::stop();run(1000);
 assert(sd_log::shared.bytes==sd_log::shared.syncedBytes);
 std::cout<<sd_log::statusJson()<<'\n'<<all();
}
'''

with tempfile.TemporaryDirectory(prefix='shore-client-sd-') as tmp:
    folder = Path(tmp)
    (folder / 'stub.h').write_text(STUB)
    for name in ['Arduino.h', 'SD.h', 'SPI.h', 'freertos/FreeRTOS.h', 'freertos/queue.h', 'freertos/task.h', 'sd_diskio.h', 'ff.h', 'esp_vfs_fat.h']:
        path = folder / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "stub.h"\n')
    (folder / 'main.cpp').write_text(CPP)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DARDUINO', '-DROLE_CLIENT',
                    '-I' + str(folder), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'src'),
                    str(folder / 'main.cpp'), '-o', str(folder / 'test')], check=True)
    for scenario in ['success', 'cancel', 'queue', 'power', 'partial_power', 'mount', 'sync', 'usb', 'usb_empty', 'retention', 'retention_foreign', 'retention_delete', 'retention_space']:
        rows = [json.loads(line) for line in subprocess.check_output([str(folder / 'test'), scenario], text=True).splitlines()]
        if scenario == 'success':
            events = [row for row in rows[1:] if row['kind'] == 'client_event']
            assert {'gps_epoch', 'gps_resume', 'gps_pause', 'tx_started', 'tx_sent', 'tx_failed', 'tx_timeout'} <= {row['event'] for row in events}
            tx = next(row for row in events if row['event'] == 'tx_sent')
            assert tx['seq'] == 42 and tx['type'] == 1 and len(bytes.fromhex(tx['raw_hex'])) == 18
            assert next(row for row in events if row['event'] == 'gps_pause')['fix'] is False
        print('PASS Client SD / retention', scenario)
