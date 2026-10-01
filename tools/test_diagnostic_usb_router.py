#!/usr/bin/env python3
"""Compile the real shared USB parser and LogTee against bounded host fakes.

Checks reservation before worker dequeue/response, diagnostic/SD exclusion and
partial input recovery. No serial port, filesystem worker or SDK is involved.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SD = (ROOT / "src/sd_log.cpp").read_text()
MAIN = (ROOT / "src/main.cpp").read_text()


def block(source, marker):
    start = source.index(marker)
    body = source.index("{", start)
    depth = 0
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    for match in re.finditer(tokens, source[body:]):
        if match.group() == "{":
            depth += 1
        elif match.group() == "}":
            depth -= 1
            if not depth:
                return source[start:body + match.end()]
    raise ValueError(marker)


PREAMBLE = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <vector>
uint32_t clockMs=0;
uint32_t millis(){return clockMs;}
struct Console {
  std::string input,output;
  int available(){return input.size();}
  int read(){assert(!input.empty());const unsigned char c=input.front();input.erase(0,1);return c;}
  size_t write(const uint8_t*p,size_t n){output.append(reinterpret_cast<const char*>(p),n);return n;}
} Serial;
constexpr int pdTRUE=1;
struct Queue {size_t size=0,capacity=4;std::deque<std::vector<uint8_t>> values;bool fail=false;} queue;
using QueueHandle_t=Queue*;
size_t uxQueueMessagesWaiting(QueueHandle_t q){return q->values.size();}
int xQueueSend(QueueHandle_t q,const void*value,int wait){
  assert(wait==0);if(q->fail||q->values.size()==q->capacity)return 0;
  const auto*bytes=static_cast<const uint8_t*>(value);q->values.emplace_back(bytes,bytes+q->size);return pdTRUE;
}
namespace diagnostic_store {
bool active=false;unsigned calls=0,accepted=0;
std::vector<std::string> lines;
bool transferActive(){return active;}
bool command(const char*line){++calls;if(active)return true;active=true;++accepted;lines.emplace_back(line);return true;}
}
namespace sd_log {
std::atomic<bool> usbActive{false}, usbPending{false};
std::atomic<uint32_t> usbDeferred{0}, usbQueueErrors{0};
QueueHandle_t commands=&queue;
unsigned textCalls=0;
void text(const uint8_t*,size_t,uint32_t){++textCalls;}
'''

POSTAMBLE = r'''
}
class Print {
 public:
  virtual ~Print()=default;
  virtual size_t write(uint8_t)=0;
  virtual size_t write(const uint8_t*,size_t)=0;
};
unsigned logCharacters=0;
void logPush(uint8_t){++logCharacters;}
'''

TEST = r'''
void pump(){for(unsigned i=0;i<100;++i){const auto before=Serial.input.size();sd_log::serviceUsb();if(Serial.input.size()==before)break;}}
void feed(const std::string&bytes){Serial.input+=bytes;pump();}
void finishSd(){assert(sd_log::usbActive.load());queue.values.clear();sd_log::usbActive.store(false);}
void finishDiag(){assert(diagnostic_store::active);diagnostic_store::active=false;}
sd_log::UsbCommand queued(){assert(queue.values.size()==1);sd_log::UsbCommand c;memcpy(&c,queue.values.front().data(),sizeof(c));return c;}
void reset(){
  // Complete/drain prior replies, including one router-held full command.
  Serial.input="\n";queue.fail=false;
  for(unsigned i=0;i<8;++i){queue.values.clear();sd_log::usbActive.store(false);diagnostic_store::active=false;pump();}
  queue.values.clear();sd_log::usbActive.store(false);diagnostic_store::active=false;
  diagnostic_store::calls=diagnostic_store::accepted=0;diagnostic_store::lines.clear();
  Serial.input.clear();Serial.output.clear();sd_log::textCalls=0;logCharacters=0;
  sd_log::usbDeferred.store(0);sd_log::usbQueueErrors.store(0);assert(!sd_log::usbPending.load());
}
void test_exclusion_and_pending_dequeue(){
  reset();feed("SD STATUS\nDIAG STATUS\nSD LIST\n");
  assert(queued().op==sd_log::UsbOp::Status&&sd_log::usbTransferActive());
  assert(diagnostic_store::calls==0&&!diagnostic_store::active);
  assert(sd_log::usbPending.load()&&sd_log::usbDeferred.load()==1&&Serial.input=="SD LIST\n");
  // Real SD worker dequeues before stopping/flushing the card. A pending
  // request must remain retained even if the worker takes more than 1 second.
  queue.values.clear();clockMs+=2000;pump();
  assert(queue.values.empty()&&diagnostic_store::calls==0&&sd_log::usbTransferActive());
  assert(sd_log::usbPending.load()&&sd_log::usbDeferred.load()==1&&Serial.input=="SD LIST\n");
  LogTee log;const uint8_t line[]="working\n";log.write(line,sizeof(line)-1);
  assert(Serial.output.empty()&&sd_log::textCalls==1&&logCharacters==sizeof(line)-1);
  finishSd();pump();
  assert(diagnostic_store::active&&diagnostic_store::accepted==1&&queue.values.empty());
  assert(diagnostic_store::lines.back()=="DIAG STATUS");
  pump();assert(sd_log::usbPending.load()&&sd_log::usbDeferred.load()==2&&Serial.input.empty());
  log.write(line,sizeof(line)-1);assert(Serial.output.empty()&&sd_log::textCalls==2);
  finishDiag();pump();assert(queued().op==sd_log::UsbOp::List&&!sd_log::usbPending.load());
  finishSd();log.write(line,sizeof(line)-1);assert(Serial.output=="working\n"&&sd_log::textCalls==3);
}
void test_diag_first_and_queue_fallback(){
  reset();feed("DIAG STATUS\nSD STATUS\nDIAG READ 0\n");
  assert(diagnostic_store::active&&diagnostic_store::accepted==1&&queue.values.empty()&&!sd_log::usbTransferActive());
  assert(sd_log::usbPending.load()&&Serial.input=="DIAG READ 0\n");
  finishDiag();pump();assert(queued().op==sd_log::UsbOp::Status);
  pump(); // Third command stays pending behind the queued SD command.
  sd_log::usbActive.store(false);pump();assert(diagnostic_store::accepted==1&&!diagnostic_store::active);
  queue.values.clear();pump();assert(diagnostic_store::accepted==2&&diagnostic_store::active);finishDiag();
  queue.fail=true;feed("SD STATUS\n");assert(!sd_log::usbTransferActive()&&queue.values.empty());
  assert(sd_log::usbPending.load()&&sd_log::usbQueueErrors.load()==1);
  for(unsigned i=0;i<100;++i)pump();
  assert(sd_log::usbQueueErrors.load()==1&&sd_log::usbPending.load());
  queue.fail=false;pump();assert(queued().op==sd_log::UsbOp::Status&&!sd_log::usbPending.load());finishSd();
}
void test_partial_lines_and_timeout(){
  reset();feed("DIAG ST");assert(!diagnostic_store::calls&&queue.values.empty());
  clockMs+=999;feed("ATUS\r\n");assert(diagnostic_store::active&&diagnostic_store::lines.back()=="DIAG STATUS");finishDiag();
  feed("SD RE");clockMs+=1001;sd_log::serviceUsb();feed("AD LAST\n");
  assert(queued().op==sd_log::UsbOp::Invalid);finishSd();
  feed("SD READ LAST\r\n");assert(queued().op==sd_log::UsbOp::Read);finishSd();
  clockMs=0xfffffff0U;feed("DIAG ");clockMs+=1001;sd_log::serviceUsb();feed("STATUS\n");
  assert(queued().op==sd_log::UsbOp::Invalid&&!diagnostic_store::active);finishSd();
}
void test_overflow_and_bounded_reads(){
  reset();Serial.input="DIAG "+std::string(180,'x')+"\n";
  const size_t total=Serial.input.size();sd_log::serviceUsb();
  assert(Serial.input.size()==total-64&&queue.values.empty()&&diagnostic_store::calls==0);
  pump();assert(queued().op==sd_log::UsbOp::Invalid&&diagnostic_store::calls==0);finishSd();
  feed("DIAG STATUS\n");assert(diagnostic_store::active&&diagnostic_store::accepted==1);finishDiag();
  feed("SD READ /logs/../secret.ndjson\n");assert(queued().op==sd_log::UsbOp::Invalid);finishSd();
  feed("SD READ /logs/0000000000000001-0000.ndjson\n");assert(queued().op==sd_log::UsbOp::Read);finishSd();
}
int main(){
  queue.size=sizeof(sd_log::UsbCommand);
  test_exclusion_and_pending_dequeue();test_diag_first_and_queue_fallback();
  test_partial_lines_and_timeout();test_overflow_and_bounded_reads();
  std::cout<<"PASS real USB router and LogTee: bounded retained request, SD/DIAG exclusion, >1s cleanup, backpressure/counters, queue failure retry, partial/overflow input, 64-byte budget and millis wrap\n";
}
'''


def main():
    source = PREAMBLE
    for marker in ["enum class UsbOp", "struct UsbCommand"]:
        source += block(SD, marker) + ";\n"
    for marker in ["bool validPath(", "bool usbTransferActive(", "void serviceUsb("]:
        source += block(SD, marker) + "\n"
    source += POSTAMBLE + block(MAIN, "class LogTee : public Print") + ";\n" + TEST
    with tempfile.TemporaryDirectory(prefix="shore-diag-router-") as tmp:
        directory = Path(tmp)
        (directory / "test.cpp").write_text(source)
        binary = directory / "test"
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DFIELD_DIAGNOSTIC", "-DROLE_STATION",
                        str(directory / "test.cpp"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
