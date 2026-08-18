#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <Preferences.h>
#include <Adafruit_BME280.h>
#include <esp_system.h>
#include <esp_mac.h>

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#include <U8g2lib.h>

#include "protocol.h"  // shared LoRa wire protocol (client + server)
#include "geo_math.h"  // pure maths (angles / bearing / circle fit / grading)

#if defined(ROLE_SERVER)
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <SensorQMC6310.hpp>  // SensorLib: QMC6310 magnetometer (station heading)
#include "alerts.h"    // 現場提醒的門檻與分級
#include "web_icon.h"  // 分頁圖示（由 tools/make_icon.py 產生）
#include "web_ui.h"
#include "wifi_config.h"  // phone hotspot SSID / password (edit there)
#endif

// IMPORTANT:
// This project keeps one main.cpp and splits behavior by build flags:
// ROLE_CLIENT (water side) / ROLE_SERVER (shore side).
// Upload env:tbeam-client or env:tbeam-server to each board.
//
// Fail fast at compile time: exactly one role must be selected.
#if !defined(ROLE_CLIENT) && !defined(ROLE_SERVER)
#error "No role selected: define ROLE_CLIENT or ROLE_SERVER (use env:tbeam-client / env:tbeam-server)."
#endif
#if defined(ROLE_CLIENT) && defined(ROLE_SERVER)
#error "Both roles defined: pick only ROLE_CLIENT or ROLE_SERVER, not both."
#endif

// T-Beam Supreme (SX1262) pins from LilyGO hardware docs.
constexpr int LORA_SCK = 12;
constexpr int LORA_MISO = 13;
constexpr int LORA_MOSI = 11;
constexpr int LORA_NSS = 10;
constexpr int LORA_DIO1 = 1;
constexpr int LORA_NRST = 5;
constexpr int LORA_BUSY = 4;

// Taiwan legal LoRa sub-band (AS923 profile commonly uses 923.2 MHz).
constexpr float RF_FREQUENCY = 923.2;
constexpr float RF_BW = 125.0;
constexpr int RF_SF = 9;
// Coding rate 4/5 (RadioLib takes the denominator, 5..8).
// 4/7 -> 4/5 removes ~21 % of the airtime of every packet at effectively no
// sensitivity cost — Semtech quotes its sensitivity figures at 4/5; the extra
// parity of 4/7 buys robustness against burst interference, not link budget.
// SF is deliberately left at 9: SF8 would halve the airtime again but costs
// 2.5 dB (~25 % range), which is the wrong trade for a tracker worn in the water.
//
// RF_SF / RF_BW / RF_FREQUENCY / RF_SYNC_WORD must match on both boards or the
// link dies. RF_CR does NOT: in explicit-header mode (RadioLib's default, and
// implicitHeader() is never called here) the payload coding rate is carried in
// the header itself, which is always sent at 4/8, so a receiver decodes any CR
// regardless of how it is configured. Mixed-CR boards interoperate fine — CR
// only sets what this board uses for its own transmissions.
constexpr int RF_CR = 5;
constexpr int RF_SYNC_WORD = 0x12;
constexpr int TX_POWER_DBM = 17;
constexpr int TX_POWER_MIN_DBM = 10;
constexpr int TX_POWER_MAX_DBM = 22;
constexpr uint32_t ATPC_EVAL_MS = 15000;

constexpr uint32_t SEND_INTERVAL_MS = 1000;
constexpr uint32_t TELEMETRY_INTERVAL_MS = 30000;  // battery + env packet rate
// TELEMETRY_INTERVAL_MS is an exact multiple of SEND_INTERVAL_MS, so a telemetry
// packet that is simply "due" always lands on top of a position packet and the
// ACK that follows it — both sides end up transmitting at once and both packets
// are lost. Telemetry is therefore only started inside the quiet slot of the
// position cycle. The slot bounds are derived at boot from the radio's own
// time-on-air (see computeAirtimeBudget) rather than hand-tuned, so they follow
// any change to RF_SF / RF_CR / packet size / SEND_INTERVAL_MS automatically.
constexpr uint32_t TELEMETRY_SLOT_GUARD_MS = 80;
constexpr uint32_t BATTERY_UPDATE_MS = 5000;

// The server acknowledges every ACK_EVERY_N-th position packet instead of all
// of them. An ACK is ~185 ms of airtime at SF9/CR4-5 and only the client
// consumes it — for ATPC and link liveness, neither of which needs 1 Hz
// resolution. Selection is by sequence number rather than a server-side counter
// so a dropped packet cannot slide the schedule, and both ends agree on which
// seq carries an ACK without extra state.
//
// Keep this a power of two: seq is uint16_t, and 65536 % N == 0 only then, so
// the cadence stays continuous across sequence wrap. Also note txSeq is shared
// with telemetry packets, so the 30 s telemetry burns one sequence number and
// the ACK spacing shows a single 3- or 5-packet gap around it. Both are
// cosmetic — nothing keys off the ACK arriving on an exact schedule.
constexpr uint16_t ACK_EVERY_N = 4;
constexpr uint32_t ACK_PERIOD_MS = ACK_EVERY_N * SEND_INTERVAL_MS;
// Client link thresholds derived from the ACK cadence so they cannot drift out
// of sync when ACK_EVERY_N changes. At N=4 these evaluate to the 16 s / 20 s
// the firmware used when every packet was acknowledged.
constexpr uint32_t ACK_STALE_MS = 4 * ACK_PERIOD_MS;
constexpr uint32_t ATPC_NO_ACK_MS = 5 * ACK_PERIOD_MS;
constexpr uint32_t ENV_UPDATE_MS = 5000;
constexpr uint32_t SERVER_IDLE_LOG_MS = 5000;
// One log line per received packet floods the 4 KB ring in ~16 s, so whatever you
// opened the log to look at has already scrolled out — the RX detail crowds out
// [SERVO], [MAGCAL] and [TRACK] entirely. Accumulate instead and print one
// summary a minute, which keeps roughly half an hour of history in the ring.
constexpr uint32_t RX_SUMMARY_MS = 60000;
constexpr uint32_t LINK_TIMEOUT_MS = 5000;
constexpr uint32_t LINK_WARN_MS = 15000;
constexpr uint32_t DISPLAY_REFRESH_MS = 500;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000;  // server: re-attempt hotspot every 5 s when offline
// PWR-key IRQ polling rate. loop() no longer blocks, so it spins at several kHz
// and polling the PMU every pass would mean thousands of I2C transactions per
// second for a button that only needs to feel instant to a human.
constexpr uint32_t PMU_KEY_POLL_MS = 100;
// GPS UART is 9600 baud (960 B/s); the default 256 B driver buffer overflows in
// 267 ms, which is shorter than a single blocking LoRa transmit. 1 KB covers a
// full second of NMEA so no sentence is lost while the radio is busy.
constexpr size_t GPS_RX_BUFFER_BYTES = 1024;

// Battery percentage scale (single Li-ion cell): 0% at 3.2 V, 100% at 4.15 V.
constexpr uint16_t BATT_EMPTY_MV = 3200;       // 0 %
constexpr uint16_t BATT_PCT_FULL_MV = 4150;    // 100 %
constexpr uint16_t BATT_SHUTDOWN_MV = 3200;    // below 0 % -> auto power off
constexpr uint16_t BATT_PRESENT_MIN_MV = 2500; // ignore implausible/no-battery reads

// Client OLED is normally off to save power; short-press PWR wakes it briefly.
constexpr uint32_t CLIENT_SCREEN_WAKE_MS = 10000;

#if defined(ROLE_SERVER)
// Camera servo (GXServo QY3242BLS/GX3242 42KG) driven by ESP32 LEDC PWM on IO21.
constexpr int SERVO_PIN = 21;
constexpr int SERVO_MIN_US = 500;    // -> 0 deg
constexpr int SERVO_MAX_US = 2500;   // -> 180 deg
constexpr uint32_t SERVO_PWM_HZ = 333;
// ESP32-S3 LEDC timers support at most 14-bit resolution.  A 16-bit attach is
// rejected by Arduino-ESP32 3.x, leaving the pin with no PWM output.
constexpr uint8_t SERVO_PWM_RES_BITS = 14;
constexpr uint32_t SERVO_PWM_MAX_DUTY = (1UL << SERVO_PWM_RES_BITS) - 1;
constexpr int SERVO_LEDC_CH = 0;     // LEDC channel (arduino-esp32 2.x)
constexpr uint8_t SERVO_CAL_VERSION = 2;  // v2 uses CCW-positive servo geometry
constexpr uint32_t MAG_SAMPLE_MS = 200;
// Heading low-pass, applied to the field vector (see sampleMag).
// alpha = 1 - exp(-MAG_SAMPLE_MS / tau) with tau = 0.5 s.
constexpr float MAG_FILTER_ALPHA = 0.33f;

// Camera tracking loop. The servo is refreshed far faster than position packets
// arrive (1 Hz): between packets the surfer's position is dead-reckoned from the
// velocity vector already carried in PositionPayload, so the camera pans
// continuously instead of stepping once per received packet — and keeps panning
// through a dropped packet instead of freezing for a whole second.
constexpr uint32_t TRACK_UPDATE_MS = 50;      // 20 Hz servo refresh
constexpr float DR_MIN_SPEED_CMS = 30.0f;     // below this the GPS course is noise
constexpr float DR_MAX_AGE_S = 2.0f;          // stop projecting after ~2 lost packets
constexpr float SERVO_MAX_SLEW_DEG_S = 120.0f;  // pan rate limit (smooth footage)

// --- Magnetometer hard-iron calibration -----------------------------------
// Spin the whole station through one slow horizontal turn, CLOCKWISE seen from
// above; the firmware fits a circle to the locus and stores its centre as the
// hard-iron offset.
//
// That turn also tells the firmware HOW THE BOARD IS MOUNTED. Whichever sensor
// axis barely moves through a horizontal turn is the one lying along the world
// vertical, so the other two span the horizontal plane and are the pair the
// heading has to come from. This is why the board no longer has to lie flat:
// standing it on edge only moves the constant axis from Z to X or Y. The pair is
// kept in cyclic order (axisA x axisB = +up) and swapped if the locus turned out
// to run the other way, which settles the sign from data instead of from a guess
// about which way the sensor is silkscreened.
//
// Why it matters: uncalibrated, heading is a sine-distorted function of true
// yaw. The on-board 18650's nickel-plated steel can sits centimetres from the
// sensor and can offset the field by tens of uT against a ~37 uT horizontal
// field, so the distortion reaches 20-30 deg AND varies with which way the rig
// faces. That is precisely what makes a stored mount_offset stop being valid
// after the station is packed up and set down facing a different way at another
// spot — i.e. it is the reason the aim calibration currently has to be redone
// every session. Fix the hard iron and mount_offset becomes a true constant.
constexpr uint32_t MAG_CAL_SAMPLE_MS = 50;        // 20 Hz while collecting
constexpr size_t   MAG_CAL_MAX_SAMPLES = 180;
constexpr float    MAG_CAL_MIN_STEP_DEG = 2.0f;   // spread samples round the circle
constexpr uint8_t  MAG_CAL_BINS = 36;             // 10 deg coverage bins
constexpr uint8_t  MAG_CAL_BINS_REQUIRED = 34;    // allow a small gap
constexpr uint32_t MAG_CAL_TIMEOUT_MS = 120000;
constexpr uint8_t  MAG_CAL_VERSION = 2;  // v2 also stores the horizontal pair

// --- Heading freeze --------------------------------------------------------
// The live heading is not used while tracking: it is snapshotted when tracking
// starts so magnetometer noise (servo current, vibration, filter ripple) never
// reaches the servo. Only a change bigger than the deadband is taken to mean
// the tripod was genuinely moved, which re-snapshots. With GPS at ~1 m the
// magnetometer is otherwise the dominant error term in the pointing budget.
constexpr float MAG_FREEZE_DEADBAND_DEG = 2.0f;
#endif

// GPS UART defaults (common on T-Beam family, override if your board differs).
// TinyGPSPlus 的 isValid() 只要「曾經」定位成功就永遠是 true —— 失去定位之後
// lat/lon/speed/course 全部凍結在最後一筆，只有 age() 會漲（見 TinyGPS++.cpp
// 的 commit() 只在 sentenceHasFix 為真時呼叫）。所以判斷「現在有沒有定位」一律
// 要看 age()，否則天線入水或走進死角時，系統會非常有自信地一直對著一個人早就
// 不在的座標，而且不會有任何徵兆。NMEA 是 1 Hz，這裡容忍約 3 筆遺失。
constexpr uint32_t GPS_FIX_MAX_AGE_MS = 3000;

constexpr int GPS_RX_PIN = 9;
constexpr int GPS_TX_PIN = 8;
constexpr int GPS_EN_PIN = 7;
constexpr uint32_t GPS_BAUD = 9600;

constexpr int PMU_SDA_PIN = 42;
constexpr int PMU_SCL_PIN = 41;

// On-board SH1106 OLED lives on I2C bus 0 (shared sensor bus).
// Its power rail is ALDO1 on the AXP2101 PMU and must be enabled first.
constexpr int OLED_SDA_PIN = 17;
constexpr int OLED_SCL_PIN = 18;
// SH1106 的位址取決於板上是哪一版磁力計：QMC6310U 在 0x1C -> 螢幕 0x3C；
// QMC6310N 在 0x3C -> 螢幕 0x3D（見 docs/hardware.md 的 I2C 位址表）。寫死
// 0x3C 在 N 版板子上會讓 u8g2 把畫面資料寫進磁力計的暫存器，螢幕全黑而且沒有
// 任何錯誤訊息。SERVER 開機時由 initMag() 探測到的位址回推，CLIENT 沒有磁力計
// 所以維持預設值。
constexpr uint8_t OLED_ADDR_DEFAULT = 0x3C;
constexpr uint8_t OLED_ADDR_ALT = 0x3D;
static uint8_t oledI2CAddr = OLED_ADDR_DEFAULT;

SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_NRST, LORA_BUSY);
TinyGPSPlus gps;
XPowersPMU pmu;

HardwareSerial GPSSerial(1);
TwoWire PMUWire = TwoWire(1);
Preferences prefs;
Adafruit_BME280 envSensor;
bool envSensorOnline = false;
uint32_t nextEnvMs = 0;
int16_t cachedTempC10 = INT16_MIN;
uint8_t cachedHumidityPct = 0xFF;
String cachedApIp = "";
#if defined(ROLE_SERVER)
IPAddress cachedApIpAddr;  // last address seen, to detect DHCP changes
#endif

// Shared OLED object — client enables it only during boot-info and shutdown screens.
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

#if defined(ROLE_SERVER)
// Rolling log the web UI can render. The station lives on a tripod at the beach,
// where nobody is going to tether a laptop to read the serial port — and OTA
// (espota) only uploads firmware, it never carries logs.
constexpr size_t LOG_BUF_BYTES = 4096;
static char     logBuf[LOG_BUF_BYTES];
static size_t   logHead = 0;     // next write position
static bool     logWrapped = false;
static uint32_t logTotal = 0;    // monotonic byte count, lets the UI fetch deltas

static void logPush(uint8_t c) {
  logBuf[logHead] = (char)c;
  logHead = (logHead + 1) % LOG_BUF_BYTES;
  if (logHead == 0) logWrapped = true;
  logTotal++;
}
#endif

// Tee for all firmware logging. Output still goes to the USB serial port; on the
// server it is additionally captured into logBuf. Deriving from Print inherits
// every print()/println() overload, so call sites change in name only.
// Safe without locking: nothing logs from an ISR (the DIO1 handler only sets a
// flag) and the web server is serviced from loop(), so this is single-threaded.
// Buffered to whole lines before touching the USB serial port. This matters far
// more than it looks: Print::print(F("...")) emits one character at a time, and
// HWCDC::write() waits up to tx_timeout_ms (100 ms by default) per call when the
// port is enumerated but nothing is draining it — exactly the state of a board
// plugged into a PC with no terminal open. That turned a ~110-character log line
// into ~11 s of blocking. Buffering makes it one bulk write per line instead of
// one per character; setTxTimeoutMs(0) in setup() then removes the wait entirely.
// Dropped serial output is acceptable because the server's ring buffer (and the
// web 紀錄 tab) is the authoritative log.
class LogTee : public Print {
 public:
  size_t write(uint8_t c) override {
    lineBuf_[lineLen_++] = c;
    if (c == '\n' || lineLen_ >= sizeof(lineBuf_)) flushLine();
    return 1;
  }
  size_t write(const uint8_t *b, size_t n) override {
    for (size_t i = 0; i < n; i++) write(b[i]);
    return n;
  }

 private:
  void flushLine() {
    if (lineLen_ == 0) return;
    Serial.write(lineBuf_, lineLen_);
#if defined(ROLE_SERVER)
    for (size_t i = 0; i < lineLen_; i++) logPush(lineBuf_[i]);
#endif
    lineLen_ = 0;
  }
  uint8_t lineBuf_[256];  // longest log line here is ~300 B, so at most 2 writes
  size_t  lineLen_ = 0;
};
static LogTee Log;

#if defined(ROLE_SERVER)
struct DecodedData {
  uint16_t srcId;
  uint16_t seq;
  double   lat;
  double   lon;
  uint8_t  fix;
  uint16_t speedCmS;
  uint16_t courseDeg10;
  int16_t  accelCmS2;
  uint8_t  satellites;
  uint8_t  hdop10;
};

struct DecodedTelemetry {
  uint16_t srcId;
  uint16_t batteryMv;
  int8_t   tempC;       // INT8_MIN = no sensor
  uint8_t  humidityPct; // 0xFF = no sensor
};

// Runtime-mutable whitelist — seeded from DEFAULT_WHITELIST at boot.
// Each id = last 2 bytes of the client ESP32 chip MAC (printed at boot).
constexpr uint16_t DEFAULT_WHITELIST[] = {0xE91C};  // 48:ca:43:57:e9:1c (COM7)
constexpr size_t   WHITELIST_MAX = 16;
static uint16_t clientWhitelist[WHITELIST_MAX]{};
static size_t   clientWhitelistCount = 0;

static bool isClientAllowed(uint16_t id) {
  for (size_t i = 0; i < clientWhitelistCount; i++) {
    if (clientWhitelist[i] == id) return true;
  }
  return false;
}
#endif

uint16_t txSeq = 0;
uint32_t nextSendMs = 0;
uint32_t lastSendMs = 0;  // start of the last position TX (telemetry slot anchor)
uint32_t nextTelemetryMs = 0;
uint32_t nextPmuKeyMs = 0;
uint32_t nextBatteryMs = 0;
// Telemetry quiet slot, filled in by computeAirtimeBudget() at boot.
uint32_t telemetrySlotMinMs = 0;
uint32_t telemetrySlotMaxMs = 0;
uint16_t cachedBatteryMv = 0;
bool pmuOnline = false;
uint16_t nodeId = 0;  // set in setup() from chip MAC last 2 bytes

static uint32_t bootMs = 0;

#if defined(ROLE_CLIENT)
// Client-side velocity / acceleration tracking
static uint16_t prevSpeedCmS   = 0;
static uint32_t prevSpeedMs    = 0;
static int16_t  smoothAccelCmS2 = 0;
static uint32_t lastAckRxMs = 0;
static uint32_t ackRxCount = 0;
static uint32_t ackMissCount = 0;
static uint32_t lastAckMissMarkMs = 0;
static int8_t currentTxPowerDbm = TX_POWER_DBM;
static bool atpcEnabled = true;
static int16_t lastAckRssiDbm10 = -1270;
static int8_t lastAckSnrDb10 = -127;
static uint32_t nextAtpcEvalMs = 0;

// Client OLED wake state (short-press PWR turns the screen on for a few seconds)
static bool clientOledAwake = false;
static uint32_t clientOledOffMs = 0;
static uint32_t nextClientOledRefreshMs = 0;

// Interrupt-driven ACK reception, mirroring the server's RX path.
//
// This used to be a blocking radio.receive() called on every loop() pass.
// RadioLib defaults that call's timeout to 500 % of the expected time-on-air
// (~1.1 s for a 20-byte ACK at SF9/CR4-7), and the call was reached again right
// after the ACK had already been consumed — so every cycle spent an extra ~1.1 s
// parked in a dead wait. That stretched the nominal 1 Hz position cadence to
// ~1.7 s and overflowed the GPS UART buffer on the way. The ISR now only raises
// a flag and loop() drains the packet when one genuinely arrives.
volatile bool clientRxFlag = false;
void IRAM_ATTR onClientDio1() { clientRxFlag = true; }
#endif

// Derive a node id from the last 2 bytes of the ESP32's factory-burned MAC.
// 65536 possible values — collision probability negligible for any real deployment.
static uint16_t derivedNodeId() {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  uint16_t id = ((uint16_t)mac[4] << 8) | mac[5];
  // Avoid colliding with reserved SERVER_ID.
  return id == SERVER_ID ? id ^ 0x0100 : id;
}

#if defined(ROLE_SERVER)
DecodedData lastData{};
bool havePkt = false;
uint32_t lastRxMs = 0;
float lastRssi = 0;
float lastSnr = 0;
uint32_t nextDisplayMs = 0;
uint32_t nextWifiRetryMs = 0;
uint32_t wifiReconnectingUntilMs = 0;
bool otaReady = false;
uint32_t nextServerIdleLogMs = 0;

// Telemetry (slow path, MSG_TELEMETRY every 30 s)
DecodedTelemetry lastTelemetry{0, 0, INT8_MIN, 0xFF};
bool haveTelemetry = false;
uint32_t lastTelemetryRxMs = 0;

// 進水偵測的基準濕度（-1 = 還沒有基準）。見 include/alerts.h：單看絕對值會誤報，
// 因為海邊空氣本來就 80% 起跳；進水真正的特徵是相對開機值單調上升。
static int clientHumBaselinePct = -1;
static int serverHumBaselinePct = -1;

// LoRa rolling stats (last RSSI_WINDOW received packets)
constexpr size_t RSSI_WINDOW = 20;
static float rssiRing[RSSI_WINDOW]{};
static float snrRing[RSSI_WINDOW]{};
static size_t rssiRingIdx   = 0;
static size_t rssiRingCount = 0;
static uint32_t pktsThisWindow  = 0;
static uint32_t pktWindowStartMs = 0;
static float    cachedPktRate   = 0.0f;  // pkts/s averaged over ~60 s

// Servo + tracking state.
// Geometry (viewed from above, increasing servo angle turns CCW while compass
// bearings increase CW): world_bearing = mag_heading - servo_angle + mountOffset
//   * Manual: operator nudges servo_angle until the camera is on the surfer.
//   * Start : mountOffset is locked from the current (bearing, heading, angle).
//   * Track : servo_angle = heading_now + mountOffset - bearing_now (clamped 0..180).
// The magnetometer term lets the station be bumped/rotated mid-session.
enum TrackMode : uint8_t { MODE_IDLE, MODE_MANUAL, MODE_TRACKING, MODE_PAUSED };
static TrackMode trackMode = MODE_MANUAL;
static float servoAngleDeg = 90.0f;   // current commanded servo angle (0..180)
static float servoTargetDeg = 90.0f;  // desired angle while tracking
static float mountOffsetDeg = 0.0f;   // servo-to-world mounting offset (persisted)
// The heading the station was facing when that offset was locked. mount_offset is
// only exactly right in that pose: an elliptical magnetometer locus (see
// magCalEllipseDeg) makes the heading error a function of which way the rig faces,
// and only the DIFFERENCE between the calibration pose and the current one leaks
// into the aim. Keeping the pose lets the firmware state that error instead of
// leaving the operator to wonder — turn back toward the calibration pose, or
// recalibrate where you actually stand, and it goes to zero.
static float mountCalHeadingDeg = -1.0f;
static bool  mountCalibrated = false;
static bool  servoPwmReady = false;
static uint32_t nextTrackMs = 0;      // TRACK_UPDATE_MS tick
static uint32_t lastServoStepMs = 0;  // 0 = no previous step (slew dt unknown)
static float frozenHeadingDeg = 0.0f; // heading snapshot in use while tracking
static bool  headingFrozen = false;

// Magnetometer (QMC6310) — station board heading in degrees (-1 = invalid)
static SensorQMC6310 mag;
static bool  magOnline = false;
static float magHeadingDeg = -1.0f;
static uint32_t nextMagMs = 0;

// The two sensor axes that span the horizontal plane (0=X, 1=Y, 2=Z) and the
// hard-iron offset on each in Gauss, subtracted before the heading is computed.
// X,Y with zero offsets until calibrated, which is exactly the flat-board,
// uncorrected behaviour this replaces.
static uint8_t magAxisA = 0;
static uint8_t magAxisB = 1;
static float magOffsetA = 0.0f;
static float magOffsetB = 0.0f;
static bool  magCalibrated = false;
static float magCalResidualDeg = -1.0f;  // total fit residual; -1 = unknown
static float magCalFieldGauss = -1.0f;   // fitted radius, sanity check vs ~0.37 G
// The total residual lumps together two faults that need opposite fixes, so it is
// split. An ELLIPTICAL locus (soft iron from the servo's steel gears, a board that
// is not square to gravity, or the board tilting as it is turned) shows up as a
// 2-theta component of the radial error; vibration and sensor noise are whatever
// is left. "Move the board" vs "turn it more steadily" — one lumped number cannot
// tell you which.
static float magCalEllipseDeg = -1.0f;   // max heading error implied by the ellipse
static float magCalScatterDeg = -1.0f;   // random part, after the ellipse is removed
static float magCalSweepDeg = 0.0f;      // net rotation actually turned

// Heading low-pass state (file scope so a fresh calibration can re-seed it).
static float magFiltX = 0.0f;
static float magFiltY = 0.0f;
static bool  magFiltInit = false;

// A one-shot heading sample taken at lock time bakes that instant's noise into a
// constant that then lives in NVS for every later session. So the lock uses the
// mean field vector over the last few seconds instead. Averaging the VECTOR and
// not the angle: degrees cannot be averaged across the 360/0 wrap. This only
// helps against the zero-mean part of the error — hard iron, the servo's steel
// gears and tilt are all deterministic and survive any amount of averaging.
constexpr uint8_t MAG_AVG_WINDOW = 25;  // 25 samples at 5 Hz = 5 s
static float   magAvgX[MAG_AVG_WINDOW];
static float   magAvgY[MAG_AVG_WINDOW];
static uint8_t magAvgIdx = 0;
static uint8_t magAvgCount = 0;

enum MagCalState : uint8_t {
  MAGCAL_IDLE, MAGCAL_COLLECTING, MAGCAL_DONE, MAGCAL_FAILED
};
static MagCalState magCalState = MAGCAL_IDLE;
// Raw 3-axis samples: which two of them matter is only known once the turn has
// gone far enough to show which axis is the vertical one. The collector keeps its
// own copy of the pair so a cancelled or timed-out turn cannot leave the live
// heading using a plane guessed from a quarter of a circle.
static float    magCalRaw[MAG_CAL_MAX_SAMPLES][3];
static uint8_t  magCalAxisA = 0;
static uint8_t  magCalAxisB = 1;
static size_t   magCalCount = 0;
static uint64_t magCalBinMask = 0;
static uint32_t magCalStartMs = 0;
static float    magCalMin[3] = {0, 0, 0}, magCalMax[3] = {0, 0, 0};
static float    magCalLastAngle = 0.0f;
static bool     magCalHaveLast = false;
static bool     magCalOverflowSeen = false;
static const char *magCalError = "";
static uint32_t rxDataCount = 0;
static uint32_t rxTelemetryCount = 0;
static uint32_t rxDropCount = 0;
static uint32_t rxErrorCount = 0;
static uint32_t ackTxCount = 0;

// Per-minute RX summary window (see RX_SUMMARY_MS).
static uint32_t nextRxSummaryMs = 0;
static uint32_t rxWinData = 0, rxWinTelem = 0, rxWinDrop = 0, rxWinErr = 0;
static uint32_t rxWinAck = 0;
static int      rxWinLastErr = 0;
static uint16_t rxWinFirstSeq = 0, rxWinLastSeq = 0;
static bool     rxWinHaveSeq = false;
static float    rxWinRssiMin = 0, rxWinRssiMax = 0, rxWinSnrMin = 0, rxWinSnrMax = 0;
static double   rxWinRssiSum = 0, rxWinSnrSum = 0;

WebServer httpServer(80);

// Interrupt-driven LoRa RX: DIO1 fires on RxDone; the ISR only sets a flag so
// the main loop never blocks in receive() and the web server stays responsive.
volatile bool rxDoneFlag = false;
void IRAM_ATTR onLoRaDio1() { rxDoneFlag = true; }
#endif

#if defined(ROLE_SERVER)
// 收到的每一種封包都要通過同一組檢查：magic / version / networkId / 收件人 /
// msgType / payload 長度 / 白名單。抄成兩份的話，之後加 MSG_HELLO 就會有第三份，
// 而漏掉其中一項檢查是不會有任何徵兆的。
static bool validateHeader(const uint8_t *buf, size_t n, uint8_t wantMsgType,
                           size_t wantPayloadLen, PacketHeader &hdr) {
  if (n < sizeof(PacketHeader)) return false;
  memcpy(&hdr, buf, sizeof(hdr));

  // NETWORK_ID 才是權威的群組判斷（sync word 只是 PHY 層的粗篩）。
  if (hdr.magic != PROTO_MAGIC || hdr.version != PROTO_VERSION) return false;
  if (hdr.networkId != NETWORK_ID) return false;
  if (hdr.dstId != SERVER_ID && hdr.dstId != ID_BROADCAST) return false;
  if (hdr.msgType != wantMsgType || hdr.payloadLen != wantPayloadLen) return false;
  if (n < sizeof(PacketHeader) + hdr.payloadLen + MAC_LEN) return false;
  if (!isClientAllowed(hdr.srcId)) return false;  // 不在白名單

  // NOTE (stage 2): verify the trailing MAC_LEN-byte HMAC over header+payload here.
  return true;
}

static bool parseDataPacket(const uint8_t *buf, size_t n, DecodedData &out) {
  PacketHeader hdr;
  if (!validateHeader(buf, n, MSG_DATA, sizeof(PositionPayload), hdr)) return false;

  PositionPayload pos;
  memcpy(&pos, buf + sizeof(PacketHeader), sizeof(pos));

  out.srcId       = hdr.srcId;
  out.seq         = hdr.seq;
  out.fix         = pos.fix;
  out.lat         = pos.latE7 / 1e7;
  out.lon         = pos.lonE7 / 1e7;
  out.speedCmS    = pos.speedCmS;
  out.courseDeg10 = pos.courseDeg10;
  out.accelCmS2   = pos.accelCmS2;
  out.satellites  = pos.satellites;
  out.hdop10      = pos.hdop10;
  return true;
}

static bool parseTelemetryPacket(const uint8_t *buf, size_t n, DecodedTelemetry &out) {
  PacketHeader hdr;
  if (!validateHeader(buf, n, MSG_TELEMETRY, sizeof(TelemetryPayload), hdr)) return false;

  TelemetryPayload tel;
  memcpy(&tel, buf + sizeof(PacketHeader), sizeof(tel));
  out.srcId       = hdr.srcId;
  out.batteryMv   = tel.batteryMv;
  out.tempC       = tel.tempC;
  out.humidityPct = tel.humidityPct;
  return true;
}
#endif

// 純數學的實作在 include/geo_math.h（有 native 測試）；這裡只取別名，
// 讓下面幾百個呼叫點不必改寫。
using geo::normalize360;
using geo::angleDiff;
using geo::computeBearing;
using geo::equirectDistanceM;
using geo::fitCircle;
using geo::batteryPercent;
using geo::SigLevel;
using geo::SIG_GOOD;
using geo::SIG_OK;
using geo::SIG_BAD;
using geo::SIG_MISS;
using geo::sig4Text;
using geo::gpsSignal;
using geo::loraSignal;

static uint16_t readBatteryMilliVolts() {
  if (!pmuOnline) {
    return 0;
  }

  if (!pmu.isBatteryConnect()) {
    return 0;
  }

  return pmu.getBattVoltage();
}

// True when external (USB / Type-C) power is present — board runs "plugged in".
static bool batteryCharging() {
  return pmuOnline && pmu.isVbusIn();
}

static bool initPmu() {
  PMUWire.begin(PMU_SDA_PIN, PMU_SCL_PIN);
  if (!pmu.begin(PMUWire, AXP2101_SLAVE_ADDRESS, PMU_SDA_PIN, PMU_SCL_PIN)) {
    Log.println(F("[PMU] AXP2101 init failed"));
    return false;
  }

  pmu.enableBattDetection();
  pmu.enableVbusVoltageMeasure();
  pmu.enableBattVoltageMeasure();
  pmu.enableSystemVoltageMeasure();

  // GPS rail (ALDO4) is needed by both roles.
  pmu.setALDO4Voltage(3300);
  pmu.enableALDO4();

  Log.println(F("[PMU] AXP2101 init ok"));
  return true;
}

static void serviceGps() {
  while (GPSSerial.available() > 0) {
    gps.encode(GPSSerial.read());
  }
}

// 「現在真的有定位」。見 GPS_FIX_MAX_AGE_MS —— isValid() 單獨用是不夠的。
static bool gpsFixFresh() {
  return gps.location.isValid() && gps.location.age() < GPS_FIX_MAX_AGE_MS;
}

static bool initEnvSensor() {
  // BME280 is common and simple; try both default addresses.
  if (envSensor.begin(0x76, &Wire) || envSensor.begin(0x77, &Wire)) {
    Log.println(F("[ENV] BME280 init ok"));
    return true;
  }
  Log.println(F("[ENV] BME280 not found (telemetry stays N/A)"));
  return false;
}

static void sampleEnvSensor() {
  if (!envSensorOnline) {
    cachedTempC10 = INT16_MIN;
    cachedHumidityPct = 0xFF;
    return;
  }
  float t = envSensor.readTemperature();
  float h = envSensor.readHumidity();
  if (isnan(t) || isnan(h)) {
    cachedTempC10 = INT16_MIN;
    cachedHumidityPct = 0xFF;
    return;
  }
  cachedTempC10 = static_cast<int16_t>(lround(t * 10.0f));
  float hc = constrain(h, 0.0f, 100.0f);
  cachedHumidityPct = static_cast<uint8_t>(lround(hc));
#if defined(ROLE_SERVER)
  // 第一筆有效讀數當成基準，之後用「上升幅度」而不是絕對值判斷受潮（見 alerts.h）。
  if (serverHumBaselinePct < 0) serverHumBaselinePct = (int)cachedHumidityPct;
#endif
}

// 8x8 lightning icon (LSB-first rows for U8g2 drawXBMP), drawn when the board
// is on external/USB power.
static const uint8_t ICON_BOLT_8[] = {0x38,0x0C,0x06,0x1F,0x1C,0x0C,0x06,0x03};

#if defined(ROLE_SERVER)
static SigLevel serverGpsState() {
  bool  fix  = gpsFixFresh();
  int   sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
  float hdop = gps.hdop.isValid() ? gps.hdop.hdop() : 99.9f;
  return gpsSignal(fix, sats, hdop);
}

static void saveWhitelistToNvs() {
  String packed;
  for (size_t i = 0; i < clientWhitelistCount; i++) {
    if (i > 0) packed += ',';
    char hex[5];
    snprintf(hex, sizeof(hex), "%04X", clientWhitelist[i]);
    packed += hex;
  }
  prefs.putString("wl", packed);
}

static void loadWhitelistFromNvs() {
  String packed = prefs.getString("wl", "");
  clientWhitelistCount = 0;
  if (packed.length() == 0) {
    for (uint16_t id : DEFAULT_WHITELIST) {
      if (clientWhitelistCount < WHITELIST_MAX) {
        clientWhitelist[clientWhitelistCount++] = id;
      }
    }
    return;
  }
  int start = 0;
  while (start < packed.length() && clientWhitelistCount < WHITELIST_MAX) {
    int comma = packed.indexOf(',', start);
    if (comma < 0) comma = packed.length();
    String token = packed.substring(start, comma);
    if (token.length() > 0) {
      clientWhitelist[clientWhitelistCount++] =
          static_cast<uint16_t>(strtoul(token.c_str(), nullptr, 16));
    }
    start = comma + 1;
  }
}

static void saveMountOffsetToNvs() {
  prefs.putFloat("mountoff", mountOffsetDeg);
  prefs.putFloat("mounthd", mountCalHeadingDeg);
  prefs.putBool("mountcal", mountCalibrated);
  prefs.putUChar("mountver", SERVO_CAL_VERSION);
}

static void loadServerSettings() {
  prefs.begin("shorespotter", false);
  loadWhitelistFromNvs();
  if (prefs.getUChar("mountver", 0) == SERVO_CAL_VERSION) {
    mountOffsetDeg = prefs.getFloat("mountoff", 0.0f);
    mountCalHeadingDeg = prefs.getFloat("mounthd", -1.0f);
    mountCalibrated = prefs.getBool("mountcal", false);
  } else {
    // Older offsets used the opposite servo direction and are not compatible.
    mountOffsetDeg = 0.0f;
    mountCalibrated = false;
  }

  if (prefs.getUChar("magver", 0) == MAG_CAL_VERSION) {
    magAxisA = prefs.getUChar("magaxa", 0);
    magAxisB = prefs.getUChar("magaxb", 1);
    if (magAxisA > 2 || magAxisB > 2 || magAxisA == magAxisB) {
      magAxisA = 0;  // corrupt pair: fall back to the flat-board plane
      magAxisB = 1;
    }
    magOffsetA = prefs.getFloat("magoa", 0.0f);
    magOffsetB = prefs.getFloat("magob", 0.0f);
    magCalibrated = prefs.getBool("magcal", false);
    magCalResidualDeg = prefs.getFloat("magres", -1.0f);
    magCalEllipseDeg = prefs.getFloat("magell", -1.0f);
    magCalScatterDeg = prefs.getFloat("magsct", -1.0f);
    magCalFieldGauss = prefs.getFloat("magfld", -1.0f);
    if (magCalibrated) magCalState = MAGCAL_DONE;
  }
}

static size_t buildAckPacket(uint8_t *buf, uint16_t dstId, uint16_t ackSeq,
                             float rssi, float snr) {
  PacketHeader hdr{};
  hdr.magic = PROTO_MAGIC;
  hdr.version = PROTO_VERSION;
  hdr.networkId = NETWORK_ID;
  hdr.srcId = SERVER_ID;
  hdr.dstId = dstId;
  hdr.msgType = MSG_ACK;
  hdr.seq = txSeq++;
  hdr.payloadLen = sizeof(AckPayload);

  AckPayload ack{};
  ack.ackSeq = ackSeq;
  ack.rssiDbm10 = static_cast<int16_t>(lround(rssi * 10.0f));
  ack.snrDb10 = static_cast<int8_t>(lround(snr * 10.0f));

  size_t off = 0;
  memcpy(buf + off, &hdr, sizeof(hdr)); off += sizeof(hdr);
  memcpy(buf + off, &ack, sizeof(ack)); off += sizeof(ack);
  memset(buf + off, 0, MAC_LEN); off += MAC_LEN;
  return off;
}
#endif

#if defined(ROLE_CLIENT)
static bool parseAckPacket(const uint8_t *buf, size_t n, AckPayload &out) {
  if (n < sizeof(PacketHeader) + sizeof(AckPayload) + MAC_LEN) return false;
  PacketHeader hdr;
  memcpy(&hdr, buf, sizeof(hdr));
  if (hdr.magic != PROTO_MAGIC || hdr.version != PROTO_VERSION) return false;
  if (hdr.networkId != NETWORK_ID) return false;
  if (hdr.msgType != MSG_ACK) return false;
  if (hdr.srcId != SERVER_ID || hdr.dstId != nodeId) return false;
  if (hdr.payloadLen != sizeof(AckPayload)) return false;
  memcpy(&out, buf + sizeof(PacketHeader), sizeof(out));
  return true;
}

static void saveClientSettings() {
  prefs.putChar("txpwr", currentTxPowerDbm);
  prefs.putBool("atpc", atpcEnabled);
}

static void loadClientSettings() {
  prefs.begin("shorespt_client", false);
  int8_t p = prefs.getChar("txpwr", TX_POWER_DBM);
  currentTxPowerDbm = constrain(p, TX_POWER_MIN_DBM, TX_POWER_MAX_DBM);
  atpcEnabled = prefs.getBool("atpc", true);
}

static void applyTxPower(int8_t pwrDbm) {
  int8_t target = constrain(pwrDbm, TX_POWER_MIN_DBM, TX_POWER_MAX_DBM);
  if (target == currentTxPowerDbm) return;
  // SetPaConfig / SetTxParams are configuration commands: drop out of the
  // continuous RX armed by loop() before issuing them, then re-arm.
  radio.standby();
  int st = radio.setOutputPower(target);
  clientRxFlag = false;
  radio.startReceive();
  if (st == RADIOLIB_ERR_NONE) {
    currentTxPowerDbm = target;
    saveClientSettings();
    Log.print(F("[CLIENT] TX power set to "));
    Log.print(currentTxPowerDbm);
    Log.println(F(" dBm"));
  }
}

static void evaluateAtpc() {
  if (!atpcEnabled) return;
  if (millis() < nextAtpcEvalMs) return;
  nextAtpcEvalMs = millis() + ATPC_EVAL_MS;

  // No ACK for a while: push one step up.
  if (millis() - lastAckRxMs > ATPC_NO_ACK_MS) {
    applyTxPower(currentTxPowerDbm + 1);
    return;
  }

  // Strong link -> step down, weak link -> step up.
  // lastAckRssiDbm10 is negative dBm * 10.
  if (lastAckRssiDbm10 > -700 && lastAckSnrDb10 > 80) {
    applyTxPower(currentTxPowerDbm - 1);
  } else if (lastAckRssiDbm10 < -980 || lastAckSnrDb10 < 20) {
    applyTxPower(currentTxPowerDbm + 1);
  }
}
#endif

#if defined(ROLE_CLIENT)
static size_t buildDataPacket(uint8_t *buf) {
  PacketHeader hdr{};
  hdr.magic = PROTO_MAGIC;
  hdr.version = PROTO_VERSION;
  hdr.networkId = NETWORK_ID;
  hdr.srcId = nodeId;
  hdr.dstId = SERVER_ID;
  hdr.msgType = MSG_DATA;
  hdr.seq = txSeq++;
  hdr.payloadLen = sizeof(PositionPayload);

  PositionPayload pos{};
  pos.fix = gpsFixFresh() ? 1 : 0;
  if (pos.fix) {
    pos.latE7 = static_cast<int32_t>(lround(gps.location.lat() * 1e7));
    pos.lonE7 = static_cast<int32_t>(lround(gps.location.lng() * 1e7));
  }
  // Velocity vector from GPS (speed in cm/s, course in 0.1° units)
  if (gps.speed.isValid()) {
    uint16_t curSpeedCmS = static_cast<uint16_t>(gps.speed.mps() * 100.0f);
    pos.speedCmS = curSpeedCmS;
    uint32_t nowMs = millis();
    if (prevSpeedMs > 0) {
      float dt = (nowMs - prevSpeedMs) / 1000.0f;
      if (dt > 0.05f) {
        int16_t rawAccel = static_cast<int16_t>(
            (int32_t(curSpeedCmS) - int32_t(prevSpeedCmS)) / dt);
        smoothAccelCmS2 = static_cast<int16_t>(smoothAccelCmS2 * 0.8f + rawAccel * 0.2f);
      }
    }
    prevSpeedCmS = curSpeedCmS;
    prevSpeedMs  = nowMs;
  }
  pos.accelCmS2 = smoothAccelCmS2;
  if (gps.course.isValid()) {
    pos.courseDeg10 = static_cast<uint16_t>(gps.course.deg() * 10.0f);
  }
  if (gps.satellites.isValid()) {
    uint32_t sv = gps.satellites.value();
    pos.satellites = (sv > 254) ? 254 : static_cast<uint8_t>(sv);
  } else {
    pos.satellites = 0xFF;
  }
  if (gps.hdop.isValid()) {
    long hv = lround(gps.hdop.hdop() * 10.0);
    if (hv < 0) hv = 0;
    if (hv > 254) hv = 254;
    pos.hdop10 = static_cast<uint8_t>(hv);
  } else {
    pos.hdop10 = 0xFF;
  }

  size_t off = 0;
  memcpy(buf + off, &hdr, sizeof(hdr));
  off += sizeof(hdr);
  memcpy(buf + off, &pos, sizeof(pos));
  off += sizeof(pos);
  memset(buf + off, 0, MAC_LEN);  // MAC reserved (stage 2)
  off += MAC_LEN;
  return off;
}

static size_t buildTelemetryPacket(uint8_t *buf) {
  PacketHeader hdr{};
  hdr.magic      = PROTO_MAGIC;
  hdr.version    = PROTO_VERSION;
  hdr.networkId  = NETWORK_ID;
  hdr.srcId      = nodeId;
  hdr.dstId      = SERVER_ID;
  hdr.msgType    = MSG_TELEMETRY;
  hdr.seq        = txSeq++;
  hdr.payloadLen = sizeof(TelemetryPayload);

  TelemetryPayload tel{};
  tel.batteryMv   = cachedBatteryMv;
  tel.tempC       = (cachedTempC10 == INT16_MIN)
                        ? INT8_MIN
                        : (int8_t)constrain((long)lround(cachedTempC10 / 10.0),
                                            -127L, 127L);
  tel.humidityPct = cachedHumidityPct;

  size_t off = 0;
  memcpy(buf + off, &hdr, sizeof(hdr));  off += sizeof(hdr);
  memcpy(buf + off, &tel, sizeof(tel));  off += sizeof(tel);
  memset(buf + off, 0, MAC_LEN);         off += MAC_LEN;
  return off;
}
#endif

static bool initRadio() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  int state = radio.begin(RF_FREQUENCY, RF_BW, RF_SF, RF_CR, RF_SYNC_WORD, TX_POWER_DBM);
  if (state != RADIOLIB_ERR_NONE) {
    Log.print(F("[LoRa] init failed, code="));
    Log.println(state);
    return false;
  }
  Log.println(F("[LoRa] init ok"));
  return true;
}

// Derive the telemetry quiet slot from the radio's actual time-on-air, and log
// the airtime budget. Asking the radio beats keeping hand-computed millisecond
// constants in sync with RF_SF / RF_CR / packet sizes — get that wrong and
// telemetry either collides forever or never finds a slot at all, both silently.
// Call after initRadio(), which is what configures SF/CR/BW.
static void computeAirtimeBudget() {
  uint32_t dataMs = radio.getTimeOnAir(DATA_PACKET_LEN) / 1000;
  uint32_t ackMs = radio.getTimeOnAir(ACK_PACKET_LEN) / 1000;
  uint32_t telMs = radio.getTimeOnAir(TELEMETRY_PACKET_LEN) / 1000;

  // Worst case: assume this cycle is one of the ACKed ones.
  telemetrySlotMinMs = dataMs + ackMs + TELEMETRY_SLOT_GUARD_MS;
  telemetrySlotMaxMs = (SEND_INTERVAL_MS > telMs + TELEMETRY_SLOT_GUARD_MS)
                           ? SEND_INTERVAL_MS - telMs - TELEMETRY_SLOT_GUARD_MS
                           : 0;

  Log.print(F("[LoRa] SF"));
  Log.print(RF_SF);
  Log.print(F(" CR4/"));
  Log.print(RF_CR);
  Log.print(F(" BW"));
  Log.print(RF_BW, 0);
  Log.print(F("k | airtime data="));
  Log.print(dataMs);
  Log.print(F("ms ack="));
  Log.print(ackMs);
  Log.print(F("ms(1/"));
  Log.print(ACK_EVERY_N);
  Log.print(F(") tel="));
  Log.print(telMs);
  Log.print(F("ms | duty="));
  Log.print((dataMs + ackMs / ACK_EVERY_N) * 100 / SEND_INTERVAL_MS);
  Log.println('%');

  if (telemetrySlotMaxMs <= telemetrySlotMinMs) {
    // Airtime no longer fits inside one send interval. Degrade to "any time
    // after the ACK should be done" rather than stalling telemetry silently.
    telemetrySlotMinMs = dataMs + ackMs;
    telemetrySlotMaxMs = SEND_INTERVAL_MS;
    Log.println(F("[LoRa] WARNING: airtime exceeds SEND_INTERVAL_MS; "
                     "telemetry may collide with position packets"));
  }
  Log.print(F("[LoRa] telemetry slot = "));
  Log.print(telemetrySlotMinMs);
  Log.print(F(".."));
  Log.print(telemetrySlotMaxMs);
  Log.println(F(" ms after position TX"));
}

// --- 無線電回復 ------------------------------------------------------------
// SX1262 若因為 SPI 干擾或狀態機卡住而停止工作，原本兩端都只會印一行 log 然後
// 安靜地永遠壞下去。CLIENT 平時螢幕是關的，沒有任何外部徵兆；SERVER 則是站在
// 沙灘上的人完全不知道為什麼鏡頭不動了。這裡在連續失敗到一定次數後重新初始化。
constexpr uint8_t RADIO_TX_FAIL_LIMIT = 5;    // client：連續 5 秒送不出去
constexpr uint16_t RADIO_RX_ERR_LIMIT = 30;   // server：連續 30 次讀取錯誤
constexpr uint32_t RADIO_RECOVER_MIN_MS = 30000;  // 兩次重建之間的最短間隔

static uint16_t radioFailStreak = 0;
static uint32_t lastRadioRecoverMs = 0;
static uint32_t radioRecoverCount = 0;

// 重新初始化無線電並回到原本的收送狀態。回傳是否成功。
static bool recoverRadio() {
  uint32_t now = millis();
  // 重建失敗時 streak 會繼續累加，沒有這個間隔就會變成每個 loop 都重建一次。
  if (lastRadioRecoverMs != 0 && (now - lastRadioRecoverMs) < RADIO_RECOVER_MIN_MS) {
    return false;
  }
  lastRadioRecoverMs = now;
  radioRecoverCount++;
  Log.print(F("[LoRa] radio unresponsive -> re-init (attempt #"));
  Log.print(radioRecoverCount);
  Log.println(')');

  if (!initRadio()) {
    Log.println(F("[LoRa] ERROR: re-init failed; will retry"));
    return false;
  }
#if defined(ROLE_CLIENT)
  // initRadio() 會把功率設回 TX_POWER_DBM，要把 ATPC 收斂到的值放回去。
  radio.setOutputPower(currentTxPowerDbm);
  radio.setDio1Action(onClientDio1);
  clientRxFlag = false;
#else
  radio.setDio1Action(onLoRaDio1);
  rxDoneFlag = false;
#endif
  radio.startReceive();
  radioFailStreak = 0;
  Log.println(F("[LoRa] radio re-init ok"));
  return true;
}

#if defined(ROLE_SERVER)
static const char *trackModeStr(TrackMode m) {
  switch (m) {
    case MODE_TRACKING: return "tracking";
    case MODE_PAUSED:   return "paused";
    case MODE_MANUAL:   return "manual";
    default:            return "idle";
  }
}

static bool servoWriteMicros(int us) {
  if (!servoPwmReady) return false;
  us = constrain(us, SERVO_MIN_US, SERVO_MAX_US);
  // Convert the requested high pulse directly to a duty value.  This remains
  // accurate at the GXServo's 333 Hz refresh rate (period ~= 3003 us).
  uint32_t duty = (uint32_t)(((uint64_t)us * SERVO_PWM_HZ *
                              SERVO_PWM_MAX_DUTY + 500000ULL) /
                             1000000ULL);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!ledcWrite(SERVO_PIN, duty)) {
    servoPwmReady = false;
    return false;
  }
  return true;
#else
  ledcWrite(SERVO_LEDC_CH, duty);
  return true;
#endif
}

static bool setServoAngle(float deg) {
  deg = constrain(deg, 0.0f, 180.0f);
  int us = SERVO_MIN_US +
           (int)lroundf(deg / 180.0f * (SERVO_MAX_US - SERVO_MIN_US));
  if (!servoWriteMicros(us)) return false;
  servoAngleDeg = deg;
  return true;
}

static bool initServo() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  servoPwmReady = ledcAttach(SERVO_PIN, SERVO_PWM_HZ, SERVO_PWM_RES_BITS);
#else
  servoPwmReady = ledcSetup(SERVO_LEDC_CH, SERVO_PWM_HZ,
                            SERVO_PWM_RES_BITS) > 0;
  if (servoPwmReady) ledcAttachPin(SERVO_PIN, SERVO_LEDC_CH);
#endif
  if (!servoPwmReady) {
    Log.print(F("[SERVO] ERROR: LEDC attach failed on IO"));
    Log.println(SERVO_PIN);
    return false;
  }
  if (!setServoAngle(servoAngleDeg)) {  // centre on boot
    Log.println(F("[SERVO] ERROR: initial PWM write failed"));
    servoPwmReady = false;
    return false;
  }
  Log.print(F("[SERVO] LEDC ready on IO"));
  Log.print(SERVO_PIN);
  Log.print(F(" at "));
  Log.print(SERVO_PWM_HZ);
  Log.print(F(" Hz/"));
  Log.print(SERVO_PWM_RES_BITS);
  Log.print(F(" bit, centre "));
  Log.print(servoAngleDeg, 0);
  Log.println(F(" deg"));
  return true;
}

static void saveMagCalToNvs() {
  prefs.putUChar("magaxa", magAxisA);
  prefs.putUChar("magaxb", magAxisB);
  prefs.putFloat("magoa", magOffsetA);
  prefs.putFloat("magob", magOffsetB);
  prefs.putBool("magcal", magCalibrated);
  prefs.putFloat("magres", magCalResidualDeg);
  prefs.putFloat("magell", magCalEllipseDeg);
  prefs.putFloat("magsct", magCalScatterDeg);
  prefs.putFloat("magfld", magCalFieldGauss);
  prefs.putUChar("magver", MAG_CAL_VERSION);
}

static void startMagCalibration() {
  magCalState = MAGCAL_COLLECTING;
  magCalCount = 0;
  magCalBinMask = 0;
  magCalHaveLast = false;
  magCalOverflowSeen = false;
  magCalStartMs = millis();
  magCalError = "";
  Log.println(F("[MAGCAL] started — turn the whole station through one slow "
                   "full horizontal circle"));
}

static uint8_t magCalBinCount() {
  uint8_t bits = 0;
  for (uint8_t i = 0; i < MAG_CAL_BINS; i++) {
    if (magCalBinMask & (1ULL << i)) bits++;
  }
  return bits;
}

static uint8_t magCalCoveragePct() {
  return (uint8_t)((uint16_t)magCalBinCount() * 100 / MAG_CAL_BINS);
}

static const char *magAxisName(uint8_t a) {
  return a == 0 ? "X" : (a == 1 ? "Y" : "Z");
}

// The axis that barely moves through a horizontal turn is the vertical one, so
// the other two are the horizontal pair. Cyclic order ((k+1)%3, (k+2)%3) gives
// axisA x axisB = +axis k, i.e. it assumes k points up; finishMagCalibration
// swaps the pair if the locus says it points down instead.
static void magCalPickPlane() {
  uint8_t k = 0;
  float smallest = magCalMax[0] - magCalMin[0];
  for (uint8_t i = 1; i < 3; i++) {
    float span = magCalMax[i] - magCalMin[i];
    if (span < smallest) {
      smallest = span;
      k = i;
    }
  }
  magCalAxisA = (uint8_t)((k + 1) % 3);
  magCalAxisB = (uint8_t)((k + 2) % 3);
}

static void finishMagCalibration() {
  magCalPickPlane();
  float cx, cy, r;
  if (!fitCircle(&magCalRaw[0][magCalAxisA], &magCalRaw[0][magCalAxisB], 3,
                 magCalCount, cx, cy, r) ||
      r <= 0.0f) {
    magCalState = MAGCAL_FAILED;
    magCalError = "circle fit failed";
    Log.println(F("[MAGCAL] FAILED: circle fit did not converge"));
    return;
  }

  // RMS distance from the fitted circle, expressed as the heading error it
  // implies. A clean mount lands under ~1 deg; a few degrees means soft iron
  // (the servo's steel gears) and the answer is to move the board, not to fit
  // a fancier model.
  // Handedness. The operator is asked to turn clockwise seen from above, which
  // makes the angle round the fitted centre accumulate positive when the pair is
  // the right way round — the same convention as the old flat-board
  // atan2(By, Bx), so a stored mount_offset keeps its meaning. If it accumulated
  // negative, the vertical axis runs the other way through the board: swapping
  // the pair mirrors the angle and exchanges the fitted centre's coordinates.
  double sweep = 0;
  for (size_t i = 1; i < magCalCount; i++) {
    float a0 = magCalRaw[i - 1][magCalAxisA] - cx;
    float b0 = magCalRaw[i - 1][magCalAxisB] - cy;
    float a1 = magCalRaw[i][magCalAxisA] - cx;
    float b1 = magCalRaw[i][magCalAxisB] - cy;
    sweep += angleDiff(degrees(atan2f(b1, a1)), degrees(atan2f(b0, a0)));
  }
  if (sweep < 0) {
    uint8_t ta = magCalAxisA;
    magCalAxisA = magCalAxisB;
    magCalAxisB = ta;
    float tc = cx;
    cx = cy;
    cy = tc;
  }
  magCalSweepDeg = (float)fabs(sweep);

  // Radial error per sample, then split it. e(theta) ~= A cos2t + B sin2t is the
  // ellipse; the samples are spread evenly round the circle by the 2 deg gate, so
  // the cross term is negligible and each coefficient is one division.
  double sum = 0, sA = 0, sB = 0, sAA = 0, sBB = 0;
  for (size_t i = 0; i < magCalCount; i++) {
    double dx = magCalRaw[i][magCalAxisA] - cx, dy = magCalRaw[i][magCalAxisB] - cy;
    double e = sqrt(dx * dx + dy * dy) - r;
    double t2 = 2.0 * atan2(dy, dx), c2 = cos(t2), s2 = sin(t2);
    sum += e * e;
    sA += e * c2;
    sB += e * s2;
    sAA += c2 * c2;
    sBB += s2 * s2;
  }
  float rms = (float)sqrt(sum / magCalCount);
  double ea = (sAA > 0) ? sA / sAA : 0.0;
  double eb = (sBB > 0) ? sB / sBB : 0.0;
  double amp = sqrt(ea * ea + eb * eb);
  double scatter = 0;
  for (size_t i = 0; i < magCalCount; i++) {
    double dx = magCalRaw[i][magCalAxisA] - cx, dy = magCalRaw[i][magCalAxisB] - cy;
    double e = sqrt(dx * dx + dy * dy) - r;
    double t2 = 2.0 * atan2(dy, dx);
    double res = e - (ea * cos(t2) + eb * sin(t2));
    scatter += res * res;
  }
  // For semi-axes r+amp and r-amp the worst heading error is atan(amp/r) — the
  // number that actually matters, unlike the raw Gauss amplitude.
  magCalEllipseDeg = degrees(atanf((float)(amp / r)));
  magCalScatterDeg = degrees(atanf((float)(sqrt(scatter / magCalCount) / r)));

  magAxisA = magCalAxisA;
  magAxisB = magCalAxisB;
  magOffsetA = cx;
  magOffsetB = cy;
  magCalFieldGauss = r;
  magCalResidualDeg = degrees(atanf(rms / r));
  magCalibrated = true;
  magFiltInit = false;  // re-seed the heading filter with corrected values
  magAvgIdx = 0;        // samples taken with the old offsets are stale now
  magAvgCount = 0;
  magCalState = MAGCAL_DONE;
  saveMagCalToNvs();

  Log.print(F("[MAGCAL] done: horizontal axes="));
  Log.print(magAxisName(magAxisA));
  Log.print(',');
  Log.print(magAxisName(magAxisB));
  Log.print(F(" offset=("));
  Log.print(magOffsetA, 4);
  Log.print(F(", "));
  Log.print(magOffsetB, 4);
  Log.print(F(") G field="));
  Log.print(magCalFieldGauss, 4);
  Log.print(F(" G residual="));
  Log.print(magCalResidualDeg, 2);
  Log.print(F(" deg (ellipse "));
  Log.print(magCalEllipseDeg, 2);
  Log.print(F(" + scatter "));
  Log.print(magCalScatterDeg, 2);
  Log.print(F(") from "));
  Log.print(magCalCount);
  Log.print(F(" samples over "));
  Log.print(magCalSweepDeg, 0);
  Log.println(F(" deg of turn"));
  // Which of the two dominates decides what to do next, so say it outright.
  if (magCalEllipseDeg > 1.0f && magCalEllipseDeg > 2.0f * magCalScatterDeg) {
    Log.println(F("[MAGCAL] locus is elliptical, not noisy: soft iron (servo steel "
                     "gears / ferrous screws) or the board is not square to "
                     "gravity. Turning more smoothly will not help — move the "
                     "board."));
  } else if (magCalScatterDeg > 1.0f && magCalScatterDeg > 2.0f * magCalEllipseDeg) {
    Log.println(F("[MAGCAL] locus is noisy, not elliptical: the board tilted or "
                     "vibrated during the turn (or the servo drew current). Turn "
                     "on the tripod head, not by hand, and keep it level."));
  }
  if (magCalFieldGauss < 0.15f || magCalFieldGauss > 0.75f) {
    Log.println(F("[MAGCAL] WARNING: fitted field is far from Earth's ~0.37 G "
                     "— strong local interference?"));
  }
  // Independent check on the handedness decided above: in the northern
  // hemisphere the field dips downward, so its component along the deduced "up"
  // should be negative. Positive usually means the turn actually went
  // anticlockwise, which leaves every later heading CHANGE with the wrong sign —
  // the camera then corrects the wrong way after a tripod bump, with nothing
  // else to hint at why. Hard iron on the vertical axis can flip this test too,
  // so it only warns: the turn direction wins.
  uint8_t upAxis = (uint8_t)(3 - magAxisA - magAxisB);
  float upSign = (((magAxisA + 1) % 3) == magAxisB) ? 1.0f : -1.0f;
  if (upSign * (magCalMin[upAxis] + magCalMax[upAxis]) * 0.5f > 0.0f) {
    Log.println(F("[MAGCAL] WARNING: that turn looked anticlockwise (or the "
                     "vertical axis is swamped by hard iron) — heading may run "
                     "backwards. Check it: turn the tripod 90 deg clockwise and "
                     "the heading should rise by ~90."));
  }
}

// Feed one RAW (uncalibrated) sample to the collector. Samples are accepted only
// after the vector has swung MAG_CAL_MIN_STEP_DEG, which spreads them evenly
// round the circle and stops a stationary rig from filling the buffer.
static void collectMagCalSample(const float f[3]) {
  for (uint8_t i = 0; i < 3; i++) {
    if (magCalCount == 0) {
      magCalMin[i] = magCalMax[i] = f[i];
    } else {
      if (f[i] < magCalMin[i]) magCalMin[i] = f[i];
      if (f[i] > magCalMax[i]) magCalMax[i] = f[i];
    }
  }
  magCalPickPlane();

  // Bin against the running min/max midpoint rather than the origin: with a
  // large hard-iron offset the origin can fall outside the locus entirely, and
  // then the angle seen from it never sweeps a full 360 deg.
  float refA = (magCalMin[magCalAxisA] + magCalMax[magCalAxisA]) * 0.5f;
  float refB = (magCalMin[magCalAxisB] + magCalMax[magCalAxisB]) * 0.5f;
  float ang = normalize360(degrees(atan2f(f[magCalAxisB] - refB,
                                         f[magCalAxisA] - refA)));

  if (magCalHaveLast && fabsf(angleDiff(ang, magCalLastAngle)) < MAG_CAL_MIN_STEP_DEG) {
    return;
  }
  magCalLastAngle = ang;
  magCalHaveLast = true;

  if (magCalCount >= MAG_CAL_MAX_SAMPLES) {
    // The buffer is full. Dropping new samples here would freeze the coverage
    // mask, and the turn could then never complete — which is exactly what
    // happens to a hand-turned rig: the 2 deg gate accepts backwards movement
    // too, so wobble and backlash burn the budget on ground already covered.
    // Halving instead keeps the whole turn represented, just more coarsely: the
    // effective step doubles (2 -> 4 -> 8 deg), still finer than the 10 deg bins.
    for (size_t i = 0; i * 2 < magCalCount; i++) {
      magCalRaw[i][0] = magCalRaw[i * 2][0];
      magCalRaw[i][1] = magCalRaw[i * 2][1];
      magCalRaw[i][2] = magCalRaw[i * 2][2];
    }
    magCalCount = (magCalCount + 1) / 2;
  }
  magCalRaw[magCalCount][0] = f[0];
  magCalRaw[magCalCount][1] = f[1];
  magCalRaw[magCalCount][2] = f[2];
  magCalCount++;

  // Rebuild the coverage mask from every stored sample rather than OR-ing in the
  // new bin. The first few degrees of movement can point at the wrong plane, and
  // bins left over from that guess would let a partial turn pass as a full one.
  // 180 atan2 per accepted sample is tens of microseconds.
  magCalBinMask = 0;
  for (size_t s = 0; s < magCalCount; s++) {
    float sang = normalize360(degrees(atan2f(magCalRaw[s][magCalAxisB] - refB,
                                            magCalRaw[s][magCalAxisA] - refA)));
    magCalBinMask |= (1ULL << (uint8_t)(sang / (360.0f / MAG_CAL_BINS)));
  }
  if (magCalBinCount() >= MAG_CAL_BINS_REQUIRED) finishMagCalibration();
}

static bool initMag() {
  // 兩種板子版本的位址是互斥的，而且磁力計落在哪個位址就決定了螢幕在哪個位址：
  //   QMC6310U 0x1C -> SH1106 0x3C  ／  QMC6310N 0x3C -> SH1106 0x3D
  // 必須先試 0x1C：在 U 版板子上 0x3C 是螢幕，對它送磁力計的探測寫入毫無意義。
  bool found = false;
  if (mag.begin(Wire, QMC6310U_SLAVE_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN)) {
    oledI2CAddr = OLED_ADDR_DEFAULT;
    found = true;
  } else if (mag.begin(Wire, QMC6310N_SLAVE_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN)) {
    // N 版：磁力計佔了 0x3C，螢幕在 0x3D。沒有這一行的話 u8g2 會把畫面資料
    // 寫進磁力計的暫存器 —— 螢幕全黑，而且不會有任何錯誤訊息。
    oledI2CAddr = OLED_ADDR_ALT;
    found = true;
  }
  if (found) {
    // OSR_8 (was OSR_1) averages 8 samples inside the sensor, cutting the noise
    // floor ~3x for free. That noise lands straight on the servo: heading feeds
    // updateTracking() at 20 Hz and the 120 deg/s slew limit passes anything
    // under 6 deg per tick, so an unfiltered ~0.5 deg jitter is visible shimmer
    // on a telephoto shot.
    // ODR drops 200 -> 50 Hz: we only sample every MAG_SAMPLE_MS (5 Hz), and a
    // lower rate both keeps OSR_8 achievable and is quieter.
    // Range stays at FS_8G (+/-800 uT) rather than the 4x finer FS_2G — the
    // station sits next to a 42 kg servo motor, and saturating is worse than
    // quantising (Earth's field is ~45 uT, so FS_8G is already ~30x finer than
    // the noise floor).
    mag.configMagnetometer(OperationMode::CONTINUOUS_MEASUREMENT,
                           MagFullScaleRange::FS_8G, 50.0f,
                           MagOverSampleRatio::OSR_8, MagDownSampleRatio::DSR_1);
    Log.print(F("[MAG] QMC6310 init ok at 0x"));
    Log.print(oledI2CAddr == OLED_ADDR_DEFAULT ? QMC6310U_SLAVE_ADDRESS
                                               : QMC6310N_SLAVE_ADDRESS, HEX);
    Log.print(F(" -> OLED at 0x"));
    Log.println(oledI2CAddr, HEX);
    return true;
  }
  Log.println(F("[MAG] QMC6310 not found (heading N/A, station assumed fixed)"));
  return false;
}

static void sampleMag() {
  if (!magOnline) {
    magHeadingDeg = -1.0f;
    return;
  }
  MagnetometerData d;
  if (!mag.readData(d)) return;

  if (d.overflow) {
    if (magCalState == MAGCAL_COLLECTING) magCalOverflowSeen = true;
    // Field exceeded full scale — the sample is meaningless. In practice this
    // means the board is mounted too close to the servo motor or to a lead
    // carrying servo current. Hold the last good heading and say so.
    static uint32_t nextMagWarnMs = 0;
    if (millis() >= nextMagWarnMs) {
      nextMagWarnMs = millis() + 10000;
      Log.println(F("[MAG] overflow: field over range — move the board away "
                       "from the servo / power leads"));
    }
    return;
  }

  const float f[3] = {d.magnetic_field.x, d.magnetic_field.y, d.magnetic_field.z};

  // Calibration runs on RAW samples: the whole point is to find the offset that
  // is about to be subtracted below.
  if (magCalState == MAGCAL_COLLECTING) {
    collectMagCalSample(f);
    if (magCalState == MAGCAL_COLLECTING &&
        millis() - magCalStartMs > MAG_CAL_TIMEOUT_MS) {
      magCalState = MAGCAL_FAILED;
      magCalError = magCalOverflowSeen
                        ? "field over range — board is too close to the servo"
                        : (magCalCount < 8 ? "no rotation detected"
                                           : "incomplete turn");
      Log.print(F("[MAGCAL] FAILED: timeout at "));
      Log.print(magCalCoveragePct());
      Log.println(F("% coverage"));
    }
  }

  // Hard-iron correction on the two horizontal axes. The offsets are zero and
  // the pair is X,Y until a calibration has been run, which reproduces the
  // original flat-board, uncorrected behaviour exactly.
  float x = f[magAxisA] - magOffsetA;
  float y = f[magAxisB] - magOffsetB;

  magAvgX[magAvgIdx] = x;
  magAvgY[magAvgIdx] = y;
  magAvgIdx = (uint8_t)((magAvgIdx + 1) % MAG_AVG_WINDOW);
  if (magAvgCount < MAG_AVG_WINDOW) magAvgCount++;

  // Low-pass in vector space, not on the angle: averaging degrees is wrong
  // across the 360/0 wrap. The station is meant to be stationary, so a ~0.5 s
  // time constant costs nothing — it halves the noise reaching the servo and
  // still settles a genuine tripod bump inside ~1.5 s.
  if (!magFiltInit) {
    magFiltX = x;
    magFiltY = y;
    magFiltInit = true;
  } else {
    magFiltX += MAG_FILTER_ALPHA * (x - magFiltX);
    magFiltY += MAG_FILTER_ALPHA * (y - magFiltY);
  }
  if (magFiltX == 0.0f && magFiltY == 0.0f) return;  // degenerate, keep last
  magHeadingDeg = normalize360(degrees(atan2f(magFiltY, magFiltX)));
}

// Heading used in tracking maths; 0 when no magnetometer (station assumed fixed).
static float trackingHeading() {
  return (magOnline && magHeadingDeg >= 0) ? magHeadingDeg : 0.0f;
}

// Heading used when LOCKING a calibration: the mean of the last MAG_AVG_WINDOW
// samples rather than the latest one. Falls back to the live heading when there
// is no history yet (straight after boot or a fresh hard-iron calibration).
static float calibrationHeading(uint8_t *samplesUsed = nullptr) {
  if (samplesUsed) *samplesUsed = magAvgCount;
  if (!magOnline || magAvgCount == 0) return trackingHeading();
  double sx = 0, sy = 0;
  for (uint8_t i = 0; i < magAvgCount; i++) {
    sx += magAvgX[i];
    sy += magAvgY[i];
  }
  if (sx == 0.0 && sy == 0.0) return trackingHeading();
  return normalize360(degrees(atan2(sy, sx)));
}

static bool haveBearingFix() {
  return havePkt && lastData.fix && gpsFixFresh();
}

// Project the last received client position forward along its velocity vector.
//
// Position packets arrive at 1 Hz, so lastData is already up to a second stale
// by the time it is used; without this the camera can only step once per packet
// and freezes completely whenever one is dropped. speedCmS / courseDeg10 are
// already in the payload, so the projection costs nothing on the air.
//
// Constant velocity only: accelCmS2 is a heavily smoothed derivative of GPS
// speed, and squaring it into the projection overshoots badly exactly when it
// matters (a surfer dropping into a wave).
static void predictClientPos(double &lat, double &lon) {
  lat = lastData.lat;
  lon = lastData.lon;
  if (lastData.speedCmS < DR_MIN_SPEED_CMS) return;  // course is noise when idle
  float ageS = (millis() - lastRxMs) / 1000.0f;
  if (ageS <= 0.0f) return;
  // Cap the projection instead of letting it run away when the link drops: the
  // camera coasts for ~2 packets, then holds.
  if (ageS > DR_MAX_AGE_S) ageS = DR_MAX_AGE_S;
  float distM = (lastData.speedCmS / 100.0f) * ageS;
  float courseRad = radians(lastData.courseDeg10 / 10.0f);
  double cosLat = cos(radians(lat));
  lat += (distM * cos(courseRad)) / 111320.0;
  if (fabs(cosLat) > 1e-6) {
    lon += (distM * sin(courseRad)) / (111320.0 * cosLat);
  }
}

// Recompute and command the servo while tracking.
// Driven from loop() every TRACK_UPDATE_MS, not by packet arrival.
static void updateTracking() {
  if (trackMode != MODE_TRACKING || !mountCalibrated || !haveBearingFix()) {
    lastServoStepMs = 0;  // a later resume must not see a huge slew dt
    headingFrozen = false;
    return;
  }

  // Heading freeze (see MAG_FREEZE_DEADBAND_DEG): snapshot on entry and hold,
  // so magnetometer noise never reaches the servo. Only a swing bigger than the
  // deadband counts as the tripod actually having been moved, and re-snapshots
  // to the live value — which resets the error well inside the band, giving
  // hysteresis for free instead of chattering at the threshold.
  float liveHeading = trackingHeading();
  if (!headingFrozen) {
    frozenHeadingDeg = liveHeading;
    headingFrozen = true;
  } else if (fabsf(angleDiff(liveHeading, frozenHeadingDeg)) >
             MAG_FREEZE_DEADBAND_DEG) {
    frozenHeadingDeg = liveHeading;
  }

  double clientLat, clientLon;
  predictClientPos(clientLat, clientLon);
  float bearing = (float)computeBearing(gps.location.lat(), gps.location.lng(),
                                        clientLat, clientLon);
  float target = normalize360(frozenHeadingDeg + mountOffsetDeg - bearing);
  if (target > 270.0f) target -= 360.0f;  // wrap small negatives toward 0
  servoTargetDeg = constrain(target, 0.0f, 180.0f);

  // Rate-limit the pan. A single bad GPS sample, or a resume from a far-off
  // angle, would otherwise whip the camera across at the servo's full ~400°/s.
  uint32_t now = millis();
  float dt = (lastServoStepMs == 0) ? 0.0f : (now - lastServoStepMs) / 1000.0f;
  lastServoStepMs = now;
  if (dt <= 0.0f || dt > 0.5f) dt = TRACK_UPDATE_MS / 1000.0f;
  float maxStep = SERVO_MAX_SLEW_DEG_S * dt;
  float step = constrain(servoTargetDeg - servoAngleDeg, -maxStep, maxStep);

  if (!setServoAngle(servoAngleDeg + step)) {
    trackMode = MODE_PAUSED;
    Log.println(F("[SERVO] ERROR: PWM write failed; tracking paused"));
    return;
  }
  static uint32_t nextTrackLogMs = 0;
  if (millis() >= nextTrackLogMs) {
    nextTrackLogMs = millis() + 3000;
    Log.print(F("[TRACK] bearing="));
    Log.print(bearing, 1);
    Log.print(F(" head="));
    Log.print(frozenHeadingDeg, 1);
    Log.print(F(" off="));
    Log.print(mountOffsetDeg, 1);
    Log.print(F(" servo="));
    Log.print(servoAngleDeg, 1);
    Log.print(F("->"));
    Log.println(servoTargetDeg, 1);
  }
}

// Lock the servo-to-world offset from one statement: "the camera is looking at
// camBearing while the servo reads servoDeg". startTracking() 與地標校正都走這裡，
// 差別只在 camBearing 從哪來（衝浪者的實際方位，或地標的座標）。
static float lockMountOffset(float camBearing, float servoDeg, float &headUsed,
                             uint8_t &headSamples) {
  headUsed = calibrationHeading(&headSamples);
  float off = normalize360(camBearing - headUsed + servoDeg);
  if (off > 180.0f) off -= 360.0f;
  mountOffsetDeg = off;
  mountCalibrated = true;
  mountCalHeadingDeg = headUsed;
  headingFrozen = false;  // re-snapshot against the new offset
  saveMountOffsetToNvs();
  return mountOffsetDeg;
}

// Lock the servo-to-world mounting offset from the current aim, then track.
// Calibration uses the same dead-reckoned position as updateTracking(), so the
// offset does not silently absorb whatever projection drift happened to exist
// at the moment the operator pressed start.
static bool startTracking() {
  if (!haveBearingFix()) return false;
  double clientLat, clientLon;
  predictClientPos(clientLat, clientLon);
  float bearingCal = (float)computeBearing(gps.location.lat(), gps.location.lng(),
                                           clientLat, clientLon);
  uint8_t headSamples = 0;
  float headCal = 0.0f;
  lockMountOffset(bearingCal, servoAngleDeg, headCal, headSamples);
  trackMode = MODE_TRACKING;
  Log.print(F("[TRACK] start: bearing="));
  Log.print(bearingCal, 1);
  Log.print(F(" head="));
  Log.print(headCal, 1);
  Log.print(F("(avg of "));
  Log.print(headSamples);
  Log.print(F(")"));
  Log.print(F(" servo="));
  Log.print(servoAngleDeg, 1);
  Log.print(F(" -> mount_offset="));
  Log.println(mountOffsetDeg, 1);
  updateTracking();
  return servoPwmReady;
}

// One line a minute instead of one per packet: everything you would otherwise
// have to read 60 separate lines to get. Sequence numbers carry the loss rate for
// free, so this also answers "is the link dropping packets" without arithmetic.
static void logRxSummary() {
  if (rxWinData == 0 && rxWinTelem == 0 && rxWinDrop == 0 && rxWinErr == 0) {
    return;  // nothing arrived; the idle log already covers a dead link
  }

  // Sequence span gives the loss count, but a client reboot inside the window
  // restarts seq at 0 and the unsigned span then wraps to ~65000 — which would
  // print as "pkt=59/65478 (0%)" and read as a dead link. Only trust a span that
  // could actually have happened in a minute at 1 Hz.
  uint16_t expected = rxWinHaveSeq ? (uint16_t)(rxWinLastSeq - rxWinFirstSeq + 1) : 0;
  if (expected < rxWinData || expected > 600) expected = 0;
  Log.print(F("[SERVER] RX 60s | pkt="));
  Log.print(rxWinData);
  if (expected > 0) {
    Log.print('/');
    Log.print(expected);
    Log.print(F(" ("));
    Log.print((uint32_t)(rxWinData * 100UL / expected));
    Log.print(F("%)"));
  }
  Log.print(F(" tlm="));
  Log.print(rxWinTelem);
  Log.print(F(" ack="));
  Log.print(rxWinAck);
  Log.print(F(" drop="));
  Log.print(rxWinDrop);
  Log.print(F(" err="));
  Log.print(rxWinErr);
  if (rxWinErr > 0) {
    Log.print(F(" (last code "));
    Log.print(rxWinLastErr);
    Log.print(')');
  }
  if (rxWinData > 0) {
    Log.print(F(" | rssi="));
    Log.print(rxWinRssiMin, 0);
    Log.print('/');
    Log.print((float)(rxWinRssiSum / rxWinData), 1);
    Log.print('/');
    Log.print(rxWinRssiMax, 0);
    Log.print(F(" snr="));
    Log.print(rxWinSnrMin, 1);
    Log.print('/');
    Log.print((float)(rxWinSnrSum / rxWinData), 1);
    Log.print('/');
    Log.print(rxWinSnrMax, 1);
    Log.print(F(" | CLI fix="));
    Log.print(lastData.fix);
    Log.print(F(" sats="));
    Log.print(lastData.satellites != 0xFF ? (int)lastData.satellites : -1);
    Log.print(F(" hdop="));
    if (lastData.hdop10 != 0xFF) Log.print(lastData.hdop10 / 10.0f, 1);
    else Log.print(F("--"));
    Log.print(F(" spd="));
    Log.print(lastData.speedCmS);
    Log.print(F("cm/s"));
  }
  Log.print(F(" | SRV fix="));
  Log.print(gpsFixFresh() ? 1 : 0);
  Log.print(F(" sats="));
  Log.print(gps.satellites.isValid() ? (int)gps.satellites.value() : -1);
  if (lastData.fix && gpsFixFresh()) {
    Log.print(F(" | dist="));
    Log.print((uint32_t)equirectDistanceM(gps.location.lat(), gps.location.lng(),
                                          lastData.lat, lastData.lon));
    Log.print(F("m brg="));
    Log.print((float)computeBearing(gps.location.lat(), gps.location.lng(),
                                    lastData.lat, lastData.lon), 0);
  }
  Log.print(F(" | head="));
  if (magOnline && magHeadingDeg >= 0) Log.print(magHeadingDeg, 1);
  else Log.print(F("N/A"));
  Log.print(F(" servo="));
  Log.print(servoAngleDeg, 1);
  Log.print(' ');
  Log.println(trackModeStr(trackMode));

  rxWinData = rxWinTelem = rxWinDrop = rxWinErr = rxWinAck = 0;
  rxWinHaveSeq = false;
  rxWinRssiSum = rxWinSnrSum = 0;
}

// Upper bound on the aim error the current pose carries, from the elliptical part
// of the magnetometer calibration. For e(theta) = A sin(2theta + phi) the leak is
// e(now) - e(cal) = 2A cos(...) sin(now - cal), so |leak| <= 2A |sin(dTheta)|:
// zero in the calibration pose, worst 90 deg away. -1 when it cannot be known.
static float poseAimErrorDeg(float deltaDeg) {
  if (magCalEllipseDeg < 0.0f) return -1.0f;
  return 2.0f * magCalEllipseDeg * fabsf(sinf(radians(deltaDeg)));
}

static void renderServerDisplay() {
  // Centre label column ("V" / T/H / GPS / BAT) is framed by two vertical lines;
  // Server values sit left of it, client values right of it. The 15 px labels get
  // a 1 px gap to each line; the odd rounding pixel is biased to the right.
  const int leftLineX = 55;
  const int rightLineX = 73;
  const int leftCx = 27;   // centre of the Server (left) region
  const int rightCx = 100; // centre of the Client (right) region
  const int midCx = 64;    // centre of the label column

  SigLevel sGps = serverGpsState();

  // Right region rotates the selected whitelist client every 5 seconds.
  size_t clientCount = clientWhitelistCount > 0 ? clientWhitelistCount : 1;
  size_t clientIdx = (millis() / 5000UL) % clientCount;
  uint16_t selectedId = clientWhitelistCount > 0 ? clientWhitelist[clientIdx] : 0;
  bool selectedOnline = havePkt && (lastData.srcId == selectedId) &&
                        ((millis() - lastRxMs) <= LINK_WARN_MS);
  bool selectedTelemetry = haveTelemetry && (lastTelemetry.srcId == selectedId);

  SigLevel loraState = selectedOnline ? loraSignal(lastRssi, lastSnr) : SIG_MISS;
  SigLevel cGps;
  if (!selectedOnline) {
    cGps = SIG_MISS;
  } else {
    int   csats = (lastData.satellites != 0xFF) ? (int)lastData.satellites : 0;
    float chdop = (lastData.hdop10 != 0xFF) ? lastData.hdop10 / 10.0f : 99.9f;
    cGps = gpsSignal(lastData.fix, csats, chdop);
  }

  // 32 而非 24：GCC 對 "Client %u/%u" 會假設 %u 最多 10 位數（-Wformat-truncation），
  // 實際上 clientWhitelistCount 上限是 WHITELIST_MAX=16，永遠不會截斷。
  char buf[32];
  display.clearBuffer();
  display.setFont(u8g2_font_5x7_tr);

  auto drawAt = [&](int cx, int y, const char* s) {
    display.drawStr(cx - display.getStrWidth(s) / 2, y, s);
  };
  auto drawLeft = [&](int y, const char* s) { drawAt(leftCx, y, s); };
  auto drawRight = [&](int y, const char* s) { drawAt(rightCx, y, s); };
  auto drawMid = [&](int y, const char* s) { drawAt(midCx, y, s); };

  // Two continuous vertical lines frame the label column from the title row down
  // through the BAT row.
  display.drawVLine(leftLineX, 0, 45);
  display.drawVLine(rightLineX, 0, 45);

  // --- Title row ---
  drawLeft(8, "Server");
  drawMid(8, "V");
  if (clientWhitelistCount > 0) {
    snprintf(buf, sizeof(buf), "Client %u/%u", (unsigned)(clientIdx + 1),
             (unsigned)clientWhitelistCount);
  } else {
    snprintf(buf, sizeof(buf), "Client -/-");
  }
  drawRight(8, buf);

  // Header separator under the title row (table look).
  display.drawHLine(0, 11, 128);

  // --- T/H row ---
  if (cachedTempC10 != INT16_MIN && cachedHumidityPct != 0xFF) {
    snprintf(buf, sizeof(buf), "%dC/%u%%", (int)lround(cachedTempC10 / 10.0),
             cachedHumidityPct);
  } else {
    snprintf(buf, sizeof(buf), "--C/--%%");
  }
  drawLeft(19, buf);
  if (selectedTelemetry && lastTelemetry.tempC != INT8_MIN &&
      lastTelemetry.humidityPct != 0xFF) {
    snprintf(buf, sizeof(buf), "%dC/%u%%", (int)lastTelemetry.tempC,
             lastTelemetry.humidityPct);
  } else {
    snprintf(buf, sizeof(buf), "--.-C/--%%");
  }
  drawRight(19, buf);
  drawMid(19, "T/H");

  // --- GPS row ---
  drawLeft(30, sig4Text(sGps));
  drawRight(30, sig4Text(cGps));
  drawMid(30, "GPS");

  // --- BAT row (battery percentage, both sides) ---
  if (cachedBatteryMv > 0) {
    snprintf(buf, sizeof(buf), "%u%%", batteryPercent(cachedBatteryMv));
  } else {
    snprintf(buf, sizeof(buf), "--%%");
  }
  drawLeft(41, buf);
  if (selectedTelemetry && lastTelemetry.batteryMv > 0) {
    snprintf(buf, sizeof(buf), "%u%%", batteryPercent(lastTelemetry.batteryMv));
  } else {
    snprintf(buf, sizeof(buf), "--%%");
  }
  drawRight(41, buf);
  drawMid(41, "BAT");
  if (batteryCharging()) display.drawXBMP(2, 34, 8, 8, ICON_BOLT_8);  // ⚡ on USB

  // --- LoRa link state (right side, below the label column) ---
  snprintf(buf, sizeof(buf), "LoRa:%s", sig4Text(loraState));
  drawAt(96, 52, buf);

  // --- Bottom full-width WiFi status line (not split into halves) ---
  char wifiBuf[64];
  if (WiFi.status() == WL_CONNECTED && !cachedApIp.isEmpty()) {
    snprintf(wifiBuf, sizeof(wifiBuf), "%s", cachedApIp.c_str());
  } else if (millis() < wifiReconnectingUntilMs) {
    snprintf(wifiBuf, sizeof(wifiBuf), "WiFi connecting: %s ...", WIFI_SSID);
  } else {
    uint32_t remain =
        nextWifiRetryMs > millis() ? (nextWifiRetryMs - millis()) / 1000 + 1 : 0;
    snprintf(wifiBuf, sizeof(wifiBuf), "reconnecting to %s in %lus", WIFI_SSID,
             (unsigned long)remain);
  }
  display.drawStr(64 - display.getStrWidth(wifiBuf) / 2, 63, wifiBuf);

  display.sendBuffer();
}
#endif

#if defined(ROLE_SERVER)
// 站體從鎖定 mount_offset 之後被轉了多少，以及那個姿態差隱含的瞄準誤差。
// /api/track、/api/status 與現場提醒共用同一個計算，數字才不會互相矛盾。
static bool poseErrorNow(float &deltaDeg, float &errDeg) {
  if (!(mountCalibrated && mountCalHeadingDeg >= 0.0f && magOnline &&
        magHeadingDeg >= 0.0f)) {
    deltaDeg = 0.0f;
    errDeg = -1.0f;
    return false;
  }
  deltaDeg = angleDiff(magHeadingDeg, mountCalHeadingDeg);
  errDeg = poseAimErrorDeg(deltaDeg);
  return true;
}

// servo / mag 兩個區塊原本在 /api/track 與 /api/status 各寫一份，欄位還不完全
// 一致（前端得靠兩個端點拼一份狀態）。統一成同一份輸出，兩邊就不可能再走岔。
static void appendServoMagJson(String &js) {
  float poseDelta = 0.0f, poseErr = -1.0f;
  bool posePossible = poseErrorNow(poseDelta, poseErr);

  js += F("\"servo\":{\"angle\":");
  js += String(servoAngleDeg, 1);
  js += F(",\"target\":");
  js += String(servoTargetDeg, 1);
  js += F(",\"mode\":\"");
  js += trackModeStr(trackMode);
  js += F("\",\"calibrated\":");
  js += mountCalibrated ? F("true") : F("false");
  js += F(",\"pwm_ok\":");
  js += servoPwmReady ? F("true") : F("false");
  js += F(",\"mount_offset_deg\":");
  js += String(mountOffsetDeg, 1);
  js += F(",\"cal_heading\":");
  js += mountCalHeadingDeg >= 0.0f ? String(mountCalHeadingDeg, 1) : F("null");
  js += F(",\"pose_delta_deg\":");
  js += posePossible ? String(poseDelta, 1) : F("null");
  js += F(",\"pose_err_deg\":");
  js += poseErr >= 0.0f ? String(poseErr, 2) : F("null");
  js += F("},\"mag\":{\"online\":");
  js += magOnline ? F("true") : F("false");
  js += F(",\"heading\":");
  js += (magOnline && magHeadingDeg >= 0) ? String(magHeadingDeg, 1) : F("-1");
  js += F(",\"calibrated\":");
  js += magCalibrated ? F("true") : F("false");
  js += F(",\"residual_deg\":");
  js += magCalResidualDeg >= 0 ? String(magCalResidualDeg, 2) : F("null");
  js += '}';
}

static String buildTrackJson() {
  bool linked = havePkt && ((millis() - lastRxMs) <= LINK_TIMEOUT_MS);
  uint32_t sinceRx = havePkt ? (uint32_t)((millis() - lastRxMs) / 1000) : UINT32_MAX;

  String js;
  js.reserve(900);
  js += F("{\"linked\":");
  js += linked ? F("true") : F("false");
  js += F(",\"bearing\":");
  if (havePkt && lastData.fix && gpsFixFresh()) {
    js += String(computeBearing(gps.location.lat(), gps.location.lng(),
                                lastData.lat, lastData.lon), 1);
  } else {
    js += F("-1");
  }
  js += F(",\"client\":{\"lat\":");
  js += havePkt ? String(lastData.lat, 6) : F("0");
  js += F(",\"lon\":");
  js += havePkt ? String(lastData.lon, 6) : F("0");
  js += F(",\"fix\":");
  js += havePkt ? String(lastData.fix) : F("0");
  js += F(",\"speed_cms\":");
  js += havePkt ? String(lastData.speedCmS) : F("0");
  js += F(",\"course_deg10\":");
  js += havePkt ? String(lastData.courseDeg10) : F("0");
  js += F(",\"accel_cms2\":");
  js += havePkt ? String(lastData.accelCmS2) : F("0");
  js += F(",\"satellites\":");
  js += (havePkt && lastData.satellites != 0xFF) ? String(lastData.satellites)
                                                 : F("-1");
  js += F(",\"hdop\":");
  js += (havePkt && lastData.hdop10 != 0xFF) ? String(lastData.hdop10 / 10.0f, 1)
                                             : F("-1");
  js += F(",\"last_rx_sec\":");
  js += (havePkt && sinceRx != UINT32_MAX) ? String(sinceRx) : F("-1");
  js += F("},\"server\":{\"lat\":");
  js += gpsFixFresh() ? String(gps.location.lat(), 6) : F("0");
  js += F(",\"lon\":");
  js += gpsFixFresh() ? String(gps.location.lng(), 6) : F("0");
  js += F(",\"fix\":");
  js += gpsFixFresh() ? F("1") : F("0");
  js += F(",\"satellites\":");
  js += gps.satellites.isValid() ? String(gps.satellites.value()) : F("-1");
  js += F(",\"hdop\":");
  js += gps.hdop.isValid() ? String(gps.hdop.hdop(), 1) : F("-1");
  js += F(",\"temp_c\":");
  if (cachedTempC10 != INT16_MIN) {
    js += String((int)lround(cachedTempC10 / 10.0));
  } else {
    js += F("null");
  }
  js += F(",\"humidity_pct\":");
  js += cachedHumidityPct != 0xFF ? String(cachedHumidityPct) : F("null");
  js += F(",\"batt_pct\":");
  js += cachedBatteryMv > 0 ? String(batteryPercent(cachedBatteryMv)) : F("-1");
  js += F(",\"charging\":");
  js += batteryCharging() ? F("true") : F("false");
  js += F("},\"telemetry\":{\"batt_mv\":");
  js += haveTelemetry ? String(lastTelemetry.batteryMv) : F("0");
  js += F(",\"temp_c\":");
  if (haveTelemetry && lastTelemetry.tempC != INT8_MIN) {
    js += String((int)lastTelemetry.tempC);
  } else {
    js += F("null");
  }
  js += F(",\"humidity_pct\":");
  js += (haveTelemetry && lastTelemetry.humidityPct != 0xFF)
            ? String(lastTelemetry.humidityPct)
            : F("null");
  js += F(",\"last_rx_sec\":");
  js += haveTelemetry
            ? String((uint32_t)((millis() - lastTelemetryRxMs) / 1000))
            : F("-1");
  js += F("},");
  appendServoMagJson(js);
  js += '}';
  return js;
}

static String buildWhitelistJson() {
  String js = F("{\"whitelist\":[");
  for (size_t i = 0; i < clientWhitelistCount; i++) {
    if (i > 0) js += ',';
    char hex[5];
    snprintf(hex, sizeof(hex), "%04X", clientWhitelist[i]);
    js += '"'; js += hex; js += '"';
  }
  js += F("],\"count\":"); js += String(clientWhitelistCount); js += '}';
  return js;
}

// ---------------------------------------------------------------------------
// 現場提醒（/api/status 的 alerts 欄位，顯示在監控頁最上方的訊息列）
//
// 讀這些字的人站在沙灘上，多半不是工程師，而且手上可能還拿著相機。所以每一則
// 都寫成「發生什麼事 + 現在該做什麼」，不出現 HDOP、dBm、mV 這類名詞；需要數字
// 時給的是他能判斷的量（百分比、秒、度）。
//
// 門檻與分級在 include/alerts.h（有 native 測試），這裡只負責措辭。
// 文字都是編譯期常數且不含引號或反斜線，所以不需要 JSON 跳脫。
// ---------------------------------------------------------------------------
struct PendingAlert {
  alerts::Level level;
  const char *id;
  const char *title;
  String detail;
};
constexpr size_t ALERTS_MAX = 16;

static void appendAlertsJson(String &js) {
  PendingAlert list[ALERTS_MAX];
  size_t n = 0;
  auto add = [&](alerts::Level lv, const char *id, const char *title,
                 const String &detail) {
    if (lv == alerts::NONE || n >= ALERTS_MAX) return;
    list[n].level = lv;
    list[n].id = id;
    list[n].title = title;
    list[n].detail = detail;
    n++;
  };

  // --- 下水端：連線 -------------------------------------------------------
  int32_t sinceRx = havePkt ? (int32_t)((millis() - lastRxMs) / 1000) : -1;
  if (!havePkt) {
    add(alerts::WARN, "client_never", "還沒收到追蹤器的訊號",
        "請確認追蹤器已經開機（長按電源鍵），而且它的 ID 已經加進白名單。");
  } else {
    alerts::Level l = alerts::linkLevel(sinceRx);
    if (l == alerts::ERROR) {
      add(l, "client_link", "和追蹤器失去連線",
          String("已經 ") + sinceRx +
          " 秒沒有收到訊號，鏡頭已經停止追蹤。可能是距離太遠、追蹤器沒電，"
          "或裝置泡在水面下。");
    } else if (l == alerts::WARN) {
      add(l, "client_link", "追蹤器訊號斷斷續續",
          String("已經 ") + sinceRx + " 秒沒有收到訊號，鏡頭暫時停在原地等訊號回來。");
    }
  }

  // --- 下水端：遙測（電量 / 溫濕度，每 30 秒一筆）------------------------
  if (haveTelemetry) {
    int hum = (lastTelemetry.humidityPct != 0xFF) ? (int)lastTelemetry.humidityPct : -1;
    alerts::Level hl = alerts::humidityLevel(hum, clientHumBaselinePct);
    if (hl == alerts::ERROR) {
      add(hl, "client_water", "追蹤器可能已經進水",
          String("防水盒裡的濕度到了 ") + hum +
          "%。請立刻請衝浪者上岸，把裝置擦乾並檢查防水圈有沒有夾到東西。");
    } else if (hl == alerts::WARN) {
      add(hl, "client_water", "追蹤器濕度正在上升",
          String("盒內濕度 ") + hum + "%，比下水前高了 " +
          (hum - clientHumBaselinePct) + " 個百分點，可能正在慢慢滲水。建議上岸檢查防水圈。");
    }

    if (lastTelemetry.batteryMv > 0) {
      int pct = batteryPercent(lastTelemetry.batteryMv);
      alerts::Level bl = alerts::batteryLevel(pct);
      if (bl == alerts::ERROR) {
        add(bl, "client_batt", "追蹤器快沒電了",
            String("只剩 ") + pct + "%，沒電就會自動關機、追蹤中斷。請換一顆電池，或先結束這一輪。");
      } else if (bl == alerts::WARN) {
        add(bl, "client_batt", "追蹤器電量偏低",
            String("剩 ") + pct + "%，還可以再撐一陣子，建議先把備用電池準備好。");
      }
    }

    if (lastTelemetry.tempC != INT8_MIN) {
      int t = lastTelemetry.tempC;
      alerts::Level tl = alerts::temperatureLevel(t, true);
      if (tl == alerts::ERROR) {
        add(tl, "client_temp", "追蹤器過熱",
            String("盒內已經 ") + t + "°C。請移到陰涼處，不要放在太陽直曬的沙灘上，太熱會傷電池。");
      } else if (tl == alerts::WARN) {
        add(tl, "client_temp", "追蹤器溫度偏高",
            String("盒內 ") + t + "°C，建議先收到陰影下。");
      }
    }
  }

  // --- 下水端：衛星 -------------------------------------------------------
  if (havePkt && sinceRx >= 0 && (uint32_t)sinceRx < alerts::kLinkWarnSec) {
    int csats = (lastData.satellites != 0xFF) ? (int)lastData.satellites : 0;
    float chdop = (lastData.hdop10 != 0xFF) ? lastData.hdop10 / 10.0f : 99.9f;
    SigLevel g = gpsSignal(lastData.fix, csats, chdop);
    if (g == SIG_BAD || g == SIG_MISS) {
      add(alerts::WARN, "client_gps", "追蹤器收不到足夠的衛星",
          "鏡頭可能會追到錯的位置。請確認追蹤器沒有被身體、衝浪板或濕毛巾蓋住，"
          "天線那一面要朝上。");
    }
  }

  // --- 岸上站：電量 / 溫濕度 ----------------------------------------------
  if (cachedBatteryMv > 0 && !batteryCharging()) {
    int pct = batteryPercent(cachedBatteryMv);
    alerts::Level bl = alerts::batteryLevel(pct);
    if (bl == alerts::ERROR) {
      add(bl, "srv_batt", "攝影站快沒電了",
          String("只剩 ") + pct + "%，沒電就會自動關機、雲台停止。請接上行動電源。");
    } else if (bl == alerts::WARN) {
      add(bl, "srv_batt", "攝影站電量偏低",
          String("剩 ") + pct + "%，建議現在就接上行動電源，不要等到拍到一半。");
    }
  }

  if (cachedHumidityPct != 0xFF) {
    int hum = cachedHumidityPct;
    if (alerts::humidityLevel(hum, serverHumBaselinePct) != alerts::NONE) {
      add(alerts::WARN, "srv_water", "攝影站可能受潮",
          String("機殼內濕度 ") + hum + "%。請確認沒有被浪打到或淋到雨，必要時先收起來。");
    }
  }

  if (cachedTempC10 != INT16_MIN) {
    int t = (int)lround(cachedTempC10 / 10.0);
    alerts::Level tl = alerts::temperatureLevel(t, true);
    if (tl == alerts::ERROR) {
      add(tl, "srv_temp", "攝影站過熱",
          String("機殼內已經 ") + t + "°C。請幫攝影站遮陽，太熱可能讓它自己關機。");
    } else if (tl == alerts::WARN) {
      add(tl, "srv_temp", "攝影站溫度偏高",
          String("機殼內 ") + t + "°C，建議加個遮陽。");
    }
  }

  if (serverGpsState() == SIG_BAD || serverGpsState() == SIG_MISS) {
    add(alerts::WARN, "srv_gps", "攝影站自己的定位不穩",
        "算出來的方位會有偏差，鏡頭容易追偏。請把攝影站移到天空開闊、沒有建築物"
        "或大樹遮住的地方。");
  }

  // --- 設定與硬體 ---------------------------------------------------------
  if (!servoPwmReady) {
    add(alerts::ERROR, "servo_fault", "雲台沒有反應",
        "鏡頭無法轉動。請檢查雲台的訊號線（IO21）和接地線有沒有鬆脫，然後重新開機。");
  }
  if (!mountCalibrated) {
    add(alerts::WARN, "mount_uncal", "還沒設定鏡頭的方向",
        "自動追蹤還不能用。請到「資訊」分頁，把遠處的地標對準畫面中央後做一次地標校正。");
  }
  if (magOnline && !magCalibrated) {
    add(alerts::WARN, "mag_uncal", "指南針還沒校正",
        "攝影站被碰到之後，鏡頭會往錯的方向修正。請到「資訊」分頁做一次磁力計校正："
        "原地慢慢順時針轉一圈就好。");
  }
  {
    float poseDelta = 0.0f, poseErr = -1.0f;
    if (poseErrorNow(poseDelta, poseErr) &&
        alerts::poseLevel(poseErr) != alerts::NONE) {
      add(alerts::WARN, "pose_moved", "攝影站被轉動過",
          String("現在的朝向和校正時差了 ") + String(fabsf(poseDelta), 0) +
          " 度，鏡頭可能偏掉 " + String(poseErr, 1) +
          " 度。把攝影站轉回原本的方向，或重新做一次地標校正就會歸零。");
    }
  }

  // 嚴重的排前面：現場的人先看到要立刻處理的那幾則。
  js += F("\"alerts\":[");
  bool first = true;
  for (int pass = alerts::ERROR; pass >= alerts::WARN; pass--) {
    for (size_t i = 0; i < n; i++) {
      if ((int)list[i].level != pass) continue;
      if (!first) js += ',';
      first = false;
      js += F("{\"id\":\"");
      js += list[i].id;
      js += F("\",\"level\":\"");
      js += alerts::levelText(list[i].level);
      js += F("\",\"title\":\"");
      js += list[i].title;
      js += F("\",\"detail\":\"");
      js += list[i].detail;
      js += F("\"}");
    }
  }
  js += ']';
}

static String buildStatusJson() {
  String js;
  js.reserve(2048);  // 含 alerts；預留不足只會多幾次 realloc，不會出錯

  // Server GPS quality
  js += F("{\"server_gps\":{\"fix\":");
  js += gpsFixFresh() ? F("1") : F("0");
  js += F(",\"satellites\":");
  js += gps.satellites.isValid() ? String(gps.satellites.value()) : F("-1");
  js += F(",\"hdop\":");
  js += gps.hdop.isValid() ? String(gps.hdop.hdop(), 2) : F("-1");
  js += F("},");

  // LoRa rolling stats
  float rssiSum = 0, snrSum = 0;
  for (size_t i = 0; i < rssiRingCount; i++) { rssiSum += rssiRing[i]; snrSum += snrRing[i]; }
  float rssiAvg = rssiRingCount ? rssiSum / rssiRingCount : 0;
  float snrAvg  = rssiRingCount ? snrSum  / rssiRingCount : 0;
  js += F("\"lora\":{\"rssi\":");
  js += String(lastRssi, 1);
  js += F(",\"snr\":");
  js += String(lastSnr, 1);
  js += F(",\"rssi_avg\":");
  js += rssiRingCount ? String(rssiAvg, 1) : F("null");
  js += F(",\"snr_avg\":");
  js += rssiRingCount ? String(snrAvg, 1) : F("null");
  js += F(",\"pkt_rate\":");
  js += String(cachedPktRate, 2);
  uint32_t rxTotal = rxDataCount + rxTelemetryCount + rxDropCount;
  js += F(",\"rx_data\":");
  js += String(rxDataCount);
  js += F(",\"rx_telemetry\":");
  js += String(rxTelemetryCount);
  js += F(",\"rx_drop\":");
  js += String(rxDropCount);
  js += F(",\"drop_rate\":");
  js += rxTotal ? String((float)rxDropCount / rxTotal, 3) : F("0");
  js += F(",\"ack_tx\":");
  js += String(ackTxCount);
  js += F("},");

  js += F("\"env\":{\"temp_c\":");
  if (cachedTempC10 != INT16_MIN) {
    js += String((int)lround(cachedTempC10 / 10.0));
  } else {
    js += F("null");
  }
  js += F(",\"humidity_pct\":");
  js += cachedHumidityPct != 0xFF ? String(cachedHumidityPct) : F("null");
  js += F("},");

  js += F("\"health\":{\"uptime_s\":");
  js += String((millis() - bootMs) / 1000);
  js += F(",\"heap_free\":");
  js += String(ESP.getFreeHeap());
  js += F(",\"heap_min\":");
  js += String(ESP.getMinFreeHeap());
  js += F(",\"reset_reason\":\"");
  js += String((int)esp_reset_reason());
  js += F("\",\"rx_error\":");
  js += String(rxErrorCount);
  js += F("},");

  // Servo / tracking + magnetometer state
  appendServoMagJson(js);
  js += F(",");
  appendAlertsJson(js);
  js += '}';
  return js;
}

static String buildMagCalJson() {
  const char *st = "idle";
  switch (magCalState) {
    case MAGCAL_COLLECTING: st = "collecting"; break;
    case MAGCAL_DONE:       st = "done"; break;
    case MAGCAL_FAILED:     st = "failed"; break;
    default:                st = "idle"; break;
  }
  String js = F("{\"state\":\"");
  js += st;
  js += F("\",\"online\":");
  js += magOnline ? F("true") : F("false");
  js += F(",\"coverage_pct\":");
  js += String(magCalState == MAGCAL_COLLECTING ? magCalCoveragePct() : 0);
  js += F(",\"samples\":");
  js += String((uint32_t)magCalCount);
  js += F(",\"calibrated\":");
  js += magCalibrated ? F("true") : F("false");
  js += F(",\"residual_deg\":");
  js += magCalResidualDeg >= 0 ? String(magCalResidualDeg, 2) : F("null");
  js += F(",\"field_gauss\":");
  js += magCalFieldGauss >= 0 ? String(magCalFieldGauss, 4) : F("null");
  js += F(",\"ellipse_deg\":");
  js += magCalEllipseDeg >= 0 ? String(magCalEllipseDeg, 2) : F("null");
  js += F(",\"scatter_deg\":");
  js += magCalScatterDeg >= 0 ? String(magCalScatterDeg, 2) : F("null");
  js += F(",\"sweep_deg\":");
  js += String(magCalSweepDeg, 0);
  js += F(",\"axes\":\"");
  js += magAxisName(magAxisA);
  js += ',';
  js += magAxisName(magAxisB);
  js += F("\",\"offset_a\":");
  js += String(magOffsetA, 4);
  js += F(",\"offset_b\":");
  js += String(magOffsetB, 4);
  js += F(",\"heading\":");
  js += (magOnline && magHeadingDeg >= 0) ? String(magHeadingDeg, 1) : F("-1");
  js += F(",\"error\":\"");
  js += magCalError;
  js += F("\"}");
  return js;
}

// Extract the log bytes the caller has not seen yet.
// Offsets are absolute (logTotal), so a caller can poll for deltas; if it has
// fallen behind the 4 KB window — or we rebooted and logTotal went backwards —
// it is snapped to the oldest byte we still hold and told data was dropped.
static String buildLogText(uint32_t from, bool &dropped, uint32_t &next) {
  uint32_t oldest = logWrapped ? (logTotal - LOG_BUF_BYTES) : 0;
  uint32_t start;
  dropped = false;
  if (from > logTotal || from < oldest) {  // behind the window, or we rebooted
    start = oldest;
    dropped = (logTotal > 0);
  } else {
    start = from;
  }
  next = logTotal;

  String out;
  out.reserve(logTotal - start + 8);
  for (uint32_t k = start; k < logTotal; k++) {
    size_t idx = logWrapped ? (logHead + (size_t)(k - oldest)) % LOG_BUF_BYTES
                            : (size_t)k;
    out += logBuf[idx];
  }
  return out;
}

static void initWebServer() {
  // send() 的 const char* 多載會先 `String passStr = content` 把整份 44 KB 複製到
  // heap（arduino-esp32 WebServer.cpp 裡自己的 log_e 就寫著 "Use send_P for long
  // arrays"）。send_P 分塊送出，不做這份複製。
  httpServer.on("/", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "public, max-age=86400");
    httpServer.send_P(200, PSTR("text/html"), WEB_UI_HTML);
  });
  // Standalone log viewer. The 資訊 page opens this in a separate browser tab so
  // watching the log no longer costs you the radar view.
  httpServer.on("/log", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "public, max-age=86400");
    httpServer.send_P(200, PSTR("text/html"), WEB_LOG_HTML);
  });
  // Web app manifest -> "Add to Home screen" launches standalone (no URL bar).
  httpServer.on("/manifest.json", HTTP_GET, []() {
    httpServer.send(200, "application/manifest+json",
                    F("{\"name\":\"Shore Spotter\",\"short_name\":\"Spotter\","
                      "\"start_url\":\"/\",\"scope\":\"/\",\"display\":\"standalone\","
                      "\"orientation\":\"portrait\",\"background_color\":\"#0d1117\","
                      "\"theme_color\":\"#0d1117\",\"icons\":["
                      "{\"src\":\"/icon-192.png\",\"sizes\":\"192x192\",\"type\":\"image/png\"},"
                      "{\"src\":\"/icon-32.png\",\"sizes\":\"32x32\",\"type\":\"image/png\"}]}"));
    // 註：刻意不宣告 purpose:"maskable" —— 紅點靠近圖磚邊緣，Android 的圓形遮罩
    // 會把它切掉。
  });
  httpServer.on("/api/track", HTTP_GET, []() {
    httpServer.send(200, "application/json", buildTrackJson());
  });
  httpServer.on("/api/whitelist", HTTP_GET, []() {
    httpServer.send(200, "application/json", buildWhitelistJson());
  });
  // POST /api/whitelist?action=add|remove|clear&id=XXXX
  httpServer.on("/api/whitelist", HTTP_POST, []() {
    String action = httpServer.arg("action");
    String idStr  = httpServer.arg("id");
    if (action == "add") {
      // strtoul 解析失敗回 0，所以 id=abc 或空字串會安靜地把 0x0000 加進白名單：
      // 佔掉一格、出現在 OLED 的 Client 輪播裡、而且永遠不會有封包配對到它。
      char *endp = nullptr;
      unsigned long parsed = strtoul(idStr.c_str(), &endp, 16);
      if (idStr.length() == 0 || endp == idStr.c_str() || *endp != '\0' ||
          parsed > 0xFFFF || parsed == 0) {
        httpServer.send(400, "application/json",
                        "{\"ok\":false,\"error\":\"id must be 1-4 hex digits\"}");
        return;
      }
      uint16_t newId = (uint16_t)parsed;
      bool found = false;
      for (size_t i = 0; i < clientWhitelistCount; i++) {
        if (clientWhitelist[i] == newId) { found = true; break; }
      }
      if (!found && clientWhitelistCount < WHITELIST_MAX) {
        clientWhitelist[clientWhitelistCount++] = newId;
        saveWhitelistToNvs();
      }
    } else if (action == "remove") {
      uint16_t rmId = (uint16_t)strtoul(idStr.c_str(), nullptr, 16);
      for (size_t i = 0; i < clientWhitelistCount; i++) {
        if (clientWhitelist[i] == rmId) {
          for (size_t j = i; j < clientWhitelistCount - 1; j++) {
            clientWhitelist[j] = clientWhitelist[j + 1];
          }
          clientWhitelistCount--;
          saveWhitelistToNvs();
          break;
        }
      }
    } else if (action == "clear") {
      clientWhitelistCount = 0;
      saveWhitelistToNvs();
    } else {
      httpServer.send(400, "application/json",
                      "{\"ok\":false,\"error\":\"unknown action\"}");
      return;
    }
    httpServer.send(200, "application/json", buildWhitelistJson());
  });
  // GET/POST /api/mag/calibrate — hard-iron calibration.
  // POST starts (or ?action=cancel aborts); GET polls progress.
  httpServer.on("/api/mag/calibrate", HTTP_GET, []() {
    httpServer.send(200, "application/json", buildMagCalJson());
  });
  httpServer.on("/api/mag/calibrate", HTTP_POST, []() {
    if (httpServer.arg("action") == "cancel") {
      magCalState = magCalibrated ? MAGCAL_DONE : MAGCAL_IDLE;
      httpServer.send(200, "application/json", buildMagCalJson());
      return;
    }
    if (!magOnline) {
      httpServer.send(503, "application/json",
                      "{\"ok\":false,\"error\":\"no magnetometer\"}");
      return;
    }
    startMagCalibration();
    httpServer.send(200, "application/json", buildMagCalJson());
  });

  // POST /api/track/calibrate?lat=<deg>&lon=<deg>
  // Lock mount_offset against a landmark of known position instead of against
  // the surfer. Centre the landmark in the viewfinder first — at 400 mm that is
  // a ~0.05 deg sight, an order of magnitude better than aiming at a person in
  // the water, and it needs neither a second person nor the tracker to be
  // present. Only the station's own GPS fix is required.
  //
  // This is the ONLY way to lock mount_offset. Forms that took a bearing typed in
  // by the operator were removed: they cost 2-5 deg of hand-aiming plus the risk
  // of a silent magnetic-vs-true blunder, which is a lot to pay for skipping a
  // 30-second sighting that also happens to need no compass at all.
  httpServer.on("/api/track/calibrate", HTTP_POST, []() {
    if (!httpServer.hasArg("lat") || !httpServer.hasArg("lon")) {
      httpServer.send(400, "application/json",
                      "{\"ok\":false,\"error\":\"missing lat/lon\"}");
      return;
    }
    if (!gpsFixFresh()) {
      httpServer.send(409, "application/json",
                      "{\"ok\":false,\"error\":\"need server GPS fix\"}");
      return;
    }
    double lat = httpServer.arg("lat").toDouble();
    double lon = httpServer.arg("lon").toDouble();
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0 ||
        (lat == 0.0 && lon == 0.0)) {
      httpServer.send(400, "application/json",
                      "{\"ok\":false,\"error\":\"lat/lon out of range\"}");
      return;
    }

    double sLat = gps.location.lat(), sLon = gps.location.lng();
    float bearing = (float)computeBearing(sLat, sLon, lat, lon);
    // Equirectangular distance is plenty here; it only drives a sanity warning.
    double distM = equirectDistanceM(sLat, sLon, lat, lon);

    float headUsed = 0.0f;
    uint8_t headSamples = 0;
    lockMountOffset(bearing, servoAngleDeg, headUsed, headSamples);

    Log.print(F("[TRACK] landmark calibration: bearing="));
    Log.print(bearing, 2);
    Log.print(F(" dist="));
    Log.print(distM, 0);
    Log.print(F("m head="));
    Log.print(headUsed, 2);
    Log.print(F(" (avg of "));
    Log.print(headSamples);
    Log.print(F(") servo="));
    Log.print(servoAngleDeg, 1);
    Log.print(F(" -> mount_offset="));
    Log.println(mountOffsetDeg, 2);

    String js = F("{\"ok\":true,\"method\":\"landmark\",\"bearing\":");
    js += String(bearing, 2);
    js += F(",\"distance_m\":");
    js += String((uint32_t)distM);
    js += F(",\"mount_offset_deg\":");
    js += String(mountOffsetDeg, 2);
    js += F(",\"servo_angle\":");
    js += String(servoAngleDeg, 1);
    js += F(",\"heading\":");
    js += String(headUsed, 2);
    js += F(",\"heading_samples\":");
    js += String(headSamples);
    js += F(",\"mag_calibrated\":");
    js += magCalibrated ? F("true") : F("false");
    js += F(",\"warning\":\"");
    if (!magCalibrated && magOnline) {
      // Without hard-iron correction this offset is only valid near the
      // orientation it was taken at — the very thing it is meant to outlive.
      js += F("run the magnetometer calibration first, or this offset will not "
              "survive being set up facing a different way");
    } else if (distM < 300.0) {
      // A near landmark amplifies the station's own position error into the
      // bearing: 1 m at 100 m is 0.6 deg, at 1 km it is 0.06 deg.
      js += F("landmark is close; 1 km or more gives a much tighter reference");
    }
    js += F("\"}");
    httpServer.send(200, "application/json", js);
  });

  // GET /api/log?from=<absolute offset> — plain text, delta since that offset.
  // Plain text rather than JSON so the log needs no escaping; the two custom
  // headers carry the bookkeeping (same-origin JS can read them freely).
  httpServer.on("/api/log", HTTP_GET, []() {
    uint32_t from = httpServer.hasArg("from")
                        ? (uint32_t)strtoul(httpServer.arg("from").c_str(), nullptr, 10)
                        : 0;
    bool dropped = false;
    uint32_t next = 0;
    String txt = buildLogText(from, dropped, next);
    httpServer.sendHeader("X-Log-Next", String(next));
    httpServer.sendHeader("X-Log-Dropped", dropped ? "1" : "0");
    httpServer.send(200, "text/plain; charset=utf-8", txt);
  });
  httpServer.on("/api/log", HTTP_POST, []() {  // clear
    logHead = 0;
    logWrapped = false;
    logTotal = 0;
    httpServer.send(200, "application/json", "{\"ok\":true}");
  });

  httpServer.on("/api/status", HTTP_GET, []() {
    httpServer.send(200, "application/json", buildStatusJson());
  });
  // POST /api/servo?angle=<0..180> — manual aim; blocked while auto-tracking.
  httpServer.on("/api/servo", HTTP_POST, []() {
    if (!httpServer.hasArg("angle")) {
      httpServer.send(400, "application/json",
                      "{\"ok\":false,\"error\":\"missing angle param\"}");
      return;
    }
    if (trackMode == MODE_TRACKING) {
      httpServer.send(409, "application/json",
                      "{\"ok\":false,\"error\":\"pause tracking first\"}");
      return;
    }
    float a = constrain(httpServer.arg("angle").toFloat(), 0.0f, 180.0f);
    if (!setServoAngle(a)) {
      httpServer.send(503, "application/json",
                      "{\"ok\":false,\"error\":\"servo PWM unavailable\"}");
      return;
    }
    servoTargetDeg = a;
    if (trackMode == MODE_IDLE) trackMode = MODE_MANUAL;
    Log.print(F("[SERVO] manual angle="));
    Log.println(servoAngleDeg, 1);
    httpServer.send(200, "application/json",
                    "{\"ok\":true,\"angle\":" + String(servoAngleDeg, 1) + "}");
  });
  // POST /api/track/start — lock the current aim as calibration and auto-track.
  httpServer.on("/api/track/start", HTTP_POST, []() {
    if (!servoPwmReady) {
      httpServer.send(503, "application/json",
                      "{\"ok\":false,\"error\":\"servo PWM unavailable\"}");
      return;
    }
    if (startTracking()) {
      httpServer.send(200, "application/json",
                      "{\"ok\":true,\"mount_offset_deg\":" +
                          String(mountOffsetDeg, 1) + "}");
    } else if (!servoPwmReady) {
      httpServer.send(503, "application/json",
                      "{\"ok\":false,\"error\":\"servo PWM unavailable\"}");
    } else {
      httpServer.send(409, "application/json",
                      "{\"ok\":false,\"error\":\"need server+client GPS fix\"}");
    }
  });
  // POST /api/track/pause — hold servo at the current angle, stop auto updates.
  httpServer.on("/api/track/pause", HTTP_POST, []() {
    if (trackMode == MODE_TRACKING) trackMode = MODE_PAUSED;
    Log.println(F("[TRACK] paused"));
    httpServer.send(200, "application/json",
                    "{\"ok\":true,\"mode\":\"" + String(trackModeStr(trackMode)) +
                        "\"}");
  });
  // POST /api/track/resume — resume auto-tracking with the existing calibration.
  httpServer.on("/api/track/resume", HTTP_POST, []() {
    if (!servoPwmReady) {
      httpServer.send(503, "application/json",
                      "{\"ok\":false,\"error\":\"servo PWM unavailable\"}");
      return;
    }
    if (!mountCalibrated) {
      httpServer.send(409, "application/json",
                      "{\"ok\":false,\"error\":\"not calibrated, use start\"}");
      return;
    }
    trackMode = MODE_TRACKING;
    updateTracking();
    if (!servoPwmReady) {
      httpServer.send(503, "application/json",
                      "{\"ok\":false,\"error\":\"servo PWM unavailable\"}");
      return;
    }
    Log.println(F("[TRACK] resumed"));
    httpServer.send(200, "application/json",
                    "{\"ok\":true,\"mode\":\"tracking\"}");
  });
  // POST /api/track/stop — return to manual control (keeps calibration).
  httpServer.on("/api/track/stop", HTTP_POST, []() {
    trackMode = MODE_MANUAL;
    Log.println(F("[TRACK] stopped -> manual"));
    httpServer.send(200, "application/json",
                    "{\"ok\":true,\"mode\":\"manual\"}");
  });
  // 分頁圖示。三個尺寸讓瀏覽器自己挑：16/32 給分頁列（1x / 2x DPI），
  // 192 給 PWA「加到主畫面」與高解析度情境。合計約 3 KB flash。
  //
  // /favicon.ico 一定要有 handler：沒有的話瀏覽器自動發出的那個請求會落到
  // onNotFound -> 302 -> "/"，於是每開一次頁面就多抓一次完整的 44 KB HTML。
  // 內容給 PNG 就好，瀏覽器認的是 Content-Type 不是副檔名。
  auto sendIcon = [](const uint8_t *png, size_t len) {
    httpServer.sendHeader("Cache-Control", "public, max-age=604800");
    httpServer.send_P(200, PSTR("image/png"), (PGM_P)png, len);
  };
  httpServer.on("/favicon.ico", HTTP_GET, [sendIcon]() {
    sendIcon(ICON_32_PNG, sizeof(ICON_32_PNG));
  });
  httpServer.on("/icon-16.png", HTTP_GET, [sendIcon]() {
    sendIcon(ICON_16_PNG, sizeof(ICON_16_PNG));
  });
  httpServer.on("/icon-32.png", HTTP_GET, [sendIcon]() {
    sendIcon(ICON_32_PNG, sizeof(ICON_32_PNG));
  });
  httpServer.on("/icon-192.png", HTTP_GET, [sendIcon]() {
    sendIcon(ICON_192_PNG, sizeof(ICON_192_PNG));
  });
  httpServer.onNotFound([]() {
    httpServer.sendHeader("Location", "/");
    httpServer.send(302);
  });
  httpServer.begin();
}

static void initArduinoOta() {
  if (otaReady || WiFi.status() != WL_CONNECTED) return;

  ArduinoOTA.setHostname("shore-spotter-server");
  ArduinoOTA
      .onStart([]() {
        if (trackMode == MODE_TRACKING) trackMode = MODE_PAUSED;
        Log.println(F("[OTA] update started; tracking paused"));
      })
      .onEnd([]() { Log.println(F("[OTA] update complete; rebooting")); })
      .onError([](ota_error_t error) {
        Log.print(F("[OTA] ERROR code="));
        Log.println((unsigned int)error);
      });
  ArduinoOTA.begin();
  otaReady = true;
  Log.print(F("[OTA] ready: shore-spotter-server.local / "));
  Log.println(WiFi.localIP());
}
#endif

// ---------------------------------------------------------------------------
// Shutdown: show message on OLED then power off via PMU.
// For CLIENT role the OLED bus is normally off; we power it briefly here.
// ---------------------------------------------------------------------------
static void showShutdownAndPowerOff() {
#if defined(ROLE_CLIENT)
  if (pmuOnline) {
    pmu.setALDO1Voltage(3300);
    pmu.enableALDO1();
    delay(100);
  }
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  display.setI2CAddress(oledI2CAddr << 1);
  display.begin();
#endif
  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);
  display.drawStr(24, 36, "SHUTDOWN...");
  display.sendBuffer();
  delay(1500);
  display.clearBuffer();
  display.sendBuffer();
  if (pmuOnline) pmu.shutdown();
}

// Critically-low battery handling. Skipped while on USB (charging) or when no /
// implausible battery is detected, so it never bricks a USB-powered board.
static bool batteryCriticallyLow() {
  if (!pmuOnline) return false;
  uint16_t mv = cachedBatteryMv;
  if (mv < BATT_PRESENT_MIN_MV) return false;  // no / implausible battery reading
  if (pmu.isVbusIn()) return false;            // on USB -> charging, don't cut
  return mv < BATT_SHUTDOWN_MV;
}

// Show a low-battery notice, then cut power. Used at boot and at runtime so a
// dead battery can neither keep running nor power the board back on.
static void showLowBatteryAndPowerOff() {
#if defined(ROLE_CLIENT)
  if (pmuOnline) {
    pmu.setALDO1Voltage(3300);
    pmu.enableALDO1();
    delay(100);
  }
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  display.setI2CAddress(oledI2CAddr << 1);
  display.begin();
#endif
  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);
  display.drawStr(16, 28, "LOW BATTERY");
  display.drawStr(10, 46, "Shutting down");
  display.sendBuffer();
  Log.print(F("[PWR] LOW BATTERY ("));
  Log.print(cachedBatteryMv);
  Log.println(F(" mV) -> power off"));
  delay(2500);
  display.clearBuffer();
  display.sendBuffer();
  if (pmuOnline) pmu.shutdown();
  while (true) { delay(1000); }  // halt if power can't be cut (e.g. on USB)
}

// Debounced runtime check: two consecutive low readings -> shut down.
static void checkLowBatteryAndMaybeShutdown() {
  static uint8_t lowCount = 0;
  if (batteryCriticallyLow()) {
    if (++lowCount >= 2) showLowBatteryAndPowerOff();
  } else {
    lowCount = 0;
  }
}

// ---------------------------------------------------------------------------
// Client boot-info screen: show MAC last 4 hex, battery %, temp/hum.
// Enables OLED rail, displays for 10 s, then disables rail to save power.
// ---------------------------------------------------------------------------
#if defined(ROLE_CLIENT)
// Draw the status page (ID / battery / temp / humidity) to the current buffer.
static void drawClientInfoScreen() {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char line1[20], line2[20], line3[20], line4[20];
  snprintf(line1, sizeof(line1), "ID: %02X%02X", mac[4], mac[5]);
  if (cachedBatteryMv > 0) {
    snprintf(line2, sizeof(line2), "Batt: %u%%", batteryPercent(cachedBatteryMv));
  } else {
    snprintf(line2, sizeof(line2), "Batt: --");
  }
  if (cachedTempC10 != INT16_MIN) {
    snprintf(line3, sizeof(line3), "Temp: %dC", (int)lround(cachedTempC10 / 10.0));
  } else {
    snprintf(line3, sizeof(line3), "Temp: N/A");
  }
  if (cachedHumidityPct != 0xFF) {
    snprintf(line4, sizeof(line4), "Hum:  %u%%", cachedHumidityPct);
  } else {
    snprintf(line4, sizeof(line4), "Hum:  N/A");
  }

  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);
  display.drawStr(0, 12, "SHORE SPOTTER");
  display.drawHLine(0, 14, 128);
  display.drawStr(0, 28, line1);
  display.drawStr(0, 40, line2);
  if (batteryCharging()) display.drawXBMP(70, 31, 8, 8, ICON_BOLT_8);  // ⚡ on USB
  display.drawStr(0, 52, line3);
  display.drawStr(0, 64, line4);
  display.sendBuffer();
}

// 讓 SH1106 進入睡眠（關顯示與升壓電路）。
//
// 只做 clearBuffer()+sendBuffer() 是不夠的：那只是把像素熄掉，控制器和升壓電路
// 還在跑，是毫安等級的常態消耗 —— 對一個要撐完一整場的下水端不划算。
//
// 這裡刻意「不」關 ALDO1 電源軌：那條軌同時供電給 I2C bus-0 上的 BME280，而
// CLIENT 每 5 秒要讀一次溫濕度（也是進水警告的來源）。關掉會連感測器一起失去。
static void sleepClientOled() {
  display.clearBuffer();
  display.sendBuffer();
  display.setPowerSave(1);
}

// Power up the OLED rail and bring up the SH1106. Returns false if it fails.
static bool enableClientOled() {
  if (!pmuOnline) return false;
  pmu.setALDO1Voltage(3300);
  pmu.enableALDO1();
  delay(100);
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  display.setI2CAddress(oledI2CAddr << 1);
  if (!display.begin()) { pmu.disableALDO1(); return false; }
  return true;
}

// 顯示 10 秒的開機資訊頁，然後讓面板睡眠（電源軌維持供電，見 sleepClientOled）。
static void showClientBootScreen() {
  if (!enableClientOled()) return;
  drawClientInfoScreen();

  delay(10000);

  sleepClientOled();
}

// Short-press PWR wakes the screen for CLIENT_SCREEN_WAKE_MS (non-blocking).
// loop() 負責重繪，到期時讓面板回去睡覺（見 sleepClientOled）。
static void wakeClientScreen() {
  if (!enableClientOled()) return;
  clientOledAwake = true;
  clientOledOffMs = millis() + CLIENT_SCREEN_WAKE_MS;
  nextClientOledRefreshMs = 0;  // force an immediate redraw
}
#endif

void setup() {
  Serial.begin(115200);
  // Never let logging stall the firmware. Without this, a board plugged into a
  // PC with no terminal open blocks up to 100 ms per write once the CDC ring
  // buffer fills (HWCDC.cpp: tx_timeout_ms) — on battery it never happens,
  // which is why the symptom only ever showed up on the bench.
  Serial.setTxTimeoutMs(0);
  delay(1200);
  bootMs = millis();
  nodeId = derivedNodeId();

  if (!initRadio()) {
    while (true) {
      delay(1000);
    }
  }
  computeAirtimeBudget();

#if defined(ROLE_CLIENT)
  loadClientSettings();
  radio.setOutputPower(currentTxPowerDbm);
  nextAtpcEvalMs = millis() + ATPC_EVAL_MS;

  pinMode(GPS_EN_PIN, OUTPUT);
  digitalWrite(GPS_EN_PIN, HIGH);

  GPSSerial.setRxBufferSize(GPS_RX_BUFFER_BYTES);  // must precede begin()
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  pmuOnline = initPmu();
  if (pmuOnline) {
    pmu.setALDO1Voltage(3300);
    pmu.enableALDO1();
    // short-press = wake screen 10 s, long-press = shutdown
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);
    delay(100);
  }
  cachedBatteryMv = readBatteryMilliVolts();
  if (batteryCriticallyLow()) showLowBatteryAndPowerOff();  // refuse to boot empty
  nextBatteryMs = millis() + BATTERY_UPDATE_MS;
  nextEnvMs = millis() + ENV_UPDATE_MS;
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  envSensorOnline = initEnvSensor();
  sampleEnvSensor();

  showClientBootScreen();  // show MAC / batt / temp for 10 s then turn off OLED

  Log.println(F("[CLIENT] mode active: send position packets"));
  Log.print(F("[CLIENT] node id (chip MAC last 2 bytes) = 0x"));
  Log.println(nodeId, HEX);
  Log.println(F("[CLIENT] Add this id to SERVER CLIENT_WHITELIST to authorise."));
  Log.print(F("[CLIENT] GPS UART baud="));
  Log.println(GPS_BAUD);
  Log.print(F("[CLIENT] TX power="));
  Log.print(currentTxPowerDbm);
  Log.print(F(" dBm (ATPC="));
  Log.print(atpcEnabled ? F("on") : F("off"));
  Log.println(F(")"));

  // Arm non-blocking ACK reception (see onClientDio1) and start the position
  // cadence from now, so the 10 s boot screen does not count as a missed slot.
  radio.setDio1Action(onClientDio1);
  radio.startReceive();
  nextSendMs = millis();
#endif

#if defined(ROLE_SERVER)
  pmuOnline = initPmu();
  if (pmuOnline) {
    pmu.setALDO1Voltage(3300);
    pmu.enableALDO1();  // power the OLED / I2C bus-0 peripherals
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_LONG_IRQ);  // long-press = shutdown
    delay(100);
  }

  // Server also reads its own GPS so it can compute bearing to the client.
  pinMode(GPS_EN_PIN, OUTPUT);
  digitalWrite(GPS_EN_PIN, HIGH);
  GPSSerial.setRxBufferSize(GPS_RX_BUFFER_BYTES);  // must precede begin()
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  envSensorOnline = initEnvSensor();
  sampleEnvSensor();
  nextEnvMs = millis() + ENV_UPDATE_MS;
  magOnline = initMag();           // QMC6310 station heading (shared I2C bus 0)
  nextMagMs = millis() + MAG_SAMPLE_MS;
  initServo();                     // LEDC PWM on IO21, centre the camera
  display.setI2CAddress(oledI2CAddr << 1);
  display.begin();
  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);
  display.drawStr(0, 12, "SHORE SPOTTER");
  display.drawStr(0, 30, "SERVER booting...");
  display.sendBuffer();

  cachedBatteryMv = readBatteryMilliVolts();
  if (batteryCriticallyLow()) showLowBatteryAndPowerOff();  // refuse to boot empty

  nextServerIdleLogMs = millis() + SERVER_IDLE_LOG_MS;
  nextRxSummaryMs = millis() + RX_SUMMARY_MS;
  nextDisplayMs = millis() + DISPLAY_REFRESH_MS;
  Log.println(F("[SERVER] mode active: receive position packets"));
  loadServerSettings();
  Log.print(F("[SERVER] whitelist ("));
  Log.print(clientWhitelistCount);
  Log.print(F(" entries): "));
  for (size_t i = 0; i < clientWhitelistCount; i++) {
    Log.print(F("0x"));
    Log.print(clientWhitelist[i], HEX);
    if (i + 1 < clientWhitelistCount) Log.print(' ');
  }
  Log.println();

  // Connect to the phone-provided hotspot in station mode.
  // Credentials come from include/wifi_config.h (WIFI_SSID / WIFI_PASSWORD).
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("shore-spotter-server");
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Log.print(F("[WiFi] Connecting to hotspot \""));
  Log.print(WIFI_SSID);
  Log.print(F("\" "));
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiStart < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Log.print('.');
  }
  Log.println();

  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);
  display.drawStr(0, 12, "SHORE SPOTTER");
  display.drawHLine(0, 14, 128);
  if (WiFi.status() == WL_CONNECTED) {
    cachedApIpAddr = WiFi.localIP();
    cachedApIp = cachedApIpAddr.toString();
    Log.print(F("[WiFi] Connected. IP: "));
    Log.println(cachedApIp);
    Log.print(F("[WiFi] Open browser -> http://"));
    Log.println(cachedApIp);
    display.drawStr(0, 32, "WiFi connected");
    display.drawStr(0, 48, cachedApIp.c_str());
  } else {
    cachedApIp = "";
    Log.println(F("[WiFi] Hotspot connect FAILED."));
    Log.println(F("[WiFi] Check SSID/password in include/wifi_config.h."));
    Log.println(F("[WiFi] LoRa tracking still runs; WiFi will auto-retry."));
    display.drawStr(0, 32, "WiFi FAILED");
    display.drawStr(0, 48, "see wifi_config.h");
  }
  display.sendBuffer();
  delay(2000);

  initWebServer();
  initArduinoOta();

  // Arm non-blocking, interrupt-driven reception.
  radio.setDio1Action(onLoRaDio1);
  radio.startReceive();
#endif
}

void loop() {
#if defined(ROLE_CLIENT)
  serviceGps();

  // Non-blocking ACK drain (see onClientDio1). Kept ahead of the transmit slots
  // below: those clear clientRxFlag to swallow their own TxDone pulse, and would
  // otherwise discard an ACK that happened to land in the same millisecond.
  if (clientRxFlag) {
    clientRxFlag = false;
    uint8_t ackBuf[ACK_PACKET_LEN];
    int ackState = radio.readData(ackBuf, sizeof(ackBuf));
    if (ackState == RADIOLIB_ERR_NONE) {
      size_t n = radio.getPacketLength();
      if (n > sizeof(ackBuf)) n = sizeof(ackBuf);
      AckPayload ack{};
      if (parseAckPacket(ackBuf, n, ack)) {
        lastAckRxMs = millis();
        ackRxCount++;
        lastAckRssiDbm10 = ack.rssiDbm10;
        lastAckSnrDb10 = ack.snrDb10;
        Log.print(F("[CLIENT] ACK seq="));
        Log.print(ack.ackSeq);
        Log.print(F(" | up(srv heard us) rssi="));
        Log.print(ack.rssiDbm10 / 10.0f, 1);
        Log.print(F(" snr="));
        Log.print(ack.snrDb10 / 10.0f, 1);
        Log.print(F(" | down(we heard ack) rssi="));
        Log.print(radio.getRSSI(), 1);
        Log.print(F(" snr="));
        Log.println(radio.getSNR(), 1);
      }
    }
    radio.startReceive();  // re-arm for the next ACK
  }

  if (millis() >= nextBatteryMs) {
    nextBatteryMs = millis() + BATTERY_UPDATE_MS;
    cachedBatteryMv = readBatteryMilliVolts();
    Log.print(F("[CLIENT] Battery update mV="));
    Log.println(cachedBatteryMv);
    checkLowBatteryAndMaybeShutdown();
  }

  if (millis() >= nextEnvMs) {
    nextEnvMs = millis() + ENV_UPDATE_MS;
    sampleEnvSensor();
  }

  if (millis() >= nextSendMs) {
    nextSendMs = millis() + SEND_INTERVAL_MS;
    lastSendMs = millis();

    uint8_t buf[DATA_PACKET_LEN];
    size_t len = buildDataPacket(buf);
    int state = radio.transmit(buf, len);
    // Re-arm RX before the (comparatively slow) serial log below: the server
    // starts its ACK within a few ms of our TxDone.
    clientRxFlag = false;  // swallow the TxDone pulse from our own transmission
    radio.startReceive();

    if (state == RADIOLIB_ERR_NONE) {
      radioFailStreak = 0;
      Log.print(F("[CLIENT] TX ok seq="));
      Log.print((uint16_t)(txSeq - 1));
      Log.print(F(" | GPS fix="));
      Log.print(gpsFixFresh() ? 1 : 0);
      Log.print(F(" sats="));
      Log.print(gps.satellites.isValid() ? (int)gps.satellites.value() : -1);
      Log.print(F(" hdop="));
      if (gps.hdop.isValid()) Log.print(gps.hdop.hdop(), 1);
      else Log.print(F("--"));
      Log.print(F(" age="));
      Log.print(gps.location.age());
      Log.print(F("ms spd="));
      Log.print(gps.speed.isValid() ? gps.speed.mps() : 0.0, 1);
      Log.print(F("m/s crs="));
      Log.print(gps.course.isValid() ? gps.course.deg() : 0.0, 0);
      Log.print(F(" | LoRa up(srv heard us) rssi="));
      Log.print(lastAckRssiDbm10 / 10.0f, 1);
      Log.print(F(" snr="));
      Log.print(lastAckSnrDb10 / 10.0f, 1);
      Log.print(F(" txpwr="));
      Log.print(currentTxPowerDbm);
      Log.print(F("dBm acks="));
      Log.print(ackRxCount);
      Log.print(F(" miss="));
      Log.println(ackMissCount);
    } else {
      Log.print(F("[CLIENT] TX failed, code="));
      Log.println(state);
      if (++radioFailStreak >= RADIO_TX_FAIL_LIMIT) recoverRadio();
    }
  }

  // Telemetry waits for the quiet slot of the position cycle (see
  // TELEMETRY_SLOT_MIN_MS); loop() spins fast enough that the slot is never
  // missed once the packet is due.
  if (millis() >= nextTelemetryMs && lastSendMs != 0) {
    uint32_t sinceSend = millis() - lastSendMs;
    if (sinceSend >= telemetrySlotMinMs && sinceSend <= telemetrySlotMaxMs) {
      nextTelemetryMs = millis() + TELEMETRY_INTERVAL_MS;
      uint8_t tbuf[TELEMETRY_PACKET_LEN];
      size_t tlen = buildTelemetryPacket(tbuf);
      int tstate = radio.transmit(tbuf, tlen);
      clientRxFlag = false;
      radio.startReceive();
      if (tstate == RADIOLIB_ERR_NONE) {
        Log.print(F("[CLIENT] TELEMETRY TX ok batt_mV="));
        Log.println(cachedBatteryMv);
      } else {
        Log.print(F("[CLIENT] TELEMETRY TX failed, code="));
        Log.println(tstate);
      }
    }
  }

  if (txSeq > 5 && (millis() - lastAckRxMs > ACK_STALE_MS) &&
      (millis() - lastAckMissMarkMs > 5000)) {
    ackMissCount++;
    lastAckMissMarkMs = millis();
  }

  evaluateAtpc();

  // PWR key: short-press wakes the screen 10 s, long-press shuts down.
  // Polled on a timer (see PMU_KEY_POLL_MS) — loop() no longer blocks, so every
  // pass would otherwise cost two I2C transactions on the PMU bus.
  if (pmuOnline && millis() >= nextPmuKeyMs) {
    nextPmuKeyMs = millis() + PMU_KEY_POLL_MS;
    pmu.getIrqStatus();
    if (pmu.isPekeyShortPressIrq()) {
      wakeClientScreen();
    }
    if (pmu.isPekeyLongPressIrq()) {
      pmu.clearIrqStatus();
      showShutdownAndPowerOff();
    }
    pmu.clearIrqStatus();
  }

  // Keep the woken screen refreshed, then put the panel back to sleep.
  if (clientOledAwake) {
    if (millis() >= clientOledOffMs) {
      sleepClientOled();
      clientOledAwake = false;
    } else if (millis() >= nextClientOledRefreshMs) {
      nextClientOledRefreshMs = millis() + DISPLAY_REFRESH_MS;
      drawClientInfoScreen();
    }
  }

  // Nothing in this loop blocks any more, so without an explicit yield the task
  // would spin at full CPU and never let the core's idle task run — wasted
  // battery on a device that has to last a session in the water. At
  // CONFIG_FREERTOS_HZ=1000 this is a 1 ms tick, far finer than anything above.
  // (The server gets the same yield for free from WebServer::handleClient().)
  delay(1);
#endif

#if defined(ROLE_SERVER)
  if (otaReady && WiFi.status() == WL_CONNECTED) ArduinoOTA.handle();
  httpServer.handleClient();
  serviceGps();  // keep the server's own GPS position fresh

  if (millis() >= nextEnvMs) {
    nextEnvMs = millis() + ENV_UPDATE_MS;
    sampleEnvSensor();
  }

  if (millis() >= nextMagMs) {
    nextMagMs = millis() + (magCalState == MAGCAL_COLLECTING ? MAG_CAL_SAMPLE_MS
                                                            : MAG_SAMPLE_MS);
    sampleMag();
  }

  if (millis() >= nextBatteryMs) {
    nextBatteryMs = millis() + BATTERY_UPDATE_MS;
    cachedBatteryMv = readBatteryMilliVolts();
    checkLowBatteryAndMaybeShutdown();
  }

  // WiFi watchdog: re-attempt the hotspot every WIFI_RETRY_INTERVAL_MS while
  // offline, and refresh the cached IP once (re)connected.
  if (WiFi.status() == WL_CONNECTED) {
    // Re-read on change, not just when empty: a DHCP renewal can hand out a
    // different address without the link ever reporting disconnected, and the
    // old code would then show a stale IP on the OLED forever. Compared as an
    // IPAddress so the common case allocates no String — loop() runs at ~1 kHz.
    IPAddress ip = WiFi.localIP();
    if (ip != cachedApIpAddr) {
      cachedApIpAddr = ip;
      cachedApIp = ip.toString();
      Log.print(F("[WiFi] address is now "));
      Log.println(cachedApIp);
    }
    initArduinoOta();
  } else {
    cachedApIp = "";
    cachedApIpAddr = IPAddress();
    if (millis() >= nextWifiRetryMs) {
      nextWifiRetryMs = millis() + WIFI_RETRY_INTERVAL_MS;
      wifiReconnectingUntilMs = millis() + 2000;  // show "reconnecting" briefly
      WiFi.reconnect();
    }
  }

  // Long-press PWR → show shutdown screen then power off
  if (pmuOnline && millis() >= nextPmuKeyMs) {
    nextPmuKeyMs = millis() + PMU_KEY_POLL_MS;
    pmu.getIrqStatus();
    if (pmu.isPekeyLongPressIrq()) {
      pmu.clearIrqStatus();
      showShutdownAndPowerOff();
    }
    pmu.clearIrqStatus();  // don't let unrelated latched IRQs accumulate
  }

  // Interrupt-driven RX: the DIO1 ISR sets rxDoneFlag; the loop stays
  // non-blocking so httpServer.handleClient() above replies instantly.
  if (rxDoneFlag) {
    rxDoneFlag = false;
    bool ackSent = false;
    uint8_t buf[DATA_PACKET_LEN];
    int state = radio.readData(buf, sizeof(buf));

    if (state == RADIOLIB_ERR_NONE) {
      radioFailStreak = 0;
      size_t n = radio.getPacketLength();
      if (n > sizeof(buf)) {
        n = sizeof(buf);
      }

    DecodedData d{};
    if (parseDataPacket(buf, n, d)) {
      lastData = d;
      lastRssi = radio.getRSSI();
      lastSnr = radio.getSNR();
      lastRxMs = millis();
      havePkt = true;
      rxDataCount++;
      // Update LoRa rolling stats
      rssiRing[rssiRingIdx] = lastRssi;
      snrRing[rssiRingIdx]  = lastSnr;
      rssiRingIdx = (rssiRingIdx + 1) % RSSI_WINDOW;
      if (rssiRingCount < RSSI_WINDOW) rssiRingCount++;
      pktsThisWindow++;
      if (pktWindowStartMs == 0) pktWindowStartMs = millis();
      if (millis() - pktWindowStartMs >= 60000) {
        cachedPktRate = pktsThisWindow / 60.0f;
        pktsThisWindow = 0;
        pktWindowStartMs = millis();
      }
      // Servo steering is NOT driven from here — it runs on the TRACK_UPDATE_MS
      // tick in loop() so the pan keeps going between (and through missing)
      // packets. This handler only refreshes the data it feeds on.

      // Acknowledge only every ACK_EVERY_N-th sequence number (see ACK_EVERY_N).
      // Keyed on the client's seq, so packet loss cannot slide the schedule.
      if (d.seq % ACK_EVERY_N == 0) {
        uint8_t ackBuf[ACK_PACKET_LEN];
        size_t ackLen = buildAckPacket(ackBuf, d.srcId, d.seq, lastRssi, lastSnr);
        ackSent = true;  // TxDone pulses on DIO1 whether or not the TX succeeded
        if (radio.transmit(ackBuf, ackLen) == RADIOLIB_ERR_NONE) {
          ackTxCount++;
          rxWinAck++;
        }
      }

      // Folded into the once-a-minute summary (see logRxSummary) rather than
      // logged here. Sequence numbers give the loss count for free: the client
      // increments seq every send, so expected = last - first + 1.
      rxWinData++;
      if (!rxWinHaveSeq) {
        rxWinFirstSeq = d.seq;
        rxWinHaveSeq = true;
        rxWinRssiMin = rxWinRssiMax = lastRssi;
        rxWinSnrMin = rxWinSnrMax = lastSnr;
      } else {
        if (lastRssi < rxWinRssiMin) rxWinRssiMin = lastRssi;
        if (lastRssi > rxWinRssiMax) rxWinRssiMax = lastRssi;
        if (lastSnr < rxWinSnrMin) rxWinSnrMin = lastSnr;
        if (lastSnr > rxWinSnrMax) rxWinSnrMax = lastSnr;
      }
      rxWinLastSeq = d.seq;
      rxWinRssiSum += lastRssi;
      rxWinSnrSum += lastSnr;
    } else {
      DecodedTelemetry t{};
      if (parseTelemetryPacket(buf, n, t)) {
        lastTelemetry = t;
        // 下水前的第一筆當基準；之後濕度相對它爬升就是滲水的徵兆。
        if (clientHumBaselinePct < 0 && t.humidityPct != 0xFF) {
          clientHumBaselinePct = (int)t.humidityPct;
        }
        haveTelemetry = true;
        lastTelemetryRxMs = millis();
        rxTelemetryCount++;
        rxWinTelem++;
      } else {
        rxDropCount++;
        rxWinDrop++;
      }
    }
    } else {
      rxErrorCount++;
      rxWinErr++;
      rxWinLastErr = state;  // reported once in the summary, with its RadioLib code
      // CRC 錯誤在距離極限本來就會出現，所以門檻設得比 client 高很多：
      // 這裡要抓的是「無線電卡住之後每次都回同一個錯」，不是偶發的壞封包。
      if (++radioFailStreak >= RADIO_RX_ERR_LIMIT) recoverRadio();
    }
    // Only swallow the DIO1 pulse when we actually transmitted — on the cycles
    // that skip the ACK the flag can only mean a genuine packet arrived while
    // this handler was running, and clearing it would drop that packet.
    if (ackSent) rxDoneFlag = false;
    radio.startReceive();     // re-arm for the next packet
  }

  // Camera tracking tick: 20 Hz, independent of the 1 Hz packet arrival.
  if (millis() >= nextTrackMs) {
    nextTrackMs = millis() + TRACK_UPDATE_MS;
    updateTracking();
  }

  if (millis() >= nextRxSummaryMs) {
    nextRxSummaryMs = millis() + RX_SUMMARY_MS;
    logRxSummary();
  }

  // Periodic idle log when no packet has arrived for a while.
  if (millis() >= nextServerIdleLogMs &&
      (!havePkt || millis() - lastRxMs > 2500)) {
    nextServerIdleLogMs = millis() + SERVER_IDLE_LOG_MS;
    Log.print(F("[SERVER] idle | mode="));
    Log.print(trackModeStr(trackMode));
    Log.print(F(" SRV-GPS fix="));
    Log.print(gpsFixFresh() ? 1 : 0);
    Log.print(F(" sats="));
    Log.print(gps.satellites.isValid() ? (int)gps.satellites.value() : -1);
    Log.print(F(" hdop="));
    if (gps.hdop.isValid()) Log.print(gps.hdop.hdop(), 1);
    else Log.print(F("--"));
    Log.print(F(" head="));
    if (magOnline && magHeadingDeg >= 0) Log.print(magHeadingDeg, 1);
    else Log.print(F("N/A"));
    Log.print(F(" servo="));
    Log.print(servoAngleDeg, 1);
    Log.println(F(" (waiting for client packets)"));
  }

  if (millis() >= nextDisplayMs) {
    nextDisplayMs = millis() + DISPLAY_REFRESH_MS;
    renderServerDisplay();
  }
#endif
}
