#!/usr/bin/env python3
"""Exercise the actual diagnostic Flash worker using an in-memory NOR partition.

No serial port, real Flash, SD card, credentials or network access is used.
"""
from pathlib import Path
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STUB = r'''
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
#include <string>
#include <vector>
using String = std::string;
uint32_t clockMs=0;
uint32_t millis(){return clockMs;}
uint32_t micros(){return clockMs*1000;}
struct Done {};
std::function<void()> onDelay;
void vTaskDelay(uint32_t ms){clockMs+=ms;if(onDelay)onDelay();}
using TaskHandle_t=void*;using portMUX_TYPE=int;
constexpr int portMUX_INITIALIZER_UNLOCKED=0,pdTRUE=1,pdPASS=1;
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(v) (v)
struct Queue {size_t cap,size;std::deque<std::vector<uint8_t>> data;};
using QueueHandle_t=Queue*;
QueueHandle_t xQueueCreate(size_t cap,size_t size){return new Queue{cap,size,{}};}
int xQueueSend(QueueHandle_t q,const void*p,int wait){assert(!wait);if(q->data.size()==q->cap)return 0;q->data.emplace_back((const uint8_t*)p,(const uint8_t*)p+q->size);return 1;}
int xQueueReceive(QueueHandle_t q,void*p,int wait){assert(!wait);if(q->data.empty())return 0;memcpy(p,q->data.front().data(),q->size);q->data.pop_front();return 1;}
size_t uxQueueMessagesWaiting(QueueHandle_t q){return q->data.size();}
int xTaskCreatePinnedToCore(void(*)(void*),const char*,int stack,void*,int pri,TaskHandle_t*t,int core){assert(stack>=6144&&pri==1&&core==0);*t=(void*)1;return pdPASS;}
struct Console {
  std::string output;bool connected=true;
  int availableForWrite(){return connected?128:0;}
  size_t write(const uint8_t*p,size_t n){n=std::min(n,size_t(37));output.append((const char*)p,n);return n;}
} Serial;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_PARTITION_TYPE_DATA=1,ESP_PARTITION_SUBTYPE_DATA_SPIFFS=0x82;
struct esp_partition_t {uint32_t address=0x670000,size=0x180000;bool encrypted=false;} fakePartition;
std::vector<uint8_t> flash(0x180000,0xff);
bool partitionAvailable=true,readOk=true,writeOk=true,eraseOk=true;
size_t writes=0,erases=0;
const esp_partition_t* esp_partition_find_first(int type,int subtype,const char*name){assert(type==1&&subtype==0x82&&!strcmp(name,"spiffs"));return partitionAvailable?&fakePartition:nullptr;}
int esp_partition_read(const esp_partition_t*p,size_t offset,void*out,size_t size){assert(p==&fakePartition&&offset+size<=flash.size());if(!readOk)return ESP_FAIL;memcpy(out,flash.data()+offset,size);return ESP_OK;}
int esp_partition_write(const esp_partition_t*p,size_t offset,const void*in,size_t size){
  assert(p==&fakePartition&&offset+size<=flash.size()&&size==512&&offset%512==0);++writes;
  const auto*data=static_cast<const uint8_t*>(in);const size_t n=writeOk?size:17;
  for(size_t i=0;i<n;++i){assert((flash[offset+i]&data[i])==data[i]);flash[offset+i]&=data[i];}
  clockMs+=2;return writeOk?ESP_OK:ESP_FAIL;
}
int esp_partition_erase_range(const esp_partition_t*p,size_t offset,size_t size){
  assert(p==&fakePartition&&offset+size<=flash.size()&&size==4096&&offset%4096==0);
  if(!eraseOk)return ESP_FAIL;
  ++erases;std::fill(flash.begin()+offset,flash.begin()+offset+size,0xff);return ESP_OK;
}
'''
MAIN = r'''
#include "diagnostic_store.cpp"
using namespace diagnostic_store;
void run(uint32_t duration){
  const uint32_t until=clockMs+duration;
  onDelay=[=](){if(clockMs>=until)throw Done{};};
  try{diagnostic_store::worker(nullptr);}catch(Done&){}
  onDelay=nullptr;
}
void commandRun(const char*line){Serial.output.clear();assert(diagnostic_store::command(line));run(2500);assert(!transferActive());}
void simulateBoot(uint32_t id){
  bootId=id;accepting.store(false);discard();initialized=false;nextSequence=0;nextIndex=0;buffered=false;
  shared={};accepting.store(true);run(1000);
}
void one(uint16_t kind,uint32_t ms,const std::string&payload){assert(submit(kind,payload.data(),payload.size(),ms));}
int main(int argc,char**argv){
  assert(argc==2);const std::string scenario=argv[1];
  if(scenario=="unknown")flash[512*20+5]=0x42;
  if(scenario=="layout")fakePartition.address=0x660000;
  if(scenario=="missing")partitionAvailable=false;
  if(scenario=="read_failure")readOk=false;
  begin(1234);
  if(scenario=="early_boot")one(1,3,"boot before scan");
  run(1500);
  if(scenario=="unknown") {
    assert(shared.state==State::Unknown&&!accepting.load()&&writes==0&&erases==0);
    assert(!submit(1,"x",1,1));
    commandRun("DIAG ERASE");assert(erases==0&&Serial.output.find("syntax")!=std::string::npos);
    commandRun("DIAG ERASE CONFIRM");assert(erases==0x180000/4096&&shared.state==State::Recording);
    assert(Serial.output.find("@DIAG ERASE OK")!=std::string::npos);
  } else if(scenario=="layout"||scenario=="missing"||scenario=="read_failure") {
    assert(shared.state==State::Error&&writes==0&&erases==0);
    if(scenario=="layout"){commandRun("DIAG ERASE CONFIRM");assert(erases==0);}
  } else if(scenario=="packed") {
    const size_t before=writes;
    one(1,100,"$GNRMC,123\r\n");one(2,110,std::string(96,'z'));one(3,120,"phase=A");
    run(200);assert(writes==before&&shared.pendingRecords==3);
    run(1000);assert(writes==before+1&&shared.records==3&&shared.validFrames==1&&codec::validFrame(flash.data()+512));
    assert(codec::get16(flash.data()+512+10)==3&&codec::get32(flash.data()+512+12)==1234);
    commandRun("DIAG READ 0 2");
    assert(Serial.output.find("\n@DIAG BEGIN 0 2\n")==0);
    assert(Serial.output.find("@DIAG FRAME 0 ")!=std::string::npos&&Serial.output.find("@DIAG FRAME 1 ")!=std::string::npos);
    assert(Serial.output.find("@DIAG END 0 2\n")!=std::string::npos);
    std::cout<<Serial.output;
  } else if(scenario=="queue") {
    for(unsigned i=0;i<40;++i)submit(5,"hello",5,100+i);
    assert(shared.dropped==8&&shared.queueHigh==32);run(1500);
    assert(shared.records==32&&shared.pendingRecords==0&&shared.validFrames==1);
    assert(codec::get32(flash.data()+512+24)==8);
  } else if(scenario=="oversize") {
    assert(!submit(9,nullptr,1,0));assert(!submit(9,"",473,0));assert(shared.dropped==2);
  } else if(scenario=="torn") {
    one(2,50,"before reset");run(1500);
    writeOk=false;one(2,75,"torn frame");run(1500);
    assert(shared.state==State::Error&&shared.corruptFrames==1&&shared.usedFrames==3);
    const auto original=flash;writeOk=true;simulateBoot(5678);
    assert(shared.corruptFrames==1&&shared.usedFrames==3&&shared.records==1);
    one(2,5,"after reset");run(1500);
    assert(shared.validFrames==2&&shared.records==2&&shared.usedFrames==4);
    assert(std::equal(original.begin(),original.begin()+1536,flash.begin()));
    assert(codec::get32(flash.data()+1536+12)==5678&&codec::get32(flash.data()+1536+16)==0);
  } else if(scenario=="full") {
    for(uint32_t i=1;i<kCapacity;++i){uint8_t f[512];codec::beginFrame(f,9,i,4);codec::append(f,7,"x",1,4);codec::seal(f);memcpy(flash.data()+i*512,f,512);}
    const auto original=flash;simulateBoot(55);
    assert(shared.state==State::Full&&shared.usedFrames==kCapacity&&!submit(1,"x",1,0));
    run(2000);assert(flash==original&&erases==0);
  } else if(scenario=="last_frame") {
    nextIndex=kCapacity-1;
    one(1,0,std::string(472,'a'));one(2,1,"cannot fit");run(1500);
    assert(shared.state==State::Full&&!accepting.load()&&shared.dropped==1&&shared.records==1);
  } else if(scenario=="usb_disconnect") {
    Serial.connected=false;assert(command("DIAG STATUS"));run(2500);
    assert(shared.usbErrors==1&&!transferActive());Serial.connected=true;
    one(1,2,"still records");run(1500);assert(shared.records==1);
  } else if(scenario=="usb_backpressure") {
    commands->cap=0;assert(!command("DIAG STATUS"));
    assert(shared.commandDrops==1&&!transferActive());
    commands->cap=1;assert(command("DIAG STATUS"));
    assert(!command("DIAG READ")&&shared.commandDrops==2&&transferActive());
    run(1000);assert(!transferActive());
    assert(Serial.output.find("@DIAG STATUS ")!=std::string::npos);
    assert(Serial.output.find("@DIAG BEGIN ")==std::string::npos);
  } else if(scenario=="read_recording") {
    for(unsigned i=0;i<20;++i)one(2,i,std::string(472,'a'));
    run(500);assert(shared.records==20);
    assert(command("DIAG READ"));
    unsigned injected=0;uint32_t last=clockMs;const uint32_t until=clockMs+2500;
    onDelay=[&](){
      if(clockMs-last>=50&&injected<20){last=clockMs;one(3,clockMs,"record while reading");++injected;}
      if(clockMs>=until)throw Done{};
    };
    try{diagnostic_store::worker(nullptr);}catch(Done&){}onDelay=nullptr;
    assert(injected==20&&shared.records==40&&shared.dropped==0&&!transferActive());
    assert(Serial.output.find("\n@DIAG BEGIN 0 21\n")==0&&Serial.output.find("@DIAG END 0 21\n")!=std::string::npos);
  } else if(scenario=="header_torn") {
    flash[0]^=1;const auto original=flash;simulateBoot(55);
    assert(shared.state==State::Unknown&&flash==original&&erases==0);
  } else if(scenario=="erase_failure") {
    one(2,3,"preserve until explicit erase");run(1500);const auto original=flash;
    eraseOk=false;commandRun("DIAG ERASE CONFIRM");
    assert(shared.state==State::Error&&flash==original&&Serial.output.find("erase_failed")!=std::string::npos);
  } else if(scenario=="syntax") {
    for(const char*s:{"DIAG READ -1","DIAG READ 1z","DIAG READ 0 0","DIAG READ 0 -1","DIAG READ 0 1 garbage"}){
      commandRun(s);assert(Serial.output.find("syntax")!=std::string::npos);
    }
    commandRun("DIAG READ 4");assert(Serial.output.find("read_range")!=std::string::npos);
    assert(!command("SD STATUS")&&!command("DIAGNOSTIC"));
  } else if(scenario=="early_boot")assert(shared.records==1);
  else if(scenario=="codec") {
    uint8_t f[512];codec::beginFrame(f,123,4,56);assert(codec::append(f,7,"abc",3,56));codec::seal(f);
    assert(codec::validFrame(f));f[40]^=1;assert(!codec::validFrame(f));
    f[40]^=1;codec::put16(f+8,481);codec::seal(f);assert(!codec::validFrame(f));
    assert(codec::crc32(reinterpret_cast<const uint8_t*>("123456789"),9)==0xcbf43926U);
  }
  std::cout<<statusJson()<<'\n';
}
'''
DISABLED = r'''
#include "diagnostic_store.cpp"
int main(){diagnostic_store::begin(1);assert(!diagnostic_store::submit(1,"x",1,0));assert(!diagnostic_store::command("DIAG ERASE CONFIRM"));assert(!diagnostic_store::transferActive());assert(writes==0&&erases==0);std::cout<<diagnostic_store::statusJson()<<'\n';}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="shore-diag-store-") as tmp:
        directory = Path(tmp)
        (directory / "stub.h").write_text(STUB)
        for name in ["Arduino.h", "esp_partition.h", "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/task.h"]:
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "stub.h"\n')
        source = directory / "main.cpp"
        source.write_text(MAIN)
        flags = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DARDUINO",
                 "-I" + str(directory), "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src")]
        binary = directory / "test"
        subprocess.run(flags + ["-DFIELD_DIAGNOSTIC", str(source), "-o", str(binary)], check=True)
        scenarios = ["packed", "queue", "oversize", "unknown", "layout", "missing", "read_failure", "torn", "full", "last_frame", "usb_disconnect", "usb_backpressure", "read_recording", "header_torn", "erase_failure", "syntax", "early_boot", "codec"]
        for scenario in scenarios:
            result = subprocess.check_output([str(binary), scenario], text=True)
            rows = result.splitlines()
            status = json.loads(rows[-1])
            assert status["enabled"]
            if scenario == "packed":
                import zlib
                for line in rows:
                    if line.startswith("@DIAG FRAME "):
                        _, _, index, crc, payload = line.split()
                        data = bytes.fromhex(payload)
                        assert len(data) == 512 and zlib.crc32(data) == int(crc, 16)
                        assert zlib.crc32(data[:508]) == int.from_bytes(data[508:], "little")
                        assert data[:8] == (b"SSDHDR01" if index == "0" else b"SSDFRM01")
        source.write_text(DISABLED)
        subprocess.run(flags + [str(source), "-o", str(binary)], check=True)
        assert json.loads(subprocess.check_output([str(binary)], text=True))["enabled"] is False
    print(f"Diagnostic Flash worker: {len(scenarios)} scenarios plus disabled build passed")


if __name__ == "__main__":
    main()
