#!/usr/bin/env python3
"""Exercise the real SDK callback and LogTee, including concurrent SDK producers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ARDUINO = r'''
#pragma once
#include <cstddef>
#include <cstdint>
class Print {
 public:
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *p, size_t n) {
    for (size_t i=0;i<n;++i) write(p[i]);
    return n;
  }
};
'''
FREERTOS = r'''
#pragma once
#include <mutex>
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(p) (p)->lock()
#define portEXIT_CRITICAL(p) (p)->unlock()
'''
ESP_LOG = r'''
#pragma once
#include <cstdarg>
using vprintf_like_t = int (*)(const char *, va_list);
extern vprintf_like_t callback;
inline vprintf_like_t esp_log_set_vprintf(vprintf_like_t cb) {
  auto old=callback; callback=cb; return old;
}
'''
MAIN = r'''
#include <cassert>
#include <string>
#include <thread>
#include <vector>
#include "sdk_log.cpp"
vprintf_like_t callback=nullptr;
std::string console, history, sd;
struct Console { size_t write(const uint8_t *p,size_t n) {
  console.append(reinterpret_cast<const char *>(p),n);return n;
}} Serial;
bool transfer=false, diagTransfer=false;
uint32_t millis(){return 42;}
void logPush(uint8_t c){history+=char(c);}
namespace sd_log {
bool usbTransferActive(){return transfer;}
void text(const uint8_t *p,size_t n,uint32_t){sd.append(reinterpret_cast<const char *>(p),n);}
}
namespace diagnostic_store {bool transferActive(){return diagTransfer;}}
// LOG_TEE
int emit(const char *fmt,...) {
  va_list args;va_start(args,fmt);int n=callback(fmt,args);va_end(args);return n;
}
void clear(){console.clear();history.clear();sd.clear();}
int main(){
  sdk_log::begin(); assert(callback);
  emit("E (%d) wifi:sta is connecting, return error\n",164603);
  assert(console.empty() && history.empty());
  transfer=true; console="@SD BEGIN 4 /logs/test.ndjson\nDATA";
  sdk_log::drain(Log);
  assert(console=="@SD BEGIN 4 /logs/test.ndjson\nDATA");
  assert(history==sd && history.find("wifi:sta is connecting")!=std::string::npos);
  transfer=false; clear(); emit("SDK back %d\n",7); sdk_log::drain(Log);
  assert(console=="SDK back 7\n" && console==history && history==sd);
  clear(); diagTransfer=true;emit("during DIAG\n");sdk_log::drain(Log);
  assert(console.empty() && history=="during DIAG\n");diagTransfer=false;
  clear();for(int i=0;i<10;++i)emit("SDK %d\n",i);sdk_log::drain(Log);
  assert(history.find("SDK 7\n")!=std::string::npos);
  assert(history.find("SDK 8\n")==std::string::npos);
  assert(history.find("overflow: dropped=2")!=std::string::npos);
  clear();std::string longLine(1000,'x');assert(emit("%s",longLine.c_str())==1000);
  sdk_log::drain(Log);assert(history.size()==383);
  assert(history.find("[SDK log truncated]\n")!=std::string::npos);
  clear();std::vector<std::thread> threads;
  for(int i=0;i<4;++i)threads.emplace_back([i]{for(int n=0;n<100;++n)emit("producer %d item %d\n",i,n);});
  for(auto &t:threads)t.join();
  assert(console.empty());sdk_log::drain(Log);
  assert(history.find("overflow: dropped=392")!=std::string::npos);
  clear();emit("after overflow\n");sdk_log::drain(Log);
  assert(console=="after overflow\n");
}
'''

def main():
    source = (ROOT / 'src/main.cpp').read_text()
    tee = source[source.index('class LogTee :'):source.index('static LogTee Log;') + len('static LogTee Log;')]
    with tempfile.TemporaryDirectory(prefix='shore-sdk-log-') as directory:
        folder = Path(directory)
        (folder / 'freertos').mkdir()
        (folder / 'Arduino.h').write_text(ARDUINO)
        (folder / 'freertos/FreeRTOS.h').write_text(FREERTOS)
        (folder / 'esp_log.h').write_text(ESP_LOG)
        (folder / 'main.cpp').write_text(MAIN.replace('// LOG_TEE', tee))
        binary = folder / 'test'
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-DARDUINO', '-DESP32', '-DROLE_STATION', '-DFIELD_DIAGNOSTIC',
                        '-I' + str(folder), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'src'),
                        str(folder / 'main.cpp'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print('PASS SDK logs: SD/DIAG isolation, retained diagnostics, normal output, bounded overflow, truncation, concurrent producers')

if __name__ == '__main__':
    main()
