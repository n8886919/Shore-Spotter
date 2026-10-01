#pragma once

// Mesh Node T096 / HT-n5262G. Values are verified against Heltec's
// Heltec_nRF52 board-config.h and variant.h, not copied from another nRF52 board.
namespace t096_pins {
constexpr int kLoraDio1 = 21, kLoraNss = 5, kLoraReset = 16, kLoraBusy = 19;
constexpr int kLoraSck = 40, kLoraMiso = 14, kLoraMosi = 11;
constexpr int kFemPower = 30, kFemCsd = 12, kFemCtx = 41;
constexpr int kGnssEnable = 6, kGnssReset = 46, kGnssPps = 43;
constexpr int kGnssRx = 23, kGnssTx = 25;
constexpr int kBatteryAdc = 3, kBatteryAdcControl = 47;
constexpr int kUserButton = 42, kLed = 28;
constexpr int kVextControl = 26;
constexpr int kFemDriveDbm = 0;  // SX1262 drive only; antenna output is unmeasured.
}  // namespace t096_pins
