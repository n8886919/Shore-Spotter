#!/usr/bin/env python3
"""產生瀏覽器分頁圖示，輸出 include/web_icon.h。

    python3 tools/make_icon.py

這個檔案是圖示的**唯一來源**。圖示是用幾何繪製的，所以與其在 repo 裡放一份 SVG
再手動同步一份嵌入韌體的副本（兩份遲早會走岔），不如把產生器本身版控起來：
要改設計就改這裡的常數，重跑一次，include/web_icon.h 自動跟著更新。

設計說明
--------
畫的是這台機器本身，而不是一朵浪：

    半圓  = servo 0-180 度的涵蓋範圍（這台機器的實體限制）
    橘扇形 = 目前瞄準方向，和監控頁雷達畫面上那道光束同一個顏色
    紅點  = 被追蹤的衝浪者，和雷達畫面上的標記同一個顏色
    白點  = 岸上攝影站本身

也就是說，分頁圖示是監控頁「雷達」分頁的縮圖。

為什麼不是浪：浪在 16px 下會糊成「藍色團塊加一個紅點」，而且任何一個衝浪 app 都
長那樣 —— 它描述的是題材，不是這個產品。扇形描述的是「一台會自己瞄準你的岸上機器」，
那才是這個專案跟別人不一樣的地方。

16px 下的可讀性是設計時的硬條件，幾個關鍵取捨都是為了它：
  * 構圖撐滿圖磚 —— 第一版半圓只佔中間，16px 下四周的留白等於浪費像素。
  * 紅點加了一圈底色描邊 —— 紅色壓在橘色光束上，16px 下兩者會糊成一團。
  * 深色圓角底 —— 在 Chrome 的淺色分頁列上有明確邊界，在深色分頁列上像個 app 圖磚。

顏色全部取自 web_ui.h 的 :root 變數，所以圖示和監控頁是同一套視覺。
"""

import math
import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("需要 Pillow：pip install Pillow")

# --- 設計參數（要改設計就改這裡）------------------------------------------
GRID = 64          # 設計格線
SS = 16            # 超取樣倍率，先放大畫再縮小，邊緣才乾淨
STATION_Y = 50     # 攝影站在格線上的 y
RADIUS = 31        # 涵蓋半圓半徑
CORNER = 13        # 圓角半徑
BEAM_DEG = 62.0    # 瞄準方向（自水平面起算，向上為正）
BEAM_HALF = 20.0   # 光束半角
DOT_DIST = 32.0    # 衝浪者離攝影站的距離
DOT_R = 8.0        # 衝浪者圓點半徑
DOT_RING = 3.6     # 圓點外的底色描邊寬度（16px 下與橘色分離的關鍵）
STATION_R = 4.0    # 攝影站圓點半徑

BG = (13, 17, 23, 255)       # #0d1117  web_ui.h --bg
DISC = (35, 64, 110, 255)    #          --acc 壓暗後的涵蓋範圍
BEAM = (249, 115, 22, 255)   # #f97316  --trk 目前瞄準
DOT = (248, 81, 73, 255)     # #f85149  --bad 衝浪者標記
STATION = (230, 237, 243, 255)  # #e6edf3  --fg

# 分頁圖示 16/32；192 給 PWA「加到主畫面」與高解析度情境。
EXPORT_SIZES = (16, 32, 192)


def build():
    """在超取樣畫布上畫出圖示，回傳 RGBA Image。"""
    px = GRID * SS
    u = lambda v: v * SS
    im = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    d.rounded_rectangle([0, 0, px - 1, px - 1], radius=u(CORNER), fill=BG)

    cx, cy, r = u(GRID // 2), u(STATION_Y), u(RADIUS)
    box = [cx - r, cy - r, cx + r, cy + r]
    d.pieslice(box, 180, 360, fill=DISC)                       # 涵蓋半圓
    d.pieslice(box, -BEAM_DEG - BEAM_HALF,
               -BEAM_DEG + BEAM_HALF, fill=BEAM)               # 瞄準光束

    rad = math.radians(BEAM_DEG)
    dx = cx + math.cos(rad) * u(DOT_DIST)
    dy = cy - math.sin(rad) * u(DOT_DIST)
    dr, ring = u(DOT_R), u(DOT_RING)
    d.ellipse([dx - dr - ring, dy - dr - ring,
               dx + dr + ring, dy + dr + ring], fill=BG)       # 分離用描邊
    d.ellipse([dx - dr, dy - dr, dx + dr, dy + dr], fill=DOT)  # 衝浪者

    sr = u(STATION_R)
    d.ellipse([cx - sr, cy - sr, cx + sr, cy + sr], fill=STATION)
    return im


def png_bytes(im, size, tmpdir):
    """縮到指定尺寸並編成調色盤 PNG。

    整張圖只有 6 種顏色，所以調色盤模式比 RGBA 小 3-5 倍（192px：2.1 KB vs 10.7 KB）。
    韌體的 flash 還很寬裕，但這是白拿的。
    """
    small = im.resize((size, size), Image.LANCZOS)
    pal = small.quantize(colors=32, method=Image.FASTOCTREE)
    path = os.path.join(tmpdir, "_icon_%d.png" % size)
    pal.save(path, optimize=True)
    with open(path, "rb") as f:
        data = f.read()
    os.remove(path)
    return data


def c_array(name, data):
    lines = ["static const uint8_t %s[] = {" % name]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + " ".join("0x%02x," % b for b in chunk))
    lines.append("};")
    return "\n".join(lines)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "include", "web_icon.h")
    im = build()

    parts = ["""#pragma once
// ---------------------------------------------------------------------------
// 瀏覽器分頁圖示（PNG，已編成調色盤格式）。
//
// !! 這個檔案是產生出來的，不要手動編輯 !!
//     python3 tools/make_icon.py
//
// 設計說明與可調參數都在 tools/make_icon.py。圖示畫的是這台機器本身：
// 半圓 = servo 0-180 度涵蓋範圍、橘扇形 = 目前瞄準、紅點 = 衝浪者、白點 = 攝影站，
// 顏色與監控頁雷達畫面一致，所以分頁圖示等於雷達分頁的縮圖。
// ---------------------------------------------------------------------------
#include <stdint.h>
"""]
    total = 0
    for size in EXPORT_SIZES:
        data = png_bytes(im, size, root)
        total += len(data)
        parts.append("\n// %dx%d — %d bytes\n%s\n"
                     % (size, size, len(data),
                        c_array("ICON_%d_PNG" % size, data)))
        print("  %3dpx  %5d bytes" % (size, len(data)))

    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(parts))
    print("寫入 %s（圖示合計 %d bytes flash）" % (os.path.relpath(out, root), total))


if __name__ == "__main__":
    main()
