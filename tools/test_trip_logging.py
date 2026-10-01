#!/usr/bin/env python3
"""Trip profile checks with real encoders, SD worker/producers and Flash worker.

Only temporary host binaries and fake storage are used. Simulated scheduling
checks queue/retention contracts; it does not measure card latency or field RF.
Pass --unit-only while the main.cpp trip integration is being edited.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

from test_sd_backend import STUB as SD_STUB
from test_diagnostic_store import STUB as FLASH_STUB

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    body = source.index('{', start)
    depth = 0
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    for match in re.finditer(tokens, source[body:]):
        if match.group() == '{':
            depth += 1
        elif match.group() == '}':
            depth -= 1
            if not depth:
                return source[start:body + match.end()]
    raise ValueError(marker)


RAW_MAIN = r'''
#include <cassert>
#include <iostream>
#include <string>
#include "trip_log.h"
int main() {
  static_assert(trip_log::kRawChunk == 96);
  static_assert(trip_log::kSnapshotMs == 60000 && trip_log::kSdStatusMs == 300000);
  trip_log::ContinuousPlan plan;
  for(uint32_t ms : {0U, 1U, 60000U, 0xffffffffU}) {
    assert(!plan.observe(ms, false)); assert(!plan.observe(ms, true)); plan.abort();
    assert(plan.phase() == 7 && plan.rf() && plan.sd());
  }
  uint8_t data[97]; for(unsigned i=0;i<sizeof(data);++i)data[i]=i;
  char out[256]; memset(out, '!', sizeof(out));
  size_t n=trip_log::encodeRaw(out,sizeof(out),UINT32_MAX,6,data,96);
  assert(n>192 && n<sizeof(out) && out[n]=='\0' && out[n-1]=='\n');
  assert(std::string(out).find("seq=4294967295 kind=6 hex=00010203")!=std::string::npos);
  char exact[256]; memset(exact, '!', sizeof(exact));
  assert(!trip_log::encodeRaw(exact,n,UINT32_MAX,6,data,96) && exact[0]=='!');
  assert(trip_log::encodeRaw(exact,n+1,UINT32_MAX,6,data,96)==n);
  assert(!trip_log::encodeRaw(exact,sizeof(exact),1,1,data,97));
  assert(!trip_log::encodeRaw(exact,sizeof(exact),1,2,data,1));
  assert(!trip_log::encodeRaw(exact,sizeof(exact),1,1,nullptr,1));
  assert(!trip_log::encodeRaw(nullptr,sizeof(exact),1,1,data,1));
  assert(trip_log::encodeRaw(exact,sizeof(exact),0,1,nullptr,0)>0);
  std::cout << "PASS trip raw boundaries, exact capacity, plan stays RF/SD on across abort/wrap\n";
}
'''

SD_PREFIX = r'''
#include "sd_encode.cpp"
#include "axiom_encode.cpp"
#include "sd_log.cpp"
#include "client_sd_policy.h"
#include "gnss_rate.h"
#include "async_lora_tx.h"
#include "trip_log.h"
#include "field_diagnostic.h"
// SD and DIAG share the parser in a real trip build. Raw UART and TX producers
// must not submit any Flash record; this stub fails immediately if they do.
namespace diagnostic_store {
bool transferActive(){return false;}
bool command(const char*){return false;}
bool submit(uint16_t,const void*,size_t,uint32_t){assert(false);return false;}
}
gnss_snapshot::Collector gnssCollector;
gnss_rate::Monitor gpsRate;
client_sd::Gate clientLogGate;
loop_metrics::Gap clientLoopGap;
uint16_t nodeId=0xe91c,cachedBatteryMv=4000;
uint32_t gpsBacklogDrops=0,clientTxCount=0,clientTxErrors=0,dataSkippedSlots=0,radioRecoverCount=0;
uint32_t clientLogTxStarted=0;uint8_t clientLogTxLength=0,clientTxBuffer[MAX_PACKET_LEN]{};
field_diagnostic::Utc diagnosticUtc;
uint8_t diagnosticRaw[128]{};size_t diagnosticRawLength=0;
uint16_t diagnosticRawKind=1;uint32_t diagnosticRawSplits=0,tripRawSequence=0;
'''

SD_MAIN = r'''
bool rail=false;unsigned powerOns=0,powerOffs=0;
bool power(bool on){rail=on;if(on)++powerOns;else ++powerOffs;return true;}
void run(unsigned duration){const auto until=clockMs+duration;onDelay=[=](){if(clockMs>=until)throw Done{};};try{sd_log::worker(nullptr);}catch(Done&){}onDelay=nullptr;}
void raw(uint32_t seq,uint16_t kind,const uint8_t*bytes,size_t n){char text[256];const auto len=trip_log::encodeRaw(text,sizeof(text),seq,kind,bytes,n);assert(len);sd_log::text(reinterpret_cast<const uint8_t*>(text),len,millis());}
gnss_snapshot::Snapshot invalidEpoch(){gnss_snapshot::Snapshot s;s.haveEpoch=s.haveRmc=s.haveGga=true;s.fix=false;s.satellites=0;s.hdop=NAN;s.epochMsOfDay=1000;return s;}
void finish(){sd_log::stop();run(2000);assert(!rail&&sd_log::stopped());}
int main(int argc,char**argv){
  assert(argc==2);const std::string scenario=argv[1];
  sd_log::begin(123,true,power);run(250);assert(!rail);
  // The actual trip producer must start SD even before a valid fix exists.
  auto sample=invalidEpoch();const uint32_t before=clockMs;serviceClientSd(sample);
  assert(clockMs==before);run(1000);assert(rail&&sd_log::autoEnabled.load());
  uint8_t bytes[256];for(unsigned i=0;i<sizeof(bytes);++i)bytes[i]=i;
  if(scenario=="uart"){
    for(uint8_t byte:bytes)diagnosticByte(char(byte),clockMs,false);
    for(uint8_t byte:std::vector<uint8_t>{0,255,'\r','\n'})diagnosticByte(char(byte),clockMs,true);
    assert(!diagnosticRawLength && diagnosticRawSplits>=1);finish();
  }else if(scenario=="roundtrip"){
    for(size_t at=0,seq=0;at<sizeof(bytes);at+=96,++seq)raw(seq,seq==1?6:1,bytes+at,std::min(size_t(96),sizeof(bytes)-at));
    sample.haveEpoch=sample.haveRmc=sample.haveGga=false;serviceClientSd(sample);run(6000);
    assert(rail&&powerOffs==0&&sd_log::shared.textDrops==0);finish();
  }else if(scenario=="queue"){
    for(unsigned i=0;i<17;++i)raw(i,1,bytes,96);
    assert(sd_log::shared.textDrops==1&&uxQueueMessagesWaiting(sd_log::texts)==16);
    run(1000);raw(17,6,bytes,96);finish();
  }else if(scenario=="stop"){
    raw(0,1,bytes,96);Serial.input="SD STOP\n";sd_log::serviceUsb();run(2000);
    assert(!rail&&sd_log::stopped()&&!sd_log::autoEnabled.load());
    sample.epochMsOfDay+=1000;serviceClientSd(sample);run(1000);assert(!rail);
    assert(Serial.output.find("stopped")!=std::string::npos);
  }else if(scenario=="failure"){
    writeOk=false;raw(0,1,bytes,96);run(2000);
    assert(!rail&&sd_log::shared.errors==1&&!sd_log::autoEnabled.load());
    const auto attempts=powerOns;serviceClientSd(sample);run(1000);assert(powerOns==attempts);
  }else if(scenario=="stream"){
    for(unsigned second=0;second<60;++second){
      sample.epochMsOfDay=1000+1000*second;serviceClientSd(sample);
      raw(2*second,1,bytes,96);raw(2*second+1,6,bytes+96,96);
      clientLogTxLength=protocol::encodeData(clientTxBuffer,sizeof(clientTxBuffer),PacketHeader{nodeId,uint16_t(second),MSG_DATA},PositionPayload{});
      clientLogTxStarted=millis();recordClientTx({async_lora_tx::Event::Started,0});
      recordClientTx({async_lora_tx::Event::Sent,0});run(1000);
    }
    assert(sd_log::shared.textDrops==0&&sd_log::shared.clientDrops==0);finish();
  }else assert(false);
  std::cout<<sd_log::statusJson()<<'\n';
  for(const auto& f:data)std::cout<<f.second;
}
'''

FLASH_MAIN = r'''
#include "diagnostic_store.cpp"
#include "trip_log.h"
void run(uint32_t duration){const uint32_t until=clockMs+duration;onDelay=[=](){if(clockMs>=until)throw Done{};};try{diagnostic_store::worker(nullptr);}catch(Done&){}onDelay=nullptr;}
int main(){
  diagnostic_store::begin(123);run(500);
  uint8_t bytes[400]{};
  // Reserve seven full startup metadata frames, including the init and first
  // runtime PMU raw samples in addition to boot/key/profile/phase evidence.
  for(unsigned i=0;i<7;++i)assert(diagnostic_store::submit(2,bytes,400,clockMs));
  run(1100);
  // Worst permitted small health: 384 bytes shares exactly one frame with
  // the 80-byte snapshot. Full SD status uses the maximum 1599-byte buffer
  // content and newline every five minutes. Use the real worker and NOR model.
  for(unsigned minute=0;minute<24*60;++minute){
    clockMs=2000+minute*trip_log::kSnapshotMs;
    assert(diagnostic_store::submit(3,bytes,80,clockMs));
    assert(diagnostic_store::submit(5,bytes,384,clockMs));
    if(minute%5==0){
      for(unsigned n:{400U,400U,400U,399U})assert(diagnostic_store::submit(8,bytes,n,clockMs));
      assert(diagnostic_store::submit(8,"\n",1,clockMs));
    }
    diagnostic_store::drainRecords();clockMs+=1100;diagnostic_store::drainRecords();
    if(minute%5==0){
      // Maximum diagnostic JSON buffer content, deliberately in a separate
      // flush window so packing with the normal snapshot cannot hide demand.
      clockMs+=1400;
      assert(diagnostic_store::submit(2,bytes,319,clockMs));
      diagnostic_store::drainRecords();clockMs+=1100;diagnostic_store::drainRecords();
    }
  }
  assert(diagnostic_store::healthy());
  assert(diagnostic_store::shared.usedFrames==2888);
  assert(diagnostic_store::shared.dropped==0&&diagnostic_store::shared.errors==0);
  assert(diagnostic_store::shared.corruptFrames==0&&erases==0);
  std::cout<<diagnostic_store::statusJson()<<'\n';
}
'''


def build(folder, name, source, stub=None, defines=(), headers=()):
    work = folder / name
    work.mkdir()
    if stub:
        (work / 'stub.h').write_text(stub)
        for header in headers:
            dest = work / header
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text('#include "stub.h"\n')
    (work / 'main.cpp').write_text(source)
    executable = work / 'test'
    subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    *['-D'+value for value in defines], '-I'+str(work),
                    '-I'+str(ROOT/'include'), '-I'+str(ROOT/'src'),
                    str(work/'main.cpp'), '-o', str(executable)], check=True)
    return executable


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unit-only', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='shore-trip-test-') as tmp:
        folder = Path(tmp)
        raw = build(folder, 'raw', RAW_MAIN)
        print(subprocess.check_output([str(raw)], text=True).strip())
        flash = build(folder, 'flash', FLASH_MAIN, FLASH_STUB,
                      ('ARDUINO', 'FIELD_DIAGNOSTIC', 'CLIENT_TRIP_LOG'),
                      ('Arduino.h', 'esp_partition.h', 'freertos/FreeRTOS.h',
                       'freertos/queue.h', 'freertos/task.h'))
        capacity = json.loads(subprocess.check_output([str(flash)], text=True))
        print('PASS actual Flash worker: 24h modeled, 2888/3072 frames, '
              f'{capacity["used_frames"]*512} bytes, no drops/errors/erases; event storms excluded')
        if args.unit_only:
            return
        source = (ROOT/'src/main.cpp').read_text()
        producers = block(source, 'static void serviceClientSd(') + '\n'
        producers += block(source, 'static void recordClientTx(') + '\n'
        producers += block(source, 'static void recordTripRaw(') + '\n'
        producers += block(source, 'static void diagnosticByte(') + '\n'
        sd = build(folder, 'sd', SD_PREFIX + producers + SD_MAIN, SD_STUB,
                   ('ARDUINO', 'ROLE_CLIENT', 'FIELD_DIAGNOSTIC', 'CLIENT_TRIP_LOG'),
                   ('Arduino.h', 'SD.h', 'SPI.h', 'freertos/FreeRTOS.h',
                    'freertos/queue.h', 'freertos/task.h', 'sd_diskio.h', 'ff.h',
                    'esp_vfs_fat.h'))
        pattern = re.compile(r'\[GNSS_RAW seq=(\d+) kind=([16]) hex=([0-9a-f]*)\]\n')
        for scenario in ('uart', 'roundtrip', 'queue', 'stop', 'failure', 'stream'):
            output = subprocess.check_output([str(sd), scenario], text=True)
            rows = [json.loads(line) for line in output.splitlines()]
            status = rows[0]
            raw_rows = []
            for row in rows[1:]:
                if row.get('kind') != 'text_log':
                    continue
                match = pattern.fullmatch(row['text'])
                assert match, row
                raw_rows.append((int(match[1]), int(match[2]), bytes.fromhex(match[3])))
            if scenario == 'uart':
                assert b''.join(row[2] for row in raw_rows if row[1] == 1) == bytes(range(256))
                assert b''.join(row[2] for row in raw_rows if row[1] == 6) == bytes([0, 255, 13, 10])
                assert [row[0] for row in raw_rows] == list(range(len(raw_rows)))
                assert max(len(row[2]) for row in raw_rows) <= 96
            elif scenario == 'roundtrip':
                assert b''.join(row[2] for row in raw_rows) == bytes(range(256))
                assert [row[1] for row in raw_rows] == [1, 6, 1]
            elif scenario == 'queue':
                assert status['text_dropped'] == 1
                assert [row[0] for row in raw_rows] == list(range(16)) + [17]
            elif scenario == 'stop':
                assert status['bytes'] == status['synced_bytes'] and len(raw_rows) == 1
            elif scenario == 'failure':
                assert status['state'] == 'error' and not status['auto_enabled']
            elif scenario == 'stream':
                assert len(raw_rows) == 120
                events = [row for row in rows[1:] if row.get('kind') == 'client_event']
                assert sum(row['event'] == 'tx_started' for row in events) == 60
                assert sum(row['event'] == 'tx_sent' for row in events) == 60
                assert status['text_dropped'] == status['client_dropped'] == 0
            print('PASS actual trip SD worker/producers:', scenario)


if __name__ == '__main__':
    main()
