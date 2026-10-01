#!/usr/bin/env python3
"""Exercise actual SD worker under blocked/trickling USB plus invalid RX evidence.

Uses fake media/clock/USB only. Recording must keep draining raw text while a
host stops reading a response; offline exports keep the longer progress timeout.
"""
from pathlib import Path
import subprocess
import tempfile

from test_sd_backend import STUB

ROOT = Path(__file__).resolve().parents[1]
STUB = STUB.replace('struct Console {', 'size_t maxUsbWrite=37; std::function<void()> onUsbAttempt;\nstruct Console {')
STUB = STUB.replace('n=std::min(n,size_t(37));output.append',
                    'if(onUsbAttempt)onUsbAttempt();n=std::min(n,maxUsbWrite);output.append')

MAIN = r'''
#include "sd_encode.cpp"
#include "axiom_encode.cpp"
#include "sd_log.cpp"
void run(unsigned ms){
  const uint32_t until=clockMs+ms;
  onDelay=[=](){if(clockMs>=until)throw Done{};};
  try{sd_log::worker(nullptr);}catch(Done&){}onDelay=nullptr;
}
void feed(const std::string&line){
  Serial.input=line+"\n";
  for(unsigned i=0;!Serial.input.empty()&&i<100;++i)sd_log::serviceUsb();
  assert(Serial.input.empty());
}
void invalid(const std::string&line,const std::string&evidence){
  Serial.output.clear();feed(line);run(500);
  assert(!sd_log::usbTransferActive());
  assert(Serial.output.find("@SD ERROR expected_STATUS_START_STOP_LIST_or_READ_path ")==0);
  assert(Serial.output.find(evidence)!=std::string::npos&&Serial.output.back()=='\n');
  assert(Serial.output.size()<200);
}
int main(int argc,char**argv){
  assert(argc==2);const std::string scenario=argv[1];
  sd_log::begin(1234,true);run(200);assert(sd_log::fd>=0);
  if(scenario=="invalid"){
    invalid("SD WAT\r","rx_len=7 overflow=0 rx_hex=5344205741540d");
    invalid(std::string("\xc0\0SD STATUS\r",12),"rx_len=12 overflow=0 rx_hex=c0005344205354415455530d");
    invalid("SD STATUS"+std::string(1,'\0')+"garbage","rx_len=17 overflow=0 rx_hex=5344205354415455530067617262616765");
    invalid(std::string(180,'x'),"rx_len=180 overflow=1 rx_hex=7878787878787878787878787878787878787878787878787878787878787878");
    assert(sd_log::usbWriteErrors.load()==0);
  }else if(scenario=="offline"){
    sd_log::stop();run(500);assert(sd_log::fd<0);
    const char message[100]={};maxUsbWrite=1;
    uint32_t started=clockMs;assert(sd_log::usbWrite(message,sizeof(message)));assert(clockMs-started==100);
    maxUsbWrite=0;started=clockMs;assert(!sd_log::usbWrite(message,sizeof(message)));
    assert(clockMs-started==3000&&sd_log::usbWriteErrors.load()==1);
  }else{
    assert(scenario=="blocked"||scenario=="trickle");
    Serial.output.clear();maxUsbWrite=scenario=="blocked"?0:1;
    uint32_t firstAttempt=UINT32_MAX,lastAttempt=0,released=UINT32_MAX;
    unsigned attempts=0,produced=0;
    onUsbAttempt=[&](){if(firstAttempt==UINT32_MAX)firstAttempt=clockMs;lastAttempt=clockMs;++attempts;};
    const uint32_t start=clockMs,until=start+2000;uint32_t nextText=start;
    feed("SD STATUS");
    onDelay=[&](){
      while(clockMs>=nextText&&nextText<until){
        char raw[40];const int n=snprintf(raw,sizeof(raw),"$RAW,seq=%u\r\n",produced++);
        sd_log::text(reinterpret_cast<const uint8_t*>(raw),n,nextText);nextText+=20;
      }
      if(firstAttempt!=UINT32_MAX&&!sd_log::usbTransferActive()&&released==UINT32_MAX)released=clockMs;
      if(clockMs>=until)throw Done{};
    };
    try{sd_log::worker(nullptr);}catch(Done&){}onDelay=nullptr;onUsbAttempt=nullptr;
    assert(attempts==50&&lastAttempt-firstAttempt==49);
    assert(released!=UINT32_MAX&&released-firstAttempt<=100);
    assert(sd_log::usbWriteErrors.load()==1&&!sd_log::usbTransferActive());
    assert(Serial.output.find('\n')==std::string::npos); // An aborted response cannot look complete.
    run(1500);
    assert(produced==100&&sd_log::shared.texts==produced&&sd_log::shared.textDrops==0);
    assert(uxQueueMessagesWaiting(sd_log::texts)==0&&sd_log::shared.errors==0&&sd_log::shared.bytes>0);
    assert(sd_log::statusJson().find("\"usb_write_errors\":1")!=std::string::npos);
    maxUsbWrite=37;Serial.output.clear();feed("SD STATUS");run(500);
    assert(Serial.output.find("\"usb_write_errors\":1")!=std::string::npos&&Serial.output.back()=='\n');
    assert(sd_log::shared.textDrops==0);
  }
  assert(formats==0);
  std::cout<<"PASS SD USB "<<scenario<<"\n";
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='shore-sd-usb-budget-') as tmp:
        directory = Path(tmp)
        (directory / 'stub.h').write_text(STUB)
        for name in ['Arduino.h', 'SD.h', 'SPI.h', 'freertos/FreeRTOS.h',
                     'freertos/queue.h', 'freertos/task.h', 'sd_diskio.h',
                     'ff.h', 'esp_vfs_fat.h']:
            target = directory / name
            target.parent.mkdir(exist_ok=True, parents=True)
            target.write_text('#include "stub.h"\n')
        (directory / 'main.cpp').write_text(MAIN)
        binary = directory / 'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DARDUINO',
                        '-I' + str(directory), '-I' + str(ROOT / 'include'),
                        '-I' + str(ROOT / 'src'), str(directory / 'main.cpp'),
                        '-o', str(binary)], check=True)
        for scenario in ['blocked', 'trickle', 'offline', 'invalid']:
            subprocess.run([str(binary), scenario], check=True)


if __name__ == '__main__':
    main()
