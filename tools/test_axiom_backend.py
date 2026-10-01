#!/usr/bin/env python3
"""Run the real encoder, settings handler and worker against fake ESP transports.
Needs the installed PlatformIO cJSON header and host libcjson (no credentials/network).
"""
from pathlib import Path
import ctypes.util
import json
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/main.cpp').read_text()
start = source.index('static void handleAxiomSettings()')
end = source.index('\nstatic void initWebServer()', start)
handler = source[start:end]
headers = list((Path.home() / '.platformio/packages').glob('framework-arduinoespressif32-libs/esp32s3/include/json/cJSON/cJSON.h'))
lib = ctypes.util.find_library('cjson')
if not headers or not lib:
    raise SystemExit('Requires PlatformIO ESP32 framework and host libcjson runtime')

stub = r'''
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <sys/time.h>
struct String : std::string {
  using std::string::string; using std::string::operator=; using std::string::operator+=;
  String(const std::string &v) : std::string(v) {}
  template<class T, typename std::enable_if<std::is_integral<T>::value,int>::type=0>
  explicit String(T v) : std::string(std::to_string(v)) {}
};
uint32_t clockMs=0, heapFree=300000; bool wifi=true, clockReady=true;
uint32_t millis(){return clockMs;} uint32_t micros(){return clockMs*1000;}
struct Esp {uint32_t getFreeHeap(){return heapFree;}} ESP;
struct Wifi {int status(){return wifi?3:0;}} WiFi;
constexpr int WL_CONNECTED=3;
int ntpCalls=0;
void configTime(int,int,const char*,const char*){++ntpCalls;}
void esp_sntp_stop(){}
time_t fakeTime(time_t*){return clockReady?1760000000+clockMs/1000:0;}
int fakeGetTime(timeval *v,void*){v->tv_sec=fakeTime(nullptr);v->tv_usec=(clockMs%1000)*1000;return 0;}
#define time fakeTime
#define gettimeofday fakeGetTime
constexpr int MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2;
uint32_t heap_caps_get_largest_free_block(int){return heapFree;}
using BaseType_t=int; using TaskHandle_t=void*; using portMUX_TYPE=int;
constexpr int portMUX_INITIALIZER_UNLOCKED=0,pdTRUE=1,pdPASS=1;
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(x) (x)
struct Queue {size_t cap,itemSize;std::deque<std::vector<uint8_t>> items;};
using QueueHandle_t=Queue*;
QueueHandle_t xQueueCreate(size_t n,size_t bytes){return new Queue{n,bytes,{}};}
void vQueueDelete(QueueHandle_t q){delete q;}
int xQueueSend(QueueHandle_t q,const void *s,int wait){assert(wait==0);if(q->items.size()==q->cap)return 0;
 q->items.emplace_back((const uint8_t*)s,(const uint8_t*)s+q->itemSize);return 1;}
int xQueueReceive(QueueHandle_t q,void *s,int wait){assert(wait==0);if(q->items.empty())return 0;
 memcpy(s,q->items.front().data(),q->itemSize);q->items.pop_front();return 1;}
size_t uxQueueSpacesAvailable(QueueHandle_t q){return q->cap-q->items.size();}
size_t uxQueueMessagesWaiting(QueueHandle_t q){return q->items.size();}
size_t uxTaskGetStackHighWaterMark(TaskHandle_t){return 4096;}
int xTaskCreatePinnedToCore(void(*)(void*),const char*,int stack,void*,int priority,TaskHandle_t *t,int core){
 assert(stack>=8192&&priority==1&&core==0);*t=(void*)1;return 1;}
std::function<void()> onDelay;
void vTaskDelay(uint32_t ms){clockMs+=ms;if(onDelay)onDelay();}
std::vector<uint8_t> savedNvs; bool saveFail=false;
struct Preferences {
 bool begin(const char *name,bool readOnly){assert(std::string(name)=="axiom_log");return !readOnly||!savedNvs.empty();}
 size_t getBytesLength(const char*){return savedNvs.size();}
 size_t getBytes(const char*,void *out,size_t n){if(savedNvs.size()!=n)return 0;memcpy(out,savedNvs.data(),n);return n;}
 size_t putBytes(const char*,const void *in,size_t n){if(saveFail)return 0;savedNvs.assign((const uint8_t*)in,(const uint8_t*)in+n);return n;}
 void end(){}
};
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_ERR_NO_MEM=-1,ESP_ERR_INVALID_STATE=-2;
constexpr int HTTP_EVENT_ON_DATA=1,HTTP_EVENT_ON_HEADER=2,HTTP_METHOD_POST=3;
esp_err_t esp_crt_bundle_attach(void*){return 0;}
struct esp_http_client_event_t {void *user_data;int event_id;int data_len;void *data;char *header_key;char *header_value;};
struct esp_http_client_config_t {const char *url;int method,timeout_ms;esp_err_t(*crt_bundle_attach)(void*);
 bool disable_auto_redirect;esp_err_t(*event_handler)(esp_http_client_event_t*);void *user_data;int buffer_size,buffer_size_tx;};
struct Client {esp_http_client_config_t options;std::string auth,payload;};
using esp_http_client_handle_t=Client*;
int performCalls=0,httpCode=200,transportError=0;std::string responseBody,retryAfter,lastPayload;
std::function<void()> duringUpload;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *o){
 assert(o->crt_bundle_attach&&o->disable_auto_redirect&&o->timeout_ms==3000);
 assert(std::string(o->url).find("https://us-east-1.aws.edge.axiom.co/v1/ingest/shore-spotter")==0);
 return new Client{*o,{},{}};
}
int esp_http_client_set_header(Client *c,const char *name,const char *v){
 if(std::string(name)=="Authorization")c->auth=v;else assert(std::string(v)=="application/x-ndjson");return 0;}
int esp_http_client_set_post_field(Client *c,const char *b,size_t n){c->payload.assign(b,n);return 0;}
int esp_http_client_perform(Client *c){
 ++performCalls;assert(c->auth=="Bearer test-ingest-only");lastPayload=c->payload;
 const auto count=std::count(c->payload.begin(),c->payload.end(),'\n');
 std::string body=responseBody.empty()?"{\"ingested\":"+std::to_string(count)+",\"failed\":0}":responseBody;
 if(!retryAfter.empty()){esp_http_client_event_t e{c->options.user_data,HTTP_EVENT_ON_HEADER,0,nullptr,(char*)"Retry-After",retryAfter.data()};c->options.event_handler(&e);}
 esp_http_client_event_t e{c->options.user_data,HTTP_EVENT_ON_DATA,int(body.size()),body.data(),nullptr,nullptr};c->options.event_handler(&e);
 if(duringUpload)duringUpload();return transportError;
}
int esp_http_client_get_status_code(Client*){return httpCode;}
void esp_http_client_cleanup(Client *c){delete c;}
'''

tests = r'''
using namespace axiom_log;
struct Stop {};
std::function<void(axiom_log::Sample&)> decorateSample;
void reset(){
 if(samples)vQueueDelete(samples);samples=nullptr;task=nullptr;
 shared=Shared{};collecting=false;otaPaused=false;generation=1;nextSampleMs=0;sampleId=0;
 clockMs=0;heapFree=300000;wifi=true;clockReady=true;ntpCalls=0;performCalls=0;
 httpCode=200;transportError=0;responseBody.clear();retryAfter.clear();lastPayload.clear();
 duringUpload={};onDelay={};decorateSample={};savedNvs.clear();saveFail=false;
}
Config enabled(){Config c;c.enabled=1;strcpy(c.token,"test-ingest-only");return c;}
void produce(){
 if(captureDue(clockMs,false)){Sample s;s.ms=clockMs;s.bootId=12;s.nodeId=34;s.eventCount=1;
   s.events[0].id=clockMs/1000+1;s.events[0].rawLength=17;if(decorateSample)decorateSample(s);submit(s,micros());}
}
// Execute the actual perpetual worker until a condition is reached; let it clean
// the batch/queue on disable before stopping the fake task at its next delay.
void runUntil(std::function<bool()> done,uint32_t limit=30000,bool makeSamples=true){
 bool stopping=false;
 onDelay=[&]{
  if(stopping)throw Stop{};
  assert(clockMs<limit);
  if(done()){collecting=false;stopping=true;return;}
  if(makeSamples)produce();
 };
 try{worker(nullptr);}catch(const Stop&){}
 onDelay={};
}
struct Http {
 String body;int argCount=1,code=0;String reply;bool bodyHeaders=true;
 String arg(const char*){return body;}int args(){return argCount;}
 String header(const char *name){return !bodyHeaders?String{}:std::string(name)=="Content-Type"?String("application/json"):String(body.length());}
 void sendHeader(const char *name,const char *v){assert(std::string(name)=="Cache-Control"&&std::string(v)=="no-store");}
 void send(int c,const char*,const String &s){code=c;reply=s;}
} httpServer;
'''
main = r'''

const cJSON *field(const cJSON *j,const char *name,const char *child=nullptr){
 const auto *v=cJSON_GetObjectItemCaseSensitive(j,name);
 return child?cJSON_GetObjectItemCaseSensitive(v,child):v;
}
void readableLogs(){
 Sample s; s.bootId=10;s.generation=2;s.clientId=3;s.ms=100;s.pwmOk=true;s.angle=90;
 strcpy(s.mode,"uart");strcpy(s.source,"hold");
 LogHistory h;char out[kBatchBytes];size_t records=99;
 auto encode=[&]{const auto n=encodeSample(out,sizeof(out),s,1760000000123,0,0,&h,&records);assert(n);return n;};
 encode();assert(records==1);
 auto *j=cJSON_Parse(out);assert(cJSON_IsNull(field(j,"delta","interval_ms")));
 assert(std::string(field(j,"level")->valuestring)=="info");
 assert(std::string(field(j,"message")->valuestring).find("uart / waiting")!=std::string::npos);
 assert(std::string(field(j,"message")->valuestring).find("New errors")==std::string::npos);
 for(const char *name:{"seq","fix","course_deg10","rssi_dbm","snr_db"})assert(cJSON_IsNull(field(j,"client",name)));
 assert(cJSON_IsNull(field(j,"client_diag","tx_errors"))&&cJSON_IsNull(field(j,"client_gnss","checksum")));
 assert(std::string(field(j,"client_gnss","state")->valuestring)=="unknown");cJSON_Delete(j);
 assert(cJSON_IsNull(field((j=cJSON_Parse(out)),"station_average","mean_lat")));
 assert(cJSON_IsNull(field(j,"gnss_rate","epoch_hz")));cJSON_Delete(j);
 s.stationSamples=60;s.stationLat=24.1;s.stationLon=121.1;s.stationRmsM=3.5;s.stationWarning=true;
 s.gpsHz=2;s.gpsRmcHz=2;s.gpsGgaHz=0;s.finishingGpsTarget=true;
 s.ms+=1000;encode();assert(records==1); // unchanged heartbeat, no duplicate transition
 j=cJSON_Parse(out);
 assert(field(j,"station_average","samples")->valueint==60);
 assert(std::abs(field(j,"station_average","mean_lat")->valuedouble-24.1)<1e-8);
 assert(cJSON_IsTrue(field(j,"station_average","warning")));
 assert(field(j,"gnss_rate","epoch_hz")->valueint==2&&field(j,"gnss_rate","gga_hz")->valueint==0);
 assert(cJSON_IsTrue(field(j,"control","finishing_last_gps_target")));
 assert(cJSON_IsNull(field(j,"station_gps","lat")));cJSON_Delete(j);
 j=cJSON_Parse(out);assert(field(j,"delta","interval_ms")->valueint==1000);cJSON_Delete(j);
 // First packet, stale boundary and recovery emit one additional document.
 s.clientPresent=true;s.clientRxAge=1999;s.clientFix=1;s.clientDiagAge=0;s.clientGnssAge=0;s.ms+=1000;
 encode();assert(records==2);j=cJSON_Parse(out);
 assert(field(j,"client","fix")->valueint==1&&field(j,"client_diag","tx_errors")->valueint==0);
 cJSON_Delete(j);j=cJSON_Parse(strchr(out,'\n')+1);
 assert(std::string(field(j,"kind")->valuestring)=="state_change");assert(cJSON_GetArraySize(field(j,"changes"))==1);cJSON_Delete(j);
 s.clientRxAge=2000;s.ms+=1000;encode();assert(records==2);
 j=cJSON_Parse(out);assert(std::string(field(j,"level")->valuestring)=="warn");cJSON_Delete(j);
 s.ms+=1000;encode();assert(records==1);
 s.clientRxAge=0;s.ms+=1000;encode();assert(records==2);
 j=cJSON_Parse(out);assert(std::string(field(j,"level")->valuestring)=="info");cJSON_Delete(j);
 // GPS unavailability matters in GPS mode, UART waiting remains informational.
 strcpy(s.mode,"gps");s.ms+=1000;encode();j=cJSON_Parse(out);
 assert(std::string(field(j,"level")->valuestring)=="warn");cJSON_Delete(j);
 s.gpsUsable=true;strcpy(s.source,"gps");s.ms+=1000;encode();assert(records==2);
 j=cJSON_Parse(out);assert(std::string(field(j,"level")->valuestring)=="info");cJSON_Delete(j);
 // Incremental counters warn only on new errors, and wrap without a huge spike.
 s.counters[14]=2;s.counters[4]=3;s.controlGapOver250=1;s.gnssCounters.checksumErrors=4;s.ms+=1000;
 encode();j=cJSON_Parse(out);assert(cJSON_IsNull(field(j,"delta","ack_errors")));assert(cJSON_IsFalse(field(j,"radio","ack_enabled")));assert(cJSON_IsNull(field(j,"radio","ack_sent")));assert(field(j,"delta","radio_errors")->valueint==3);
 assert(std::string(field(j,"message")->valuestring).find("New errors: RF 3")!=std::string::npos);cJSON_Delete(j);
 s.ms+=1000;encode();j=cJSON_Parse(out);assert(std::string(field(j,"level")->valuestring)=="info");
 assert(cJSON_IsNull(field(j,"delta","ack_errors")));cJSON_Delete(j);
 h.radioErrors=UINT32_MAX-1;s.counters[4]=1;h.ms=UINT32_MAX-99;s.ms=100;
 encode();j=cJSON_Parse(out);assert(field(j,"delta","radio_errors")->valueint==3);
 assert(field(j,"delta","interval_ms")->valueint==200);cJSON_Delete(j);
 // A rebind, boot or configuration generation begins a new comparison baseline.
 for(int i=0;i<3;++i){if(i==0)++s.bootId;else if(i==1)++s.generation;else ++s.clientId;
  s.pwmOk=!s.pwmOk;s.ms+=1000;encode();assert(records==1);j=cJSON_Parse(out);
  assert(cJSON_IsNull(field(j,"delta","ack_errors")));cJSON_Delete(j);}
 // All seven simultaneous changes and full raw packet pages still fit a batch.
 s.pwmOk=true;s.motionFault=false;s.gpsUsable=true;s.clientRxAge=0;strcpy(s.mode,"gps");strcpy(s.source,"gps");
 s.stationGnss.byteAgeMs=0;s.stationGnss.sentenceAgeMs=0;s.stationGnss.sourceAgeMs=0;s.stationGnss.flags=3;
 s.ms+=1000;encode();
 s.pwmOk=false;s.motionFault=true;s.gpsUsable=false;s.clientRxAge=2000;strcpy(s.mode,"uart");strcpy(s.source,"hold");
 s.stationGnss.byteAgeMs=UINT16_MAX;s.eventCount=8;s.ms+=1000;
 for(auto &e:s.events){e.rawLength=17;memset(e.raw,0xff,17);e.id=UINT32_MAX;}
 // An insufficient buffer must not consume the transition baseline.
 const auto previous=h;char tiny[64];records=99;
 assert(!encodeSample(tiny,sizeof(tiny),s,1760000000123,0,0,&h,&records)&&records==0);
 assert(h.ms==previous.ms&&h.pwmOk==previous.pwmOk);
 const auto n=encode();assert(records==2&&n*5<kBatchBytes);
 j=cJSON_Parse(out);assert(std::string(field(j,"level")->valuestring)=="error");cJSON_Delete(j);
 j=cJSON_Parse(strchr(out,'\n')+1);assert(cJSON_GetArraySize(field(j,"changes"))==7);cJSON_Delete(j);
 std::cout<<out;std::cerr<<"max paired records bytes="<<n<<", history RAM="<<sizeof(LogHistory)<<"\n";
 auto cloudHistory=previous;char cloudOut[kBatchBytes];
 assert(encodeSample(cloudOut,sizeof(cloudOut),s,1760000000123,0,0,&cloudHistory,&records,true));
 assert(records==2&&strstr(cloudOut,"\"field\":\"server_gnss\""));std::cout<<cloudOut;
 s.ms+=1000;s.pwmOk=true;s.motionFault=false;encode();assert(records==2);
 std::cerr<<"PASS log summaries, unknown values, transitions/recovery, mode severity, deltas/wrap/reset and bounded atomic encoding\n";
}

int main(){
 setenv("TZ","UTC",1);tzset();
 Config c;assert(!c.enabled&&validConfig(c));c.enabled=1;assert(!validConfig(c));
 c=enabled();assert(validConfig(c));strcpy(c.dataset,"../token");assert(!validConfig(c));
 c=enabled();strcpy(c.token,"secret\r\nInjected: yes");assert(!validConfig(c));
 c=enabled();memset(c.token,'x',sizeof(c.token));assert(!validConfig(c));
 assert(backoffMs(1)==5000&&backoffMs(2)==10000&&backoffMs(32)==300000);
 assert(backoffMs(1,7200)==7200000&&backoffMs(1,UINT32_MAX)==86400000);
 assert(!loop_metrics::due(0xfffffff0,20)&&loop_metrics::due(21,20));
 // Exact serializer: max-sized event page, invalid GPS as null, bounded overflow.
 Sample s;s.eventCount=8;s.ms=42;s.bootId=123;s.nodeId=456;s.gps.hdop=NAN;
 strcpy(s.mode,"uart");strcpy(s.uartState,"a\"b\\c\n");
 for(auto &e:s.events){e.rawLength=17;e.id=UINT32_MAX;memset(e.raw,0xff,17);}
 char payload[kBatchBytes];const size_t n=encodeSample(payload,sizeof(payload),s,1760000000123,7,91);
 assert(n>0&&n*5<kBatchBytes);std::cout<<payload;
 // Cloud retains exactly the schema 2 field paths; SD exports use schema 3.
 assert(encodeSample(payload,sizeof(payload),s,1760000000123,7,91,nullptr,nullptr,true));std::cout<<payload;
 char small[32];assert(encodeSample(small,sizeof(small),s,1760000000000,0,0)==0);
 s.events[0].rawLength=37;assert(encodeSample(payload,sizeof(payload),s,1760000000000,0,0)==0);
 std::cerr<<"sample bytes="<<n<<", sample RAM="<<sizeof(Sample)<<"\n";
 readableLogs();

 // The real worker counts NDJSON records, including state changes, for full and partial receipts.
 for(bool partial:{false,true}){
  reset();assert(configure(enabled()));
  decorateSample=[](Sample &v){v.pwmOk=true;strcpy(v.mode,"uart");strcpy(v.source,"hold");
   v.clientPresent=(v.ms/1000)%2;v.clientRxAge=v.clientPresent?0:UINT32_MAX;};
  if(partial)responseBody=R"({"ingested":8,"failed":1})";
  runUntil([]{return performCalls>0;});
  assert(std::count(lastPayload.begin(),lastPayload.end(),'\n')==9);
  auto *cloud=cJSON_Parse(lastPayload.c_str());
  assert(field(cloud,"schema_version")->valueint==2);
  assert(field(cloud,"server_gps")&&field(cloud,"server_gnss")&&field(cloud,"station"));
  assert(!field(cloud,"station_gps")&&!field(cloud,"station_gnss")&&!field(cloud,"station_average"));
  cJSON_Delete(cloud);
  assert(shared.sent==(partial?8:9)&&shared.failed==(partial?1:0));
  assert(shared.haveSuccess==!partial);
 }
 // Fresh/default disabled: no queue/task/NTP/transport, even with Wi-Fi available.
 reset();begin();assert(!task&&!samples&&!captureDue(1000,false)&&ntpCalls==0&&performCalls==0);
 // POST validates bounded body; URL arguments and invalid data cannot configure.
 httpServer.body=R"({"enabled":true,"dataset":"shore-spotter","region":"us"})";
 handleAxiomSettings();assert(httpServer.code==400);
 httpServer.body=R"({"enabled":true,"dataset":"shore-spotter","region":"us","token":"test-ingest-only"})";
 httpServer.bodyHeaders=false;handleAxiomSettings();assert(httpServer.code==400&&!task);
 httpServer.bodyHeaders=true;
 httpServer.argCount=2;handleAxiomSettings();assert(httpServer.code==400&&!task);
 httpServer.argCount=1;handleAxiomSettings();assert(httpServer.code==202&&shared.saving&&!collecting);
 assert(httpServer.reply.find("test-ingest-only")==std::string::npos);
 handleAxiomSettings();assert(httpServer.code==409);
 runUntil([]{return shared.sent>=5;});assert(shared.sent>=5&&shared.failed==0&&performCalls==1);
 assert(lastPayload.find("test-ingest-only")==std::string::npos&&savedNvs.size()==sizeof(Config));
 Config persisted;memcpy(&persisted,savedNvs.data(),sizeof(persisted));assert(persisted.enabled);
 // Startup with persisted settings enables background collection.
 begin();assert(collecting);
 // Empty token retains existing secret; explicit clearing disables and persists.
 httpServer.body=R"({"enabled":false,"dataset":"shore-spotter","region":"us","token":""})";
 handleAxiomSettings();assert(httpServer.code==202);runUntil([]{return !shared.saving;},clockMs+5000,false);
 assert(shared.config.token[0]&&!shared.config.enabled);
 httpServer.body=R"({"enabled":false,"dataset":"shore-spotter","region":"us","clear_token":true})";
 handleAxiomSettings();runUntil([]{return !shared.saving;},clockMs+5000,false);
 assert(!shared.config.token[0]);assert(statusJson().find("test-ingest-only")==std::string::npos);
 // Runtime holds on storage failure and never falsely reports enabled/success.
 reset();begin();saveFail=true;assert(configure(enabled()));
 runUntil([]{return !shared.saving;},5000,false);assert(shared.state==State::Storage&&!collecting&&!shared.config.enabled);
 // Offline queue remains fixed. Reconnect expires old samples, then sends recent data.
 reset();assert(configure(enabled()));wifi=false;
 onDelay=[] {produce();if(clockMs>=20000)throw Stop{};};try{worker(nullptr);}catch(const Stop&){}
 assert(performCalls==0&&uxQueueMessagesWaiting(samples)==kQueueSamples&&shared.dropped>0);
 wifi=true;runUntil([]{return performCalls>0;},40000);assert(shared.sent>0&&shared.dropped>=kQueueSamples);
 // TLS / DNS / HTTP failures back off and count lost data, never successful upload.
 reset();assert(configure(enabled()));transportError=-7;
 runUntil([]{return performCalls>0;});assert(shared.sent==0&&shared.failed>0&&shared.state==State::Backoff);
 reset();assert(configure(enabled()));httpCode=429;retryAfter="120";
 runUntil([]{return performCalls>0;});assert(shared.sent==0&&shared.retryAt>=120000);
 // Malformed and partial HTTP 200 bodies are not blanket successes.
 reset();assert(configure(enabled()));responseBody="{}";
 runUntil([]{return performCalls>0;});assert(shared.sent==0&&shared.failed==5);
 reset();assert(configure(enabled()));responseBody=R"({"ingested":2,"failed":3})";
 runUntil([]{return performCalls>0;});assert(shared.sent==2&&shared.failed==3&&!shared.haveSuccess);
 reset();assert(configure(enabled()));responseBody=std::string(1100,'x');
 runUntil([]{return performCalls>0;});assert(shared.sent==0&&shared.failed==5);
 // Authentication/config rejection latches until settings are saved again.
 reset();assert(configure(enabled()));httpCode=403;
 runUntil([]{return performCalls>0;});assert(shared.state==State::Rejected&&!collecting);
 assert(configure(enabled()));httpCode=200;runUntil([]{return shared.sent>0;},clockMs+15000);
 // Missing time / low heap / OTA / loop overload do not start any HTTP upload.
 reset();assert(configure(enabled()));clockReady=false;
 runUntil([]{return clockMs>6000;});assert(performCalls==0&&ntpCalls==1);
 reset();assert(configure(enabled()));heapFree=80000;
 runUntil([]{return clockMs>6000;});assert(performCalls==0&&shared.state==State::Memory);
 reset();assert(configure(enabled()));pauseForOta(true);
 runUntil([]{return clockMs>6000;});assert(performCalls==0&&shared.queued==0);
 pauseForOta(false);collecting=true;nextSampleMs=0;assert(!captureDue(clockMs,true)&&shared.dropped>0);
 // Disable accepted during an in-flight request permits only that request to finish.
 reset();assert(configure(enabled()));duringUpload=[]{Config c=enabled();c.enabled=0;assert(configure(c));};
 runUntil([]{return performCalls==1&&!shared.saving;});assert(!shared.config.enabled&&performCalls==1);
 assert(uxQueueMessagesWaiting(samples)==0);
 reset();
 std::cerr<<"PASS Axiom backend: real worker/encoder/API, TLS configuration, bounded queue, time, memory, OTA, failures, retention, clearing and in-flight disable\n";
}
'''
with tempfile.TemporaryDirectory(prefix='shore-axiom-test-') as tmp:
    tmp = Path(tmp)
    (tmp / 'fake.h').write_text(stub)
    for name in ['Arduino.h', 'Preferences.h', 'WiFi.h', 'esp_crt_bundle.h', 'esp_heap_caps.h', 'esp_http_client.h', 'esp_sntp.h']:
        (tmp / name).write_text('#include "fake.h"\n')
    (tmp / 'test.cpp').write_text('#include "fake.h"\n#include "' + str(ROOT / 'src/axiom_log.cpp') + '"\n#include "' + str(ROOT / 'src/axiom_encode.cpp') + '"\n' + tests + handler + main)
    binary = tmp / 'axiom-test'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Wno-misleading-indentation',
                    '-DARDUINO=10800', '-DROLE_STATION', '-DCONFIG_ARDUINO_RUNNING_CORE=1',
                    '-I'+str(tmp), '-I'+str(ROOT/'include'), '-I'+str(headers[0].parent),
                    str(tmp/'test.cpp'), '-l:'+lib, '-o',str(binary)],check=True)
    result = subprocess.run([str(binary)], text=True, capture_output=True)
    print(result.stderr, end='')
    result.check_returncode()
    documents = [json.loads(line) for line in result.stdout.splitlines()]
    event = documents[0]
    assert event['_time'] == '2025-10-09T08:53:20.123Z'
    assert event['station_gps']['lat'] is None and event['station_gps']['hdop'] is None
    assert event['control']['uart_state'] == 'a"b\\c\n'
    assert len(event['events']) == 8 and event['events'][0]['raw_hex'] == 'ff'*17
    fields_by_schema = {2: set(), 3: set()}
    def walk(value, fields, prefix=''):
        if isinstance(value, dict):
            for key, child in value.items():
                path = prefix+'.'+key if prefix else key
                fields.add(path)
                walk(child, fields, path)
        elif isinstance(value, list):
            for child in value: walk(child, fields, prefix)
    legacy = documents[1]
    assert legacy['schema_version'] == 2 and legacy['kind'] == 'station_snapshot'
    normalized = dict(legacy, schema_version=3)
    for old, new in [('server_gps', 'station_gps'), ('server_gnss', 'station_gnss'), ('station', 'station_average')]:
        normalized[new] = normalized.pop(old)
    assert normalized == event, 'cloud compatibility changes names only, not measurements'
    for document in documents:
        assert document["schema_version"] in (2, 3)
        walk(document, fields_by_schema[document['schema_version']])
        assert document["message"].isascii()
        assert document["level"] in ("info", "warn", "error")
    for schema, fields in fields_by_schema.items():
        assert len(fields) < 256, (schema, len(fields))
    print(f'PASS strict JSON, millisecond UTC, nulls, escapes, cloud legacy keys and per-schema field budgets: {[(s, len(f)) for s, f in fields_by_schema.items()]}')
