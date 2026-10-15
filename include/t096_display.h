#pragma once

#include <Adafruit_ST7735.h>

// Heltec's T096 panel differs from Adafruit's MINI160x80_PLUGIN preset.
// Match Heltec_nRF52/libraries/Adafruit-ST7735/Adafruit_ST7735.cpp without
// patching the dependency. Apply offsets before rotation derives x/y starts.
class T096Display : public Adafruit_ST7735 {
 public:
  using Adafruit_ST7735::Adafruit_ST7735;

  void initPanel() {
    initR(INITR_MINI160x80_PLUGIN);
    setColRowStart(24, 0);
    setRotation(1);
  }
};
