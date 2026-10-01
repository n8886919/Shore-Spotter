#pragma once
#include <stdint.h>

// Station hardware is selected at build time. Keep the original T-Beam mapping
// as the default so existing station builds retain their exact pins and rails.
#if defined(BOARD_HELTEC_V4) && !defined(ROLE_STATION)
#error "BOARD_HELTEC_V4 is a Station-only hardware profile"
#endif
#if defined(BOARD_HELTEC_V4) && defined(HELTEC_V4_2) && defined(HELTEC_V4_3)
#error "HELTEC_V4_2 and HELTEC_V4_3 cannot both be selected"
#endif
#if defined(HELTEC_FEM_GC1109) && !defined(HELTEC_V4_2)
#error "HELTEC_FEM_GC1109 is only valid for Heltec V4.2"
#endif

namespace station_board {
#if defined(BOARD_HELTEC_V4)
constexpr bool kHeltecV4 = true;
constexpr bool kHasPmu = false;
constexpr bool kHasGnss = false;  // 38/39 are only on the optional GNSS expansion.
constexpr bool kHasSd = false;
constexpr bool kHasBme280 = false;
constexpr int kLoraSck = 9;
constexpr int kLoraMiso = 11;
constexpr int kLoraMosi = 10;
constexpr int kLoraNss = 8;
constexpr int kLoraDio1 = 14;
constexpr int kLoraReset = 12;
constexpr int kLoraBusy = 13;
constexpr int kOledSda = 17;
constexpr int kOledScl = 18;
constexpr int kOledReset = 21;
constexpr int kServoPin = 4;  // exposed GPIO on the Heltec V4 header
constexpr int kVext = 36;     // active-low OLED/FEM supply enable
constexpr int kFemPower = 7;
constexpr int kFemCsd = 2;
constexpr int kTxPowerDbm = 0;  // SX1262 drive only; board RF output is unmeasured.
enum class HeltecFem : uint8_t { Unknown, Gc1109, Kct8103l };
inline HeltecFem heltecFem = HeltecFem::Unknown;
inline const char *heltecFemName() {
  return heltecFem == HeltecFem::Gc1109 ? "gc1109" :
      heltecFem == HeltecFem::Kct8103l ? "kct8103l" : "unknown";
}
inline const char *deviceBoard() {
#if defined(HELTEC_V4_2)
  return "heltec-v4.2-station";
#elif defined(HELTEC_V4_3)
  return "heltec-v4.3-station";
#else
  return heltecFem == HeltecFem::Gc1109 ? "heltec-v4-gc1109-station" :
      heltecFem == HeltecFem::Kct8103l ? "heltec-v4-kct8103l-station" :
      "heltec-v4-fem-unknown-station";
#endif
}
#if defined(HELTEC_V4_2)
constexpr const char *kDeviceBoard = "heltec-v4.2-station";
#elif defined(HELTEC_V4_3)
constexpr const char *kDeviceBoard = "heltec-v4.3-station";
#else
constexpr const char *kDeviceBoard = "heltec-v4-autodetect-station";
#endif
#else
constexpr bool kHeltecV4 = false;
constexpr bool kHasPmu = true;
constexpr bool kHasGnss = true;
constexpr bool kHasSd = true;
constexpr bool kHasBme280 = true;
constexpr int kLoraSck = 12;
constexpr int kLoraMiso = 13;
constexpr int kLoraMosi = 11;
constexpr int kLoraNss = 10;
constexpr int kLoraDio1 = 1;
constexpr int kLoraReset = 5;
constexpr int kLoraBusy = 4;
constexpr int kOledSda = 17;
constexpr int kOledScl = 18;
constexpr int kOledReset = -1;
constexpr int kServoPin = 21;
constexpr int kTxPowerDbm = 17;
constexpr const char *kDeviceBoard = "tbeam-supreme-station";
inline const char *deviceBoard() { return kDeviceBoard; }
#endif

#if defined(ARDUINO)
inline void prepareStationPeripherals() {
#if defined(BOARD_HELTEC_V4)
  pinMode(kVext, OUTPUT);
  digitalWrite(kVext, LOW);
#endif
}

inline bool prepareStationRadioFrontend() {
#if defined(BOARD_HELTEC_V4)
  // Meshtastic's Heltec V4 variant probes CSD (GPIO2) while the FEM is powered:
  // high identifies KCT8103L, low identifies GC1109.  Require every sample to
  // agree so a floating or unsettled strap never selects an RF path silently.
  pinMode(kFemPower, OUTPUT); digitalWrite(kFemPower, HIGH);
  delay(1);
#if defined(HELTEC_V4_2)
  heltecFem = HeltecFem::Gc1109;
#elif defined(HELTEC_V4_3)
  heltecFem = HeltecFem::Kct8103l;
#else
  pinMode(kFemCsd, INPUT);
  delay(1);
  uint8_t high = 0, low = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    if (digitalRead(kFemCsd)) ++high; else ++low;
    delay(1);
  }
  if (high == 8) heltecFem = HeltecFem::Kct8103l;
  else if (low == 8) heltecFem = HeltecFem::Gc1109;
  else return false;
#endif
  pinMode(kFemCsd, OUTPUT); digitalWrite(kFemCsd, HIGH);
#if defined(HELTEC_V4_2)
  pinMode(46, OUTPUT); digitalWrite(46, LOW);  // GC1109 PA bypass at boot/RX
#elif defined(HELTEC_V4_3)
  pinMode(5, OUTPUT); digitalWrite(5, LOW);    // KCT8103L RX LNA
#else
  if (heltecFem == HeltecFem::Gc1109) {
    pinMode(46, OUTPUT); digitalWrite(46, LOW);
  } else {
    pinMode(5, OUTPUT); digitalWrite(5, LOW);
  }
#endif
#endif
  return true;
}
#endif
}  // namespace station_board

// Pending Station downlink code calls this immediately before/after TX. It is
// intentionally a no-op for the T-Beam profile.
inline void setStationRadioTransmit(bool transmit) {
#if defined(ARDUINO) && defined(BOARD_HELTEC_V4)
#if defined(HELTEC_V4_2)
  digitalWrite(46, transmit ? HIGH : LOW);  // CPS: PA only while transmitting
#elif defined(HELTEC_V4_3)
  digitalWrite(5, transmit ? HIGH : LOW);   // CTX: TX/bypass, RX LNA
#else
  if (station_board::heltecFem == station_board::HeltecFem::Gc1109)
    digitalWrite(46, transmit ? HIGH : LOW);
  else if (station_board::heltecFem == station_board::HeltecFem::Kct8103l)
    digitalWrite(5, transmit ? HIGH : LOW);
#endif
#else
  (void)transmit;
#endif
}
