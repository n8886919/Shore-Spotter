#!/usr/bin/env python3
"""Compile the real SD worker/encoders with bounded queues and failing card I/O.
No device, card, network or credentials are accessed.
"""
from pathlib import Path
import json, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[1]
STUB = r"""
#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
using String=std::string;
struct Console {std::string input,output;size_t fakeWrite(const uint8_t*p,size_t n){n=std::min(n,size_t(37));output.append((const char*)p,n);return n;} int available(){return input.size();} int fakeRead(){int c=input[0];input.erase(0,1);return c;}} Serial;
using BYTE=uint8_t;using LBA_t=uint32_t;using FRESULT=int;
constexpr int FR_OK=0,FR_DISK_ERR=1,FM_FAT32=2,FF_MAX_SS=4096;
struct MKFS_PARM {BYTE fmt;BYTE n_fat;unsigned align,n_root,au_size;};
int formats=0, partitionsMade=0;


uint32_t clockMs=0; uint32_t millis(){return clockMs;} uint32_t micros(){return clockMs*1000;}
struct Esp {uint32_t getFreeHeap(){return 200000;}} ESP;
constexpr int HSPI=2,OUTPUT=1,HIGH=1,INPUT=0;
void pinMode(int p,int){assert(p==34||p==35||p==36||p==47);} void digitalWrite(int p,int){assert(p==34||p==35||p==36||p==47);}
struct SPIClass { void end(){} explicit SPIClass(int bus){assert(bus==HSPI);} bool begin(int s,int i,int o,int c){assert(s==36&&i==37&&o==35&&c==47);return true;} };
bool mountOk=true, writeOk=true, syncOk=true, readOk=true, openOk=true;
uint64_t diskUsed=0; bool deleteOk=true,spaceOk=true;
constexpr int ESP_OK=0;
int esp_vfs_fat_info(const char*,uint64_t*t,uint64_t*f){*t=8000000000ULL;*f=*t-diskUsed;return spaceOk?0:-1;}
struct Sd {bool begin(int cs,SPIClass&,int hz,const char*,int,bool format){assert(cs==47&&hz==1000000&&!format);return mountOk;}
 void end(){} bool exists(const char*){return true;} bool mkdir(const char*){return true;} uint64_t cardSize(){return 8000000000ULL;} uint64_t totalBytes(){return 8000000000ULL;} uint64_t usedBytes(){return diskUsed;}} SD;
uint8_t sdcard_init(int cs,SPIClass*,int hz){assert(cs==47&&hz==1000000);return 0;}
bool sdcard_mount(uint8_t,const char*,int,bool format){assert(!format);return mountOk;}
void sdcard_unmount(uint8_t){} void sdcard_uninit(uint8_t){}
uint32_t sdcard_num_sectors(uint8_t){return 16000000;}
bool sd_read_raw(uint8_t,uint8_t*,uint32_t){return true;}
int f_mount(void*,const char*,int){return 0;}
int f_fdisk(uint8_t,const LBA_t*p,void*work){assert(p[0]==100&&p[1]==0);memset(work,0,FF_MAX_SS);++partitionsMade;return FR_OK;}
int f_mkfs(const char*,const MKFS_PARM*p,void*work,unsigned n){assert(p->fmt==FM_FAT32&&n==512);memset(work,0,FF_MAX_SS);++formats;mountOk=true;return FR_OK;}
using TaskHandle_t=void*;using portMUX_TYPE=int;
constexpr int portMUX_INITIALIZER_UNLOCKED=0,pdTRUE=1,pdPASS=1;
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(v) (v)
struct Queue {size_t cap,size;std::deque<std::vector<uint8_t>> data;};using QueueHandle_t=Queue*;
QueueHandle_t xQueueCreate(size_t cap,size_t size){return new Queue{cap,size,{}};}
void vQueueDelete(QueueHandle_t q){delete q;}
int xQueueSend(QueueHandle_t q,const void*p,int wait){assert(!wait);if(q->data.size()==q->cap)return 0;q->data.emplace_back((const uint8_t*)p,(const uint8_t*)p+q->size);return 1;}
int xQueueReceive(QueueHandle_t q,void*p,int wait){assert(!wait);if(q->data.empty())return 0;memcpy(p,q->data.front().data(),q->size);q->data.pop_front();return 1;}
size_t uxQueueMessagesWaiting(QueueHandle_t q){return q->data.size();}
size_t uxTaskGetStackHighWaterMark(TaskHandle_t){return 4096;}
void vTaskPrioritySet(TaskHandle_t,int priority){assert(priority==0||priority==1);}
int xTaskCreatePinnedToCore(void(*)(void*),const char*,int stack,void*,int pri,TaskHandle_t*t,int core){assert(stack>=16384&&pri==1&&core==0);*t=(void*)1;return pdPASS;}
struct Done{};std::function<void()> onDelay;
void vTaskDelay(uint32_t ms){clockMs+=ms;if(onDelay)onDelay();}
std::map<int,std::string> data;std::vector<std::string> paths;int nextFd=1, closed=0;
std::map<int,size_t> offsets; std::map<int,uint64_t> fileSizes;
int fakeOpen(const char *p,int flags,int=0){if(flags==O_RDONLY){for(size_t i=0;i<paths.size();++i)if(paths[i]==p){offsets[i+1]=0;return i+1;}return -1;}assert(flags&O_EXCL);if(!openOk){errno=ENOSPC;return -1;}paths.emplace_back(p);data[nextFd]="";return nextFd++;}
int shortAfter=-1;bool shortTriggered=false;
ssize_t fakeWrite(int f,const void*p,size_t n){assert(n<=512);clockMs+=3;if(!writeOk){errno=ENOSPC;return -1;}
 if(shortAfter==0){n=std::min(n,size_t(17));shortAfter=-1;shortTriggered=true;}else if(shortAfter>0)--shortAfter;
 data[f].append((const char*)p,n);return n;}
int fakeFsync(int){clockMs+=2;return syncOk?0:-1;}
ssize_t fakePread(int f,void*p,size_t n,off_t at){if(!readOk)return -1;assert(at>=0);n=std::min(n,data[f].size()-size_t(at));memcpy(p,data[f].data()+at,n);return n;}
ssize_t fakeRead(int f,void*p,size_t n){n=std::min(n,data[f].size()-offsets[f]);memcpy(p,data[f].data()+offsets[f],n);offsets[f]+=n;return n;}
int fakeFstat(int f,struct stat*s){s->st_size=fileSizes.count(f)?fileSizes[f]:data[f].size();s->st_mode=S_IFREG;return 0;}
int fakeStat(const char*p,struct stat*s){const int f=fakeOpen(p,O_RDONLY);return f<0?-1:fakeFstat(f,s);}
size_t dirIndex=0;int directoryError=0;
DIR* fakeOpendir(const char*){dirIndex=0;if(directoryError){errno=directoryError;return nullptr;}return reinterpret_cast<DIR*>(1);}
dirent* fakeReaddir(DIR*){static dirent entry;if(dirIndex>=paths.size())return nullptr;snprintf(entry.d_name,sizeof(entry.d_name),"%s",paths[dirIndex++].c_str()+9);return &entry;}
int fakeClosedir(DIR*){return 0;}
int fakeUnlink(const char*p){if(!deleteOk)return -1;for(size_t i=0;i<paths.size();++i)if(paths[i]==p){diskUsed-=fileSizes.count(i+1)?fileSizes[i+1]:data[i+1].size();paths[i]="/sd/logs/deleted";data[i+1].clear();return 0;}return -1;}
int fakeClose(int){++closed;return 0;}
int fakeGettimeofday(timeval*t,void*){t->tv_sec=0;t->tv_usec=0;return 0;}
#define open fakeOpen
#define write fakeWrite
#define fsync fakeFsync
#define pread fakePread
#define close fakeClose
#define unlink fakeUnlink
#define read(...) fakeRead(__VA_ARGS__)
#define stat(...) fakeStat(__VA_ARGS__)
#define fstat fakeFstat
#define opendir fakeOpendir
#define readdir fakeReaddir
#define closedir fakeClosedir
#define gettimeofday fakeGettimeofday
"""
MAIN = r"""
#include "sd_encode.cpp"
#include "axiom_encode.cpp"
#include "sd_log.cpp"
void run(unsigned ms){const uint32_t until=clockMs+ms;onDelay=[=](){if(clockMs>=until)throw Done{};};try{sd_log::worker(nullptr);}catch(Done&){}onDelay=nullptr;}
void command(const std::string&s){Serial.output.clear();Serial.input=s+"\n";sd_log::serviceUsb();run(500);assert(!sd_log::usbTransferActive());}
int main(int argc,char**argv){
 assert(argc==2);const std::string scenario=argv[1];
 if(scenario=="mount")mountOk=false;
 if(scenario=="open")openOk=false;
 sd_log::begin(1234,scenario!="power");
 if(scenario=="usb"||scenario=="format") {
   run(100);command("SD STOP");assert(sd_log::stopped());assert(Serial.output.find("stopped")!=std::string::npos);
   command("SD FORMAT");assert(formats==0&&Serial.output.find("@SD ERROR")!=std::string::npos);
   command("SD READ /logs/../secret.ndjson");assert(Serial.output.find("@SD ERROR")!=std::string::npos);
   if(scenario=="format"){mountOk=false;command("SD FORMAT FAT32 ERASE");assert(formats==1&&partitionsMade==1&&Serial.output.find("@SD FORMAT OK FAT32")!=std::string::npos);}
   else {
     command("SD LIST");assert(Serial.output.find(paths[0].substr(3))!=std::string::npos);
     command("SD READ LAST");const auto&payload=data[1];assert(Serial.output.find(payload)!=std::string::npos);
     char tail[64];snprintf(tail,sizeof(tail),"@SD END %08x",sd_log::crc32(0xffffffffU,(const uint8_t*)payload.data(),payload.size())^0xffffffffU);
     assert(Serial.output.find(tail)!=std::string::npos);assert(formats==0);
   }
   command("SD START");assert(!sd_log::stopped());command("SD STOP");assert(sd_log::stopped());
   std::cout<<sd_log::statusJson()<<'\n';return 0;
 }
 if(scenario=="cancel") {sd_log::stop();run(100);assert(sd_log::stopped());std::cout<<sd_log::statusJson()<<'\n';return 0;}
 uint8_t raw[255];for(unsigned i=0;i<255;++i)raw[i]=i;
 packet_diagnostics::Event event;event.id=1;event.ms=987;event.kind=packet_diagnostics::Kind::RadioError;event.code=-7;event.length=255;event.rssiDbm10=-1103;event.snrQuarterDb=-30;
 sd_log::packet(event,raw,sizeof(raw));
 const uint8_t text[]="hello \"world\"\n";sd_log::text(text,sizeof(text)-1,988);
 axiom_log::Sample sample;sample.bootId=1234;sample.ms=989;strcpy(sample.mode,"manual");strcpy(sample.source,"hold");sd_log::submit(sample,micros());
 if(scenario=="power"){assert(sd_log::stopped());std::cout<<sd_log::statusJson()<<'\n';return 0;}
 if(scenario=="queue")for(unsigned i=0;i<40;++i)sd_log::packet(event,raw,255);
 run(200);
 if(scenario=="write")writeOk=false;
 if(scenario=="short")shortAfter=1;
 if(scenario=="sync")syncOk=false;
 if(scenario=="read")readOk=false;
 if(scenario=="rotate") {data[sd_log::fd].resize(sd_log::kRotateBytes, ' ');sd_log::fileBytes=sd_log::kRotateBytes;}
 if(scenario!="rotate")sd_log::stop();
 run(scenario=="rotate"?5500:500);
 if(scenario=="success"){
   assert(sd_log::stopped());assert(sd_log::start());run(100);
   sd_log::submit(sample,micros());sd_log::stop();run(500);assert(paths.size()==2&&paths[0]!=paths[1]);
 }
 if(scenario=="rotate") {sd_log::stop();run(500);}
 if(scenario=="short")assert(shortTriggered&&sd_log::shared.unconfirmedBytes>0&&sd_log::shared.bytes>sd_log::shared.syncedBytes);
 std::cout<<sd_log::statusJson()<<'\n';
 if(scenario=="success"||scenario=="queue")for(auto &f:data)std::cout<<f.second;
}
"""
def main():
    with tempfile.TemporaryDirectory(prefix='shore-sd-test-') as tmp:
        folder=Path(tmp)
        (folder/'stub.h').write_text(STUB)
        for name in ['Arduino.h','SD.h','SPI.h','freertos/FreeRTOS.h','freertos/queue.h','freertos/task.h','sd_diskio.h','ff.h','esp_vfs_fat.h']:
            dest=folder/name;dest.parent.mkdir(exist_ok=True,parents=True);dest.write_text('#include "stub.h"\n')
        (folder/'main.cpp').write_text(MAIN)
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-DARDUINO','-DROLE_STATION',
                        '-I'+str(folder),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'src'),str(folder/'main.cpp'),'-o',str(folder/'test')],check=True)
        for scenario in ['success','queue','mount','open','power','write','short','sync','read','rotate','cancel','usb','format']:
            result=subprocess.check_output([str(folder/'test'),scenario],text=True)
            rows=[json.loads(line) for line in result.splitlines()]
            status=rows[0]
            if scenario in ['cancel','usb','format']:assert status['state']=='stopped'
            elif scenario in ['success','queue','rotate']:
                assert status['state']=='stopped',status
                assert status['bytes']==status['synced_bytes'],status
                assert status['readback_checks']>=2,status
            else:
                assert status['state']=='error' and status['error'],status
                assert not status['enabled'],status
            if scenario=='queue':assert status['packet_dropped']==9 and status['packets']==32,status
            if scenario=='success':
                packet=next(r for r in rows[1:] if r['kind']=='lora_packet')
                assert packet['raw_hex']==bytes(range(255)).hex() and packet['code']==-7 and packet['rssi_dbm']==-110.3
                assert not packet['raw_truncated']
                text=next(r for r in rows[1:] if r['kind']=='text_log');assert text['text']=='hello "world"\n'
                snap=next(r for r in rows[1:] if r['kind']=='station_snapshot');assert snap['_time'] is None
                assert len([r for r in rows[1:] if r['kind']=='session'])==2
            print('PASS SD',scenario)
    print('PASS real SD worker: zero-wait queue, 255-byte CRC raw, offline snapshots, escaping, stop/restart, rotation, no format, I/O failure visibility')

if __name__ == "__main__":
    main()
