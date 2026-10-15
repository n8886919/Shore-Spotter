#!/usr/bin/env python3
"""Capture actual Adafruit LCD commands through the T096 panel wrapper.

Requires the project's installed ST7735 dependency. No board is accessed;
command geometry is verified here, physical rendering is a separate check.
"""
from pathlib import Path
import re
import subprocess
import tempfile

from test_t096_gnss_uart import extract_function

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / '.pio/libdeps/t096-client/Adafruit ST7735 and ST7789 Library'
st7735 = (LIB / 'Adafruit_ST7735.cpp').read_text()
st77xx = (LIB / 'Adafruit_ST77xx.cpp').read_text()
defines = '\n'.join(
    line for name in ('Adafruit_ST7735.h', 'Adafruit_ST77xx.h')
    for line in (LIB / name).read_text().splitlines()
    if re.match(r'#define (INITR_|ST7735_|ST77XX_|ST_CMD_DELAY)', line)
)
actual = '\n'.join((
    extract_function(st7735, 'void Adafruit_ST7735::initR(uint8_t options)'),
    extract_function(st7735, 'void Adafruit_ST7735::setRotation(uint8_t m)'),
    extract_function(st77xx, re.search(r'void Adafruit_ST77xx::setAddrWindow\([^)]*\)', st77xx).group(0))
        .replace('Adafruit_ST77xx::', 'Adafruit_ST7735::'),
    extract_function(st77xx, re.search(r'void Adafruit_ST77xx::setColRowStart\([^)]*\)', st77xx).group(0))
        .replace('Adafruit_ST77xx::', 'Adafruit_ST7735::'),
))
stub = r'''
#pragma once
#include <cstdint>
#include <vector>
'''+defines+r'''
static const uint8_t Rcmd1[]={0},Rcmd2green[]={0},Rcmd2green144[]={0},
  Rcmd2green160x80[]={0},Rcmd2green160x80plugin[]={0},Rcmd2red[]={0},Rcmd3[]={0};
class Adafruit_ST7735 {
 public:
  Adafruit_ST7735(int,int,int){}
  void initR(uint8_t options);
  void setRotation(uint8_t m);
  void setAddrWindow(uint16_t x,uint16_t y,uint16_t w,uint16_t h);
  uint16_t width()const{return _width;}uint16_t height()const{return _height;}
  std::vector<uint8_t> commands;std::vector<uint32_t> addresses;
 protected:
  uint16_t _width=128,_height=160;
  uint8_t _colstart=0,_rowstart=0,_xstart=0,_ystart=0,rotation=0,tabcolor=0;
  uint8_t invertOnCommand=ST77XX_INVON,invertOffCommand=ST77XX_INVOFF;
  void setColRowStart(int8_t col,int8_t row);
  void commonInit(const uint8_t*){}void displayInit(const uint8_t*){}
  void sendCommand(uint8_t cmd,uint8_t*,uint8_t){commands.push_back(cmd);}
  void writeCommand(uint8_t cmd){commands.push_back(cmd);}
  void SPI_WRITE32(uint32_t word){addresses.push_back(word);}
};
'''+actual
cpp = r'''
#include <cassert>
#include <cstdio>
#include "t096_display.h"
int main(){
  Adafruit_ST7735 generic(22,15,13);
  generic.initR(INITR_MINI160x80_PLUGIN);generic.setRotation(1);
  generic.setAddrWindow(0,0,160,16);
  assert(generic.addresses[0]==0x000100a0u); // Adafruit x=1..160
  assert(generic.addresses[1]==0x001a0029u); // Adafruit y=26..41
  T096Display panel(22,15,13);panel.initPanel();
  assert(panel.width()==160&&panel.height()==80);
  for(unsigned row=0;row<5;++row){
    panel.commands.clear();panel.addresses.clear();
    panel.setAddrWindow(0,row*16,160,16);
    assert((panel.commands==std::vector<uint8_t>{ST77XX_CASET,ST77XX_RASET,ST77XX_RAMWR}));
    assert(panel.addresses[0]==0x0000009fu); // Heltec x=0..159
    const uint32_t y=24+row*16;
    assert(panel.addresses[1]==((y<<16)|(y+15)));
  }
  panel.initPanel();panel.addresses.clear();
  panel.setAddrWindow(0,0,160,80);
  assert(panel.addresses[0]==0x0000009fu&&panel.addresses[1]==0x00180067u);
  std::puts("PASS real ST7735 init/rotation/CASET/RASET: Heltec 160x80, x=0..159, y=24..103; generic-offset mismatch reproduced");
}
'''
source = (ROOT / 'src/t096_client.cpp').read_text()
assert 'T096Display tft(' in source
init = extract_function(source, 'void initClientDisplay()')
assert 'tft.initPanel();' in init and 'tft.setRotation(' not in init
with tempfile.TemporaryDirectory(prefix='t096-lcd-window-') as directory:
    p = Path(directory)
    (p / 'Adafruit_ST7735.h').write_text(stub)
    (p / 'test.cpp').write_text(cpp)
    subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                    '-I'+str(p),'-I'+str(ROOT/'include'),str(p/'test.cpp'),
                    '-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
