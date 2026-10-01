#pragma once

#include <stdint.h>

// RadioLib SX1262::begin() takes preamble length before TCXO voltage. Keep
// these values together and route the call through this adapter so a host fake
// can verify the physical-radio arguments without Arduino headers.
namespace t096_radio {
constexpr uint16_t kPreambleSymbols = 8;
constexpr float kTcxoVoltage = 1.8f;

template <typename Radio>
int16_t begin(Radio &radio, float frequencyMhz, float bandwidthKhz,
              uint8_t spreadingFactor, uint8_t codingRate, uint8_t syncWord,
              int8_t outputDriveDbm) {
  return radio.begin(frequencyMhz, bandwidthKhz, spreadingFactor, codingRate,
                     syncWord, outputDriveDbm, kPreambleSymbols, kTcxoVoltage);
}
}  // namespace t096_radio
