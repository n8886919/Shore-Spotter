#!/usr/bin/env python3
"""Run both actual USB workers and router with a scheduled other-core arrival.

Inject the next request from Serial.write as soon as the terminal newline is
visible, before the original worker can clear its active flag. No device I/O.
"""
from pathlib import Path
import subprocess
import tempfile

from test_sd_backend import STUB as SD_STUB
from test_diagnostic_store import STUB as DIAG_STUB

ROOT = Path(__file__).resolve().parents[1]
STUB = SD_STUB.replace(
    'struct Console {std::string input,output;',
    'std::function<void()> onUsbWrite;\nstruct Console {std::string input,output;'
    'int availableForWrite(){return 128;}',
).replace('output.append((const char*)p,n);return n;',
          'output.append((const char*)p,n);if(onUsbWrite)onUsbWrite();return n;')
STUB = STUB.replace('stack>=16384', 'stack>=6144')
# Reuse the established SD/media and NOR stubs, with one shared USB/clock/queue.
NOR = DIAG_STUB[DIAG_STUB.index('constexpr int ESP_OK='):]
NOR = NOR.replace('constexpr int ESP_OK=0,ESP_FAIL=-1,', 'constexpr int ESP_FAIL=-1,')
NOR = NOR.replace('partitionAvailable=true,readOk=true,writeOk=true,eraseOk=true',
                  'partitionAvailable=true,eraseOk=true')
STUB += '\n' + NOR

MAIN = r'''
#include "sd_encode.cpp"
#include "axiom_encode.cpp"
#include "sd_log.cpp"
#include "diagnostic_store.cpp"
bool rail=false;
bool power(bool on){rail=on;return true;}
void pump(){
  for(unsigned i=0;i<4;++i){
    const auto before=Serial.input.size();sd_log::serviceUsb();
    if(Serial.input.size()==before)break;
  }
}
void run(bool diag,unsigned ms){
  const uint32_t until=clockMs+ms;
  onDelay=[=](){pump();if(clockMs>=until)throw Done{};};
  try{if(diag)diagnostic_store::worker(nullptr);else sd_log::worker(nullptr);}catch(Done&){}
  onDelay=nullptr;
}
size_t occurrences(const std::string&haystack,const std::string&needle){
  size_t count=0,at=0;while((at=haystack.find(needle,at))!=std::string::npos){++count;at+=needle.size();}return count;
}
int main(int argc,char**argv){
  assert(argc==3);const std::string previous=argv[1], next=argv[2];
  const char *path="/sd/logs/0000000000000009-0000.ndjson";
  const int input=fakeOpen(path,O_CREAT|O_EXCL|O_RDWR);
  data[input]="{\"kind\":\"saved_log\"}\n";
  sd_log::begin(560532669,true,power);diagnostic_store::begin(560532669);run(true,1000);
  Serial.output.clear();
  std::string initial,terminal;
  if(previous=="sd_list"){initial="SD LIST";terminal="@SD LIST END\n";}
  else if(previous=="sd_read"){
    initial="SD READ /logs/0000000000000009-0000.ndjson";
    char line[64];snprintf(line,sizeof(line),"@SD END %08lx\n",(unsigned long)diagnostic_store::codec::crc32(reinterpret_cast<const uint8_t*>(data[input].data()),data[input].size()));terminal=line;
  }else if(previous=="sd_status"){initial="SD STATUS";terminal="@SD STATUS ";}
  else if(previous=="diag_read"){initial="DIAG READ 0 1";terminal="@DIAG END 0 1\n";}
  else {assert(previous=="diag_status");initial="DIAG STATUS";terminal="@DIAG STATUS ";}
  const std::string follow=next=="sd_read"?"SD READ /logs/0000000000000009-0000.ndjson":next=="sd"?"SD STATUS":"DIAG STATUS";
  bool fired=false;size_t terminalEnd=0;
  onUsbWrite=[&](){
    if(fired||Serial.output.empty()||Serial.output.back()!='\n'||Serial.output.find(terminal)==std::string::npos)return;
    fired=true;terminalEnd=Serial.output.size();
    assert(previous.compare(0,3,"sd_")==0?sd_log::usbTransferActive():diagnostic_store::transferActive());
    Serial.input=follow+"\n";pump();
    assert(Serial.input.empty());
    assert(sd_log::usbPending.load()&&sd_log::usbDeferred.load()==1);
    assert(uxQueueMessagesWaiting(sd_log::commands)==0&&uxQueueMessagesWaiting(diagnostic_store::commands)==0);
    assert(Serial.output.size()==terminalEnd); // No nested response or busy text.
  };
  Serial.input=initial+"\n";pump();
  for(unsigned i=0;i<3;++i){run(false,1000);run(true,1000);pump();}
  onUsbWrite=nullptr;
  assert(fired&&!sd_log::usbTransferActive()&&!diagnostic_store::transferActive());
  assert(!sd_log::usbPending.load()&&sd_log::usbQueueErrors.load()==0);
  assert(sd_log::shared.errors==0&&diagnostic_store::shared.usbErrors==0&&diagnostic_store::shared.commandDrops==0);
  assert(!strcmp(sd_log::stateName(),"stopped"));
  const auto status=sd_log::statusJson();
  assert(status.find("\"usb_pending\":false")!=std::string::npos&&status.find("\"usb_deferred\":1")!=std::string::npos&&status.find("\"usb_queue_errors\":0")!=std::string::npos);
  assert(formats==0&&erases==0&&sd_log::bootId==560532669&&diagnostic_store::bootId==560532669);
  const std::string reply=Serial.output.substr(terminalEnd);
  if(next=="sd_read"){
    assert(reply.find("@SD BEGIN ")==0&&reply.find(data[input])!=std::string::npos&&reply.find("@SD END ")!=std::string::npos);
  }else if(next=="sd")assert(reply.find("@SD STATUS ")==0&&occurrences(reply,"@SD STATUS ")==1);
  else assert(reply.find("\n@DIAG STATUS ")==0&&occurrences(reply,"@DIAG STATUS ")==1);
  if(previous=="diag_read"){
    assert(occurrences(Serial.output,"@DIAG FRAME ")==1&&occurrences(Serial.output,"@DIAG END ")==1);
  }
  std::cout<<"PASS immediate "<<previous<<" -> "<<next<<": retained at terminal newline, delivered once after worker release, no interleaving\n";
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='shore-usb-completion-') as tmp:
        directory = Path(tmp)
        (directory / 'stub.h').write_text(STUB)
        for name in ['Arduino.h', 'SD.h', 'SPI.h', 'freertos/FreeRTOS.h',
                     'freertos/queue.h', 'freertos/task.h', 'sd_diskio.h',
                     'ff.h', 'esp_vfs_fat.h', 'esp_partition.h']:
            destination = directory / name
            destination.parent.mkdir(exist_ok=True, parents=True)
            destination.write_text('#include "stub.h"\n')
        (directory / 'main.cpp').write_text(MAIN)
        binary = directory / 'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-DARDUINO', '-DROLE_CLIENT', '-DFIELD_DIAGNOSTIC',
                        '-I' + str(directory), '-I' + str(ROOT / 'include'),
                        '-I' + str(ROOT / 'src'), str(directory / 'main.cpp'),
                        '-o', str(binary)], check=True)
        for previous in ['sd_list', 'sd_read', 'sd_status', 'diag_read', 'diag_status']:
            for following in ['sd', 'diag']:
                subprocess.run([str(binary), previous, following], check=True)
        subprocess.run([str(binary), 'sd_list', 'sd_read'], check=True)


if __name__ == '__main__':
    main()
