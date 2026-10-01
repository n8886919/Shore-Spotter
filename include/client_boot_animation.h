#pragma once

#include <stdint.h>

// Include after U8g2lib.h (or u8g2.h for a host preview). This renderer only
// updates the 128x64 buffer; the caller owns timing, sendBuffer and services.
// The radar and wave are a logo animation, never an indication of a GPS fix.
namespace client_boot_animation {

constexpr uint32_t kDurationMs = 3000;
constexpr uint32_t kFrameMs = 100;

template <typename Display>
void corners(Display& d, int x, int y, int width, int height, int length) {
  d.drawHLine(x, y, length);
  d.drawVLine(x, y, length);
  d.drawHLine(x + width - length, y, length);
  d.drawVLine(x + width - 1, y, length);
  d.drawHLine(x, y + height - 1, length);
  d.drawVLine(x, y + height - length, length);
  d.drawHLine(x + width - length, y + height - 1, length);
  d.drawVLine(x + width - 1, y + height - length, length);
}

template <typename Display>
void centered(Display& d, int baseline, const char* text) {
  d.drawStr((128 - d.getStrWidth(text)) / 2, baseline, text);
}

// An outlined breaking wave; all coordinates are fixed and within the panel.
// Open contours retain the crisp one-pixel style at the OLED's native size.
template <typename Display>
void wave(Display& d, int centerX, int top, uint8_t reveal) {
  static constexpr int8_t outline[][2] = {
    {-24, 16}, {-18, 16}, {-12, 13}, {-7, 7}, {-2, 2}, {4, 0},
    {10, 1}, {14, 4}, {15, 8}, {12, 11}, {8, 10}, {6, 7},
    {3, 8}, {3, 12}, {7, 16}, {15, 18}, {24, 18}
  };
  const uint8_t segments = sizeof(outline) / sizeof(outline[0]) - 1;
  if (reveal > segments) reveal = segments;
  for (uint8_t i = 0; i < reveal; ++i) {
    d.drawLine(centerX + outline[i][0], top + outline[i][1],
               centerX + outline[i + 1][0], top + outline[i + 1][1]);
  }
  if (reveal >= 13) {
    d.drawLine(centerX - 20, top + 20, centerX - 11, top + 20);
    d.drawLine(centerX - 11, top + 20, centerX - 4, top + 17);
    d.drawLine(centerX - 4, top + 17, centerX + 3, top + 20);
    d.drawHLine(centerX + 3, top + 20, 18);
  }
}

template <typename Display>
void draw(Display& d, uint32_t elapsedMs) {
  const uint32_t t = elapsedMs < kDurationMs ? elapsedMs : kDurationMs;
  d.clearBuffer();
  d.setDrawColor(1);
  d.setFont(u8g2_font_5x7_tr);

  if (t < 1200) {
    // Sixteen integer unit vectors make one clean sweep in 1.2 seconds.
    static constexpr int8_t vectors[16][2] = {
      {0, -22}, {8, -20}, {16, -16}, {20, -8}, {22, 0}, {20, 8},
      {16, 16}, {8, 20}, {0, 22}, {-8, 20}, {-16, 16}, {-20, 8},
      {-22, 0}, {-20, -8}, {-16, -16}, {-8, -20}
    };
    constexpr int cx = 35, cy = 29;
    const uint8_t sweep = (t * 16) / 1200;
    d.drawCircle(cx, cy, 22);
    d.drawCircle(cx, cy, 11);
    d.drawHLine(cx - 25, cy, 4); d.drawHLine(cx + 22, cy, 4);
    d.drawVLine(cx, cy - 25, 4); d.drawVLine(cx, cy + 22, 4);
    d.drawLine(cx, cy, cx + vectors[sweep][0], cy + vectors[sweep][1]);
    const uint8_t trail = (sweep + 15) % 16;
    for (int radius = 5; radius < 21; radius += 4) {
      d.drawPixel(cx + vectors[trail][0] * radius / 22,
                  cy + vectors[trail][1] * radius / 22);
    }
    d.drawDisc(cx, cy, 2);
    if (t >= 600) {
      d.drawDisc(cx + 13, cy - 10, 2);
      if ((t / 100) % 2 == 0) corners(d, cx + 8, cy - 15, 11, 11, 3);
    }
    d.drawStr(71, 19, "WAKE");
    d.drawStr(71, 30, "THE");
    d.drawStr(71, 41, "WAVES");
    d.drawStr(9, 58, "SHORE SPOTTER / BOOT");
  } else if (t < 2000) {
    const uint32_t phase = t - 1200;
    const int inset = 4 + (phase < 400 ? phase * 9 / 400 : 9);
    corners(d, 64 - (36 - inset), 5 + inset / 2,
            2 * (36 - inset) + 1, 44 - inset, 5);
    wave(d, 64, 16, 3 + phase * 14 / 700);
    if (phase >= 400) {
      d.drawPixel(84, 12); d.drawPixel(88, 15); d.drawPixel(86, 18);
    }
    centered(d, 57, "CATCH THE WAVE");
  } else {
    wave(d, 64, 5, 16);
    d.drawPixel(84, 4); d.drawPixel(88, 7); d.drawPixel(86, 10);
    d.setFont(u8g2_font_7x13B_tr);
    // Reveal the wordmark, then hold it still for the final 700 ms.
    static constexpr char wordmark[] = "SHORE SPOTTER";
    char revealed[sizeof(wordmark)]{};
    const uint8_t count = t < 2300 ? 1 + (t - 2000) * 12 / 300 : 13;
    for (uint8_t i = 0; i < count; ++i) revealed[i] = wordmark[i];
    d.drawStr((128 - d.getStrWidth(wordmark)) / 2, 44, revealed);
    d.setFont(u8g2_font_5x7_tr);
    centered(d, 56, "CLIENT");
  }

  // A bounded boot-animation timeline, not a hardware initialization meter.
  d.drawHLine(8, 62, 112);
  const uint8_t progress = t * 112 / kDurationMs;
  if (progress) d.drawBox(8, 60, progress, 2);
}

}  // namespace client_boot_animation
