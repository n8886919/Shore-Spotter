#pragma once
// Fixed per-pair channels: no scan, no automatic channel changes. Both ends
// must be built with the same group. Frequency alone is not RF certification.
#ifndef SHORE_RF_GROUP
#define SHORE_RF_GROUP 0
#endif
static_assert(SHORE_RF_GROUP >= 0 && SHORE_RF_GROUP <= 255, "RF group must be 0..255");
#ifndef SHORE_RF_FREQUENCY_MHZ
static_assert(SHORE_RF_GROUP <= 1, "Extra groups require the canonical RF config loader");
#define SHORE_RF_FREQUENCY_MHZ (SHORE_RF_GROUP == 0 ? 923.2f : 923.8f)
#endif
namespace radio_profile {
constexpr unsigned group = SHORE_RF_GROUP;
constexpr float frequencyMhz = SHORE_RF_FREQUENCY_MHZ;
constexpr float bandwidthKhz = 125.0f;
constexpr int spreadingFactor = 10, codingRate = 5, syncWord = 0x12;
}
