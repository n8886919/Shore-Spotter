#!/usr/bin/env python3
"""Render the actual U8g2 animation to GIF/contact sheet without any device IO.

Uses the locally installed U8g2 C rasterizer and the production header, so pixel
placement and fonts match the 128x64 buffer. Bounds are checked at every ms.
Requires gcc/g++, Pillow and a populated .pio/libdeps/*/U8g2 directory.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
HOST = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "u8g2.h"
#include "client_boot_animation.h"
struct Display {
  u8g2_t u{}; uint8_t pixels[1024]{}; u8x8_display_info_t info{};
  Display() {
    info.tile_width=16;info.tile_height=8;info.pixel_width=128;info.pixel_height=64;
    u.u8x8.display_info=&info;
    u8g2_SetupBuffer(&u,pixels,8,u8g2_ll_hvline_vertical_top_lsb,U8G2_R0);
  }
  void point(int x,int y){assert(x>=0&&x<128&&y>=0&&y<64);}
  void box(int x,int y,int w,int h){assert(w>0&&h>0);point(x,y);point(x+w-1,y+h-1);}
  void clearBuffer(){u8g2_ClearBuffer(&u);}
  void setFont(const uint8_t*f){u8g2_SetFont(&u,f);}
  void setDrawColor(uint8_t c){u8g2_SetDrawColor(&u,c);}
  int getStrWidth(const char*s){return u8g2_GetStrWidth(&u,s);}
  void drawStr(int x,int y,const char*s){point(x,y);assert(x+getStrWidth(s)<=128);u8g2_DrawStr(&u,x,y,s);}
  void drawPixel(int x,int y){point(x,y);u8g2_DrawPixel(&u,x,y);}
  void drawLine(int a,int b,int c,int d){point(a,b);point(c,d);u8g2_DrawLine(&u,a,b,c,d);}
  void drawHLine(int x,int y,int n){box(x,y,n,1);u8g2_DrawHLine(&u,x,y,n);}
  void drawVLine(int x,int y,int n){box(x,y,1,n);u8g2_DrawVLine(&u,x,y,n);}
  void drawBox(int x,int y,int w,int h){box(x,y,w,h);u8g2_DrawBox(&u,x,y,w,h);}
  void drawCircle(int x,int y,int r){box(x-r,y-r,2*r+1,2*r+1);u8g2_DrawCircle(&u,x,y,r,U8G2_DRAW_ALL);}
  void drawDisc(int x,int y,int r){box(x-r,y-r,2*r+1,2*r+1);u8g2_DrawDisc(&u,x,y,r,U8G2_DRAW_ALL);}
};
int main() {
  Display d;
  for(uint32_t t=0;t<=3000;++t){
    client_boot_animation::draw(d,t);
    if(t<3000&&t%100==0)fwrite(d.pixels,1,sizeof(d.pixels),stdout);
  }
  uint8_t last[1024];memcpy(last,d.pixels,sizeof(last));
  client_boot_animation::draw(d,UINT32_MAX);
  assert(memcmp(last,d.pixels,sizeof(last))==0);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    libs = sorted((ROOT / ".pio/libdeps").glob("*/U8g2/src/clib"))
    if not libs:
        parser.error("Build a firmware environment first to install U8g2")
    lib = libs[0]
    sources = ["u8g2_setup", "u8g2_buffer", "u8g2_ll_hvline", "u8g2_hvline",
               "u8g2_line", "u8g2_circle", "u8g2_box", "u8g2_font", "u8g2_fonts",
               "u8g2_intersection", "u8x8_8x8"]
    with tempfile.TemporaryDirectory(prefix="shore-boot-preview-") as temp:
        tmp = Path(temp)
        (tmp / "host.cpp").write_text(HOST)
        objects = []
        for name in sources:
            obj = tmp / f"{name}.o"
            subprocess.run(["gcc", "-O1", "-ffunction-sections", "-fdata-sections",
                            "-I", str(lib), "-c", str(lib / f"{name}.c"), "-o", str(obj)], check=True)
            objects.append(str(obj))
        exe = tmp / "preview"
        subprocess.run(["g++", "-std=c++11", "-O1", "-Wall", "-Wextra", "-Werror",
                        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-I", str(lib), "-I", str(ROOT / "include"), str(tmp / "host.cpp"),
                        *objects, "-o", str(exe)], check=True)
        raw = subprocess.run([str(exe)], check=True, capture_output=True).stdout
    assert len(raw) == 30 * 1024
    frames = []
    for frame in range(30):
        data = raw[frame * 1024:(frame + 1) * 1024]
        image = Image.new("RGB", (128, 64), (4, 12, 16))
        image.putdata([(218, 255, 251) if data[(y // 8) * 128 + x] & (1 << (y % 8))
                       else (4, 12, 16) for y in range(64) for x in range(128)])
        frames.append(image.resize((768, 384), Image.Resampling.NEAREST))
    args.output.mkdir(parents=True, exist_ok=True)
    gif = args.output / "client-boot-animation.gif"
    frames[0].save(gif, save_all=True, append_images=frames[1:], duration=100, loop=0, optimize=False)
    sheet = Image.new("RGB", (768 * 3, 424 * 2), (16, 24, 29))
    pen = ImageDraw.Draw(sheet)
    for index, frame in enumerate([0, 6, 12, 17, 22, 29]):
        x, y = (index % 3) * 768, (index // 3) * 424
        sheet.paste(frames[frame], (x, y))
        pen.text((x + 16, y + 396), f"{frame / 10:.1f} s", fill="white")
    sheet.save(args.output / "client-boot-animation-sheet.png")
    print(f"PASS: every millisecond in 0..3000 and UINT32_MAX; 30 actual U8g2 frames: {gif}")


if __name__ == "__main__":
    main()
