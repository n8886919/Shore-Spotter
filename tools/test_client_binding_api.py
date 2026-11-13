#!/usr/bin/env python3
"""Compile real binding HTTP/NVS code; no hardware, network or actuator I/O."""
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'src/main.cpp').read_text()


def block(marker):
    start = SOURCE.index(marker)
    body = SOURCE.index('{', start)
    depth = 0
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    for match in re.finditer(tokens, SOURCE[body:]):
        if match.group() == '{':
            depth += 1
        elif match.group() == '}':
            depth -= 1
            if not depth:
                return SOURCE[start:body + match.end()]
    raise ValueError(marker)


cpp = r'''
#include <cassert>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include "client_binding.h"
#define F(x) x
using String=std::string;
uint16_t gpsClientId=0;
constexpr uint16_t DEFAULT_GPS_CLIENT_ID=0;
struct Preferences {
  std::map<String,uint16_t> values;
  bool fail=false; int writes=0;
  bool isKey(const char*k){return values.count(k);}
  uint16_t getUShort(const char*k,uint16_t d){return isKey(k)?values[k]:d;}
  String getString(const char*,const char*d){return d;}
  size_t putUShort(const char*k,uint16_t v){++writes;if(fail)return 0;values[k]=v;return sizeof(v);}
} prefs;
struct {void println(const char*){}} Log;
struct Http {
  std::map<String,String> params; int code=0; String body,type;
  String arg(const char*k){return params[k];}
  void send(int c,const char*t,const String&b){code=c;type=t;body=b;}
} httpServer;
int manualCalls=0,resetCalls=0;
void enterManual(){++manualCalls;}
namespace station_extensions {
struct Link { bool busy=false; bool pending() const {return busy;} } link;
bool scanning=false,txActive=false;
void resetClient(){++resetCalls;}
}
namespace station_ble { bool available=true; bool tryBindingLock(){return available;} void unlockBinding(){} }
// Opaque cached values; the real setter owns when each cache is reset.
struct ClientState {int evidence=0;} gpsClient;
namespace mcu_temperature {struct Received {int evidence=0;};}
namespace rf_reception {struct Latest {int evidence=0;};}
namespace command_freshness {struct RadioSequence {int evidence=0;};}
namespace gnss_diagnostics {struct Latest {int evidence=0;};}
mcu_temperature::Received clientMcuTemperature;
rf_reception::Latest rfReception;
command_freshness::RadioSequence gpsSequence;
gnss_diagnostics::Latest clientGnssDiagnostic;
bool haveClientDiagnostic=false,rxWinHaveSeq=false;
int lastDataIntervalMs=0,rssiRingIdx=0,rssiRingCount=0,pktsThisWindow=0,pktWindowStartMs=0;
float cachedPktRate=0;
int rxWinData=0,rxWinTelem=0,rxWinDrop=0,rxWinErr=0,rxWinMissing=0,rxWinRssiSum=0,rxWinSnrSum=0;
'''
for marker in ['static void loadGpsClientBinding()',
               'static bool setGpsClientBinding(uint16_t id)',
               'static String buildWhitelistJson()']:
    cpp += block(marker) + '\n'
for method in ['GET', 'POST']:
    b = block(f'httpServer.on("/api/whitelist", HTTP_{method}, []()')
    cpp += f'void handle{method}()' + b[b.index('{'):] + '\n'
cpp += r'''
void post(const String&a,const String&id=""){
  httpServer.params={{"action",a},{"id",id}};handlePOST();
  assert(httpServer.type=="application/json");
  std::cout<<"REPLY "<<httpServer.code<<" "<<httpServer.body<<"\n";
}
void expectBinding(uint16_t id){
  assert(gpsClientId==id&&prefs.values.at("gpsclient")==id);
  gpsClientId=0x1234;loadGpsClientBinding();assert(gpsClientId==id);
}
int main(){
  loadGpsClientBinding();handleGET();
  assert(httpServer.code==200&&httpServer.body=="{\"whitelist\":[],\"count\":0,\"capacity\":1}");
  gpsClient.evidence=7;clientMcuTemperature.evidence=9;haveClientDiagnostic=true;
  post("set","fb27");assert(httpServer.code==200);expectBinding(0xFB27);
  assert(manualCalls==1&&resetCalls==1&&gpsClient.evidence==0&&clientMcuTemperature.evidence==0&&!haveClientDiagnostic);
  handleGET();assert(httpServer.body=="{\"whitelist\":[\"FB27\"],\"count\":1,\"capacity\":1}");
  gpsClient.evidence=7;post("set","FB27");
  assert(httpServer.code==200&&manualCalls==1&&resetCalls==1&&gpsClient.evidence==7);
  for(const auto*id:{"","0","0000","10","0010","FFFF","ffff","0xFB27","64295","12345"," FB27","FB27 ","FG27"}){
    const int writes=prefs.writes;post("set",id);assert(httpServer.code==400&&prefs.writes==writes);expectBinding(0xFB27);
  }
  station_extensions::link.busy=true; post("set","F035"); assert(httpServer.code==409 && gpsClientId==0xFB27);station_extensions::link.busy=false;
  station_extensions::scanning=true; post("clear"); assert(httpServer.code==409 && gpsClientId==0xFB27);station_extensions::scanning=false;
  station_ble::available=false;post("set","F035");assert(httpServer.code==503 && gpsClientId==0xFB27);station_ble::available=true;
  post("bogus","FB27");assert(httpServer.code==400);expectBinding(0xFB27);
  post("add","F035");assert(httpServer.code==409);expectBinding(0xFB27);
  post("add","FB27");assert(httpServer.code==200&&manualCalls==1);
  post("remove","F035");assert(httpServer.code==200&&manualCalls==1);expectBinding(0xFB27);
  prefs.fail=true;post("set","F035");assert(httpServer.code==503);
  assert(manualCalls==1&&resetCalls==1&&gpsClient.evidence==7);expectBinding(0xFB27);
  post("clear");assert(httpServer.code==503);expectBinding(0xFB27);
  prefs.fail=false;post("set","F035");assert(httpServer.code==200);expectBinding(0xF035);
  assert(manualCalls==2&&resetCalls==2);
  post("clear");assert(httpServer.code==200);expectBinding(0);assert(manualCalls==3);
  post("add","a");assert(httpServer.code==200);expectBinding(0x000A);
  assert(httpServer.body=="{\"whitelist\":[\"000A\"],\"count\":1,\"capacity\":1}");
  post("remove","a");assert(httpServer.code==200);expectBinding(0);
}
'''

spec = json.loads((ROOT / 'docs/openapi-client-binding.json').read_text())
assert spec['openapi'] == '3.0.3'
operation = spec['paths']['/api/whitelist']['post']
assert 'requestBody' not in operation
assert {p['name'] for p in operation['parameters']} == {'action', 'id'}
assert all(p['in'] == 'query' for p in operation['parameters'])


def check_refs(value):
    if isinstance(value, dict):
        if '$ref' in value:
            assert value['$ref'].startswith('#/')
            target = spec
            for key in value['$ref'][2:].split('/'):
                target = target[key]
        for child in value.values():
            check_refs(child)
    elif isinstance(value, list):
        for child in value:
            check_refs(child)


check_refs(spec)
seen = set()
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory)
    (p / 'test.cpp').write_text(cpp)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-I', str(ROOT / 'include'),
                    str(p / 'test.cpp'), '-o', str(p / 'test')], check=True)
    result = subprocess.check_output([str(p / 'test')], text=True)
    for line in result.splitlines():
        _, status, body = line.split(' ', 2)
        seen.add(status)
        assert status in operation['responses']
        data = json.loads(body)
        if status == '200':
            assert data['capacity'] == 1 and data['count'] == len(data['whitelist']) <= 1
            assert all(re.fullmatch(r'[0-9A-F]{4}', value) for value in data['whitelist'])
        else:
            assert data['ok'] is False and isinstance(data['error'], str)
assert seen == set(operation['responses'])
print('PASS real binding GET/POST: set/add/remove/clear, ID validation, 200/400/409/503 JSON')
print('PASS real NVS setter/reload: persistence, failed-save retention, same-ID preservation and cache reset')
print('PASS OpenAPI local references, query contract and documented response status coverage')
