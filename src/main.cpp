#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <Preferences.h>
#include <Adafruit_BME280.h>
#include <esp_system.h>

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#include <U8g2lib.h>

#include "protocol.h"  // shared LoRa wire protocol (client + server)

#if defined(ROLE_SERVER)
#include <WiFi.h>
#include <WebServer.h>
#include <SensorQMC6310.hpp>  // SensorLib: QMC6310 magnetometer (station heading)
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
constexpr int RF_CR = 7;
constexpr int RF_SYNC_WORD = 0x12;
constexpr int TX_POWER_DBM = 17;
constexpr int TX_POWER_MIN_DBM = 10;
constexpr int TX_POWER_MAX_DBM = 22;
constexpr uint32_t ATPC_EVAL_MS = 15000;

constexpr uint32_t SEND_INTERVAL_MS = 1000;
constexpr uint32_t TELEMETRY_INTERVAL_MS = 30000;  // battery + env packet rate
constexpr uint32_t BATTERY_UPDATE_MS = 5000;
constexpr uint32_t ENV_UPDATE_MS = 5000;
constexpr uint32_t SERVER_IDLE_LOG_MS = 5000;
constexpr uint32_t LINK_TIMEOUT_MS = 5000;
constexpr uint32_t LINK_WARN_MS = 15000;
constexpr uint32_t DISPLAY_REFRESH_MS = 500;
constexpr uint32_t LCD_ALERT_BLINK_MS = 5000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000;  // server: re-attempt hotspot every 5 s when offline

constexpr uint16_t BATT_LOW_MV = 3500;   // conservative battery-safe threshold
constexpr uint16_t BATT_FULL_MV = 4150;
// Battery percentage scale (single Li-ion cell): 0% at 3.2 V, 100% at 4.2 V.
constexpr uint16_t BATT_EMPTY_MV = 3200;       // 0 %
constexpr uint16_t BATT_PCT_FULL_MV = 4200;    // 100 %
constexpr uint16_t BATT_SHUTDOWN_MV = 3200;    // below 0 % -> auto power off
constexpr uint16_t BATT_PRESENT_MIN_MV = 2500; // ignore implausible/no-battery reads
constexpr int16_t TEMP_WARN_C10 = 500;   // 50.0C
constexpr uint8_t HUM_WARN_PCT = 90;

// Client OLED is normally off to save power; short-press PWR wakes it briefly.
constexpr uint32_t CLIENT_SCREEN_WAKE_MS = 10000;

#if defined(ROLE_SERVER)
// Camera servo (SPT5435LV-180) driven by ESP32 LEDC PWM on IO21.
constexpr int SERVO_PIN = 21;
constexpr int SERVO_MIN_US = 500;    // -> 0 deg
constexpr int SERVO_MAX_US = 2500;   // -> 180 deg
constexpr int SERVO_LEDC_CH = 0;     // LEDC channel (arduino-esp32 2.x)
constexpr uint32_t MAG_SAMPLE_MS = 200;
#endif

// GPS UART defaults (common on T-Beam family, override if your board differs).
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
constexpr uint8_t OLED_I2C_ADDR = 0x3C;

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

// Shared OLED object — client enables it only during boot-info and shutdown screens.
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

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
uint32_t nextTelemetryMs = 0;
uint32_t nextBatteryMs = 0;
uint32_t nextServerIdleLogMs = 0;
uint16_t cachedBatteryMv = 0;
bool pmuOnline = false;
uint16_t nodeId = 0;  // set in setup() from chip MAC last 2 bytes

// Client-side velocity / acceleration tracking
static uint16_t prevSpeedCmS   = 0;
static uint32_t prevSpeedMs    = 0;
static int16_t  smoothAccelCmS2 = 0;
static uint32_t bootMs = 0;
static uint16_t lastAckedSeq = 0;
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

// Telemetry (slow path, MSG_TELEMETRY every 30 s)
DecodedTelemetry lastTelemetry{0, 0, INT8_MIN, 0xFF};
bool haveTelemetry = false;
uint32_t lastTelemetryRxMs = 0;

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
// Geometry: world_bearing = mag_heading + servo_angle + mountOffset
//   * Manual: operator nudges servo_angle until the camera is on the surfer.
//   * Start : mountOffset is locked from the current (bearing, heading, angle).
//   * Track : servo_angle = bearing_now - heading_now - mountOffset (clamped 0..180).
// The magnetometer term lets the station be bumped/rotated mid-session.
enum TrackMode : uint8_t { MODE_IDLE, MODE_MANUAL, MODE_TRACKING, MODE_PAUSED };
static TrackMode trackMode = MODE_MANUAL;
static float servoAngleDeg = 90.0f;   // current commanded servo angle (0..180)
static float servoTargetDeg = 90.0f;  // desired angle while tracking
static float mountOffsetDeg = 0.0f;   // servo-to-world mounting offset (persisted)
static bool  mountCalibrated = false;

// Magnetometer (QMC6310) — station board heading in degrees (-1 = invalid)
static SensorQMC6310 mag;
static bool  magOnline = false;
static float magHeadingDeg = -1.0f;
static uint32_t nextMagMs = 0;
static uint32_t rxDataCount = 0;
static uint32_t rxTelemetryCount = 0;
static uint32_t rxDropCount = 0;
static uint32_t rxErrorCount = 0;
static uint32_t ackTxCount = 0;

WebServer httpServer(80);

// Interrupt-driven LoRa RX: DIO1 fires on RxDone; the ISR only sets a flag so
// the main loop never blocks in receive() and the web server stays responsive.
volatile bool rxDoneFlag = false;
void IRAM_ATTR onLoRaDio1() { rxDoneFlag = true; }
#endif

#if defined(ROLE_SERVER)
static bool parseDataPacket(const uint8_t *buf, size_t n, DecodedData &out) {
  if (n < sizeof(PacketHeader)) {
    return false;
  }

  PacketHeader hdr;
  memcpy(&hdr, buf, sizeof(hdr));

  // Validate + group-filter. NETWORK_ID is the authoritative group check.
  if (hdr.magic != PROTO_MAGIC || hdr.version != PROTO_VERSION) {
    return false;
  }
  if (hdr.networkId != NETWORK_ID) {
    return false;
  }
  if (hdr.dstId != SERVER_ID && hdr.dstId != ID_BROADCAST) {
    return false;
  }
  if (hdr.msgType != MSG_DATA || hdr.payloadLen != sizeof(PositionPayload)) {
    return false;
  }
  if (n < sizeof(PacketHeader) + hdr.payloadLen + MAC_LEN) {
    return false;
  }
  if (!isClientAllowed(hdr.srcId)) {
    return false;  // sender not on the whitelist
  }

  // NOTE (stage 2): verify the trailing MAC_LEN-byte HMAC over header+payload here.

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
  if (n < sizeof(PacketHeader)) return false;
  PacketHeader hdr;
  memcpy(&hdr, buf, sizeof(hdr));
  if (hdr.magic != PROTO_MAGIC || hdr.version != PROTO_VERSION) return false;
  if (hdr.networkId != NETWORK_ID) return false;
  if (hdr.dstId != SERVER_ID && hdr.dstId != ID_BROADCAST) return false;
  if (hdr.msgType != MSG_TELEMETRY || hdr.payloadLen != sizeof(TelemetryPayload)) return false;
  if (n < sizeof(PacketHeader) + hdr.payloadLen + MAC_LEN) return false;
  if (!isClientAllowed(hdr.srcId)) return false;
  TelemetryPayload tel;
  memcpy(&tel, buf + sizeof(PacketHeader), sizeof(tel));
  out.srcId       = hdr.srcId;
  out.batteryMv   = tel.batteryMv;
  out.tempC       = tel.tempC;
  out.humidityPct = tel.humidityPct;
  return true;
}
#endif

static uint16_t readBatteryMilliVolts() {
  if (!pmuOnline) {
    return 0;
  }

  if (!pmu.isBatteryConnect()) {
    return 0;
  }

  return pmu.getBattVoltage();
}

// Battery charge percentage: 0% at BATT_EMPTY_MV, 100% at BATT_PCT_FULL_MV.
static uint8_t batteryPercent(uint16_t mv) {
  if (mv <= BATT_EMPTY_MV) return 0;
  if (mv >= BATT_PCT_FULL_MV) return 100;
  return (uint8_t)lround((mv - BATT_EMPTY_MV) * 100.0f /
                         (BATT_PCT_FULL_MV - BATT_EMPTY_MV));
}

// True when external (USB / Type-C) power is present — board runs "plugged in".
static bool batteryCharging() {
  return pmuOnline && pmu.isVbusIn();
}

static bool initPmu() {
  PMUWire.begin(PMU_SDA_PIN, PMU_SCL_PIN);
  if (!pmu.begin(PMUWire, AXP2101_SLAVE_ADDRESS, PMU_SDA_PIN, PMU_SCL_PIN)) {
    Serial.println(F("[PMU] AXP2101 init failed"));
    return false;
  }

  pmu.enableBattDetection();
  pmu.enableVbusVoltageMeasure();
  pmu.enableBattVoltageMeasure();
  pmu.enableSystemVoltageMeasure();

  // GPS rail (ALDO4) is needed by both roles.
  pmu.setALDO4Voltage(3300);
  pmu.enableALDO4();

  Serial.println(F("[PMU] AXP2101 init ok"));
  return true;
}

static void serviceGps() {
  while (GPSSerial.available() > 0) {
    gps.encode(GPSSerial.read());
  }
}

static bool initEnvSensor() {
  // BME280 is common and simple; try both default addresses.
  if (envSensor.begin(0x76, &Wire) || envSensor.begin(0x77, &Wire)) {
    Serial.println(F("[ENV] BME280 init ok"));
    return true;
  }
  Serial.println(F("[ENV] BME280 not found (telemetry stays N/A)"));
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
}

// 8x8 lightning icon (LSB-first rows for U8g2 drawXBMP), drawn when the board
// is on external/USB power.
static const uint8_t ICON_BOLT_8[] = {0x38,0x0C,0x06,0x1F,0x1C,0x0C,0x06,0x03};

// 4-level signal grade (Good / OK / Bad / Miss) for GPS & LoRa, shown on the
// server run screen.
#if defined(ROLE_SERVER)
enum SigLevel : uint8_t { SIG_GOOD = 0, SIG_OK = 1, SIG_BAD = 2, SIG_MISS = 3 };
static const char* sig4Text(SigLevel s) {
  switch (s) {
    case SIG_GOOD: return "Good";
    case SIG_OK:   return "OK";
    case SIG_BAD:  return "Bad";
    default:       return "Miss";
  }
}

// LoRa grade from the last packet's RSSI/SNR; the caller maps "no link" -> Miss.
static SigLevel loraSignal(float rssi, float snr) {
  if (snr >= 7.0f && rssi >= -105.0f) return SIG_GOOD;
  if (snr >= 0.0f) return SIG_OK;
  return SIG_BAD;
}

// Unified GPS grade for the station's own GPS and the client's (decoded) GPS.
//   Good : fix & HDOP <= 2.0 & sats >= 6
//   OK   : fix & HDOP <= 5.0 & sats >= 4
//   Bad  : tracking satellites but no usable fix (no fix / sats < 4 / HDOP > 5)
//   Miss : no satellites and no fix (no signal at all)
static SigLevel gpsSignal(bool fix, int sats, float hdop) {
  if (sats <= 0 && !fix) return SIG_MISS;
  if (!fix || sats < 4) return SIG_BAD;
  if (hdop <= 2.0f && sats >= 6) return SIG_GOOD;
  if (hdop <= 5.0f) return SIG_OK;
  return SIG_BAD;
}

static SigLevel serverGpsState() {
  bool  fix  = gps.location.isValid();
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
  prefs.putBool("mountcal", mountCalibrated);
}

static void loadServerSettings() {
  prefs.begin("shorespotter", false);
  loadWhitelistFromNvs();
  mountOffsetDeg = prefs.getFloat("mountoff", 0.0f);
  mountCalibrated = prefs.getBool("mountcal", false);
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
  int st = radio.setOutputPower(target);
  if (st == RADIOLIB_ERR_NONE) {
    currentTxPowerDbm = target;
    saveClientSettings();
    Serial.print(F("[CLIENT] TX power set to "));
    Serial.print(currentTxPowerDbm);
    Serial.println(F(" dBm"));
  }
}

static void evaluateAtpc() {
  if (!atpcEnabled) return;
  if (millis() < nextAtpcEvalMs) return;
  nextAtpcEvalMs = millis() + ATPC_EVAL_MS;

  // No ACK for a while: push one step up.
  if (millis() - lastAckRxMs > 20000) {
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
  pos.fix = gps.location.isValid() ? 1 : 0;
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
    Serial.print(F("[LoRa] init failed, code="));
    Serial.println(state);
    return false;
  }
  Serial.println(F("[LoRa] init ok"));
  return true;
}

#if defined(ROLE_SERVER)
static double computeBearing(double lat1, double lon1, double lat2, double lon2) {
  double phi1 = radians(lat1);
  double phi2 = radians(lat2);
  double dLon = radians(lon2 - lon1);
  double y = sin(dLon) * cos(phi2);
  double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dLon);
  double brng = degrees(atan2(y, x));
  if (brng < 0) {
    brng += 360.0;
  }
  return brng;
}

static float normalize360(float a) {
  while (a < 0) a += 360.0f;
  while (a >= 360.0f) a -= 360.0f;
  return a;
}

static const char *trackModeStr(TrackMode m) {
  switch (m) {
    case MODE_TRACKING: return "tracking";
    case MODE_PAUSED:   return "paused";
    case MODE_MANUAL:   return "manual";
    default:            return "idle";
  }
}

static void servoWriteMicros(int us) {
  us = constrain(us, SERVO_MIN_US, SERVO_MAX_US);
  uint32_t duty = (uint32_t)((float)us / 20000.0f * 65535.0f);  // 50 Hz, 16-bit
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(SERVO_PIN, duty);
#else
  ledcWrite(SERVO_LEDC_CH, duty);
#endif
}

static void setServoAngle(float deg) {
  deg = constrain(deg, 0.0f, 180.0f);
  servoAngleDeg = deg;
  int us = SERVO_MIN_US +
           (int)lroundf(deg / 180.0f * (SERVO_MAX_US - SERVO_MIN_US));
  servoWriteMicros(us);
}

static void initServo() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(SERVO_PIN, 50, 16);
#else
  ledcSetup(SERVO_LEDC_CH, 50, 16);
  ledcAttachPin(SERVO_PIN, SERVO_LEDC_CH);
#endif
  setServoAngle(servoAngleDeg);  // centre on boot
  Serial.print(F("[SERVO] LEDC ready on IO"));
  Serial.print(SERVO_PIN);
  Serial.print(F(", centre "));
  Serial.print(servoAngleDeg, 0);
  Serial.println(F(" deg"));
}

static bool initMag() {
  if (mag.begin(Wire, QMC6310U_SLAVE_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN) ||
      mag.begin(Wire, QMC6310N_SLAVE_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN)) {
    mag.configMagnetometer(OperationMode::CONTINUOUS_MEASUREMENT,
                           MagFullScaleRange::FS_8G, 200.0f,
                           MagOverSampleRatio::OSR_1, MagDownSampleRatio::DSR_1);
    Serial.println(F("[MAG] QMC6310 init ok"));
    return true;
  }
  Serial.println(F("[MAG] QMC6310 not found (heading N/A, station assumed fixed)"));
  return false;
}

static void sampleMag() {
  if (!magOnline) {
    magHeadingDeg = -1.0f;
    return;
  }
  MagnetometerData d;
  if (mag.readData(d)) {
    magHeadingDeg = normalize360(d.heading_degrees);
  }
}

// Heading used in tracking maths; 0 when no magnetometer (station assumed fixed).
static float trackingHeading() {
  return (magOnline && magHeadingDeg >= 0) ? magHeadingDeg : 0.0f;
}

static bool haveBearingFix() {
  return havePkt && lastData.fix && gps.location.isValid();
}

// Recompute and command the servo while tracking.
static void updateTracking() {
  if (trackMode != MODE_TRACKING || !mountCalibrated || !haveBearingFix()) return;
  float bearing = (float)computeBearing(gps.location.lat(), gps.location.lng(),
                                        lastData.lat, lastData.lon);
  float target = normalize360(bearing - trackingHeading() - mountOffsetDeg);
  if (target > 270.0f) target -= 360.0f;  // wrap small negatives toward 0
  servoTargetDeg = constrain(target, 0.0f, 180.0f);
  setServoAngle(servoTargetDeg);
  static uint32_t nextTrackLogMs = 0;
  if (millis() >= nextTrackLogMs) {
    nextTrackLogMs = millis() + 3000;
    Serial.print(F("[TRACK] bearing="));
    Serial.print(bearing, 1);
    Serial.print(F(" head="));
    Serial.print(trackingHeading(), 1);
    Serial.print(F(" off="));
    Serial.print(mountOffsetDeg, 1);
    Serial.print(F(" servo="));
    Serial.println(servoTargetDeg, 1);
  }
}

// Lock the servo-to-world mounting offset from the current aim, then track.
static bool startTracking() {
  if (!haveBearingFix()) return false;
  float bearingCal = (float)computeBearing(gps.location.lat(), gps.location.lng(),
                                           lastData.lat, lastData.lon);
  float off = normalize360(bearingCal - trackingHeading() - servoAngleDeg);
  if (off > 180.0f) off -= 360.0f;
  mountOffsetDeg = off;
  mountCalibrated = true;
  saveMountOffsetToNvs();
  trackMode = MODE_TRACKING;
  Serial.print(F("[TRACK] start: bearing="));
  Serial.print(bearingCal, 1);
  Serial.print(F(" head="));
  Serial.print(trackingHeading(), 1);
  Serial.print(F(" servo="));
  Serial.print(servoAngleDeg, 1);
  Serial.print(F(" -> mount_offset="));
  Serial.println(mountOffsetDeg, 1);
  updateTracking();
  return true;
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

  char buf[24];
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
static String buildTrackJson() {
  bool linked = havePkt && ((millis() - lastRxMs) <= LINK_TIMEOUT_MS);
  uint32_t sinceRx = havePkt ? (uint32_t)((millis() - lastRxMs) / 1000) : UINT32_MAX;

  String js;
  js.reserve(900);
  js += F("{\"linked\":");
  js += linked ? F("true") : F("false");
  js += F(",\"bearing\":");
  if (havePkt && lastData.fix && gps.location.isValid()) {
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
  js += gps.location.isValid() ? String(gps.location.lat(), 6) : F("0");
  js += F(",\"lon\":");
  js += gps.location.isValid() ? String(gps.location.lng(), 6) : F("0");
  js += F(",\"fix\":");
  js += gps.location.isValid() ? F("1") : F("0");
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
  js += F("},\"servo\":{\"angle\":");
  js += String(servoAngleDeg, 1);
  js += F(",\"target\":");
  js += String(servoTargetDeg, 1);
  js += F(",\"mode\":\"");
  js += trackModeStr(trackMode);
  js += F("\",\"calibrated\":");
  js += mountCalibrated ? F("true") : F("false");
  js += F(",\"mount_offset_deg\":");
  js += String(mountOffsetDeg, 1);
  js += F("},\"mag\":{\"online\":");
  js += magOnline ? F("true") : F("false");
  js += F(",\"heading\":");
  js += (magOnline && magHeadingDeg >= 0) ? String(magHeadingDeg, 1) : F("-1");
  js += F("}}");
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

static String buildStatusJson() {
  String js;
  js.reserve(900);

  // Server GPS quality
  js += F("{\"server_gps\":{\"fix\":");
  js += gps.location.isValid() ? F("1") : F("0");
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
  js += F("\"servo\":{\"angle\":");
  js += String(servoAngleDeg, 1);
  js += F(",\"mode\":\"");
  js += trackModeStr(trackMode);
  js += F("\",\"calibrated\":");
  js += mountCalibrated ? F("true") : F("false");
  js += F(",\"mount_offset_deg\":");
  js += String(mountOffsetDeg, 1);
  js += F("},\"mag\":{\"online\":");
  js += magOnline ? F("true") : F("false");
  js += F(",\"heading\":");
  js += (magOnline && magHeadingDeg >= 0) ? String(magHeadingDeg, 1) : F("-1");
  js += F("}}");
  return js;
}

static void initWebServer() {
  httpServer.on("/", HTTP_GET, []() {
    httpServer.send(200, "text/html", WEB_UI_HTML);
  });
  // Web app manifest -> "Add to Home screen" launches standalone (no URL bar).
  httpServer.on("/manifest.json", HTTP_GET, []() {
    httpServer.send(200, "application/manifest+json",
                    F("{\"name\":\"Shore Spotter\",\"short_name\":\"Spotter\","
                      "\"start_url\":\"/\",\"scope\":\"/\",\"display\":\"standalone\","
                      "\"orientation\":\"portrait\",\"background_color\":\"#0d1117\","
                      "\"theme_color\":\"#0d1117\"}"));
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
      uint16_t newId = (uint16_t)strtoul(idStr.c_str(), nullptr, 16);
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
    setServoAngle(a);
    servoTargetDeg = a;
    if (trackMode == MODE_IDLE) trackMode = MODE_MANUAL;
    Serial.print(F("[SERVO] manual angle="));
    Serial.println(servoAngleDeg, 1);
    httpServer.send(200, "application/json",
                    "{\"ok\":true,\"angle\":" + String(servoAngleDeg, 1) + "}");
  });
  // POST /api/track/start — lock the current aim as calibration and auto-track.
  httpServer.on("/api/track/start", HTTP_POST, []() {
    if (startTracking()) {
      httpServer.send(200, "application/json",
                      "{\"ok\":true,\"mount_offset_deg\":" +
                          String(mountOffsetDeg, 1) + "}");
    } else {
      httpServer.send(409, "application/json",
                      "{\"ok\":false,\"error\":\"need server+client GPS fix\"}");
    }
  });
  // POST /api/track/pause — hold servo at the current angle, stop auto updates.
  httpServer.on("/api/track/pause", HTTP_POST, []() {
    if (trackMode == MODE_TRACKING) trackMode = MODE_PAUSED;
    Serial.println(F("[TRACK] paused"));
    httpServer.send(200, "application/json",
                    "{\"ok\":true,\"mode\":\"" + String(trackModeStr(trackMode)) +
                        "\"}");
  });
  // POST /api/track/resume — resume auto-tracking with the existing calibration.
  httpServer.on("/api/track/resume", HTTP_POST, []() {
    if (!mountCalibrated) {
      httpServer.send(409, "application/json",
                      "{\"ok\":false,\"error\":\"not calibrated, use start\"}");
      return;
    }
    trackMode = MODE_TRACKING;
    updateTracking();
    Serial.println(F("[TRACK] resumed"));
    httpServer.send(200, "application/json",
                    "{\"ok\":true,\"mode\":\"tracking\"}");
  });
  // POST /api/track/stop — return to manual control (keeps calibration).
  httpServer.on("/api/track/stop", HTTP_POST, []() {
    trackMode = MODE_MANUAL;
    Serial.println(F("[TRACK] stopped -> manual"));
    httpServer.send(200, "application/json",
                    "{\"ok\":true,\"mode\":\"manual\"}");
  });
  httpServer.onNotFound([]() {
    httpServer.sendHeader("Location", "/");
    httpServer.send(302);
  });
  httpServer.begin();
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
  display.setI2CAddress(OLED_I2C_ADDR << 1);
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
  display.setI2CAddress(OLED_I2C_ADDR << 1);
  display.begin();
#endif
  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);
  display.drawStr(16, 28, "LOW BATTERY");
  display.drawStr(10, 46, "Shutting down");
  display.sendBuffer();
  Serial.print(F("[PWR] LOW BATTERY ("));
  Serial.print(cachedBatteryMv);
  Serial.println(F(" mV) -> power off"));
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

// Power up the OLED rail and bring up the SH1106. Returns false if it fails.
static bool enableClientOled() {
  if (!pmuOnline) return false;
  pmu.setALDO1Voltage(3300);
  pmu.enableALDO1();
  delay(100);
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  display.setI2CAddress(OLED_I2C_ADDR << 1);
  if (!display.begin()) { pmu.disableALDO1(); return false; }
  return true;
}

static void showClientBootScreen() {
  if (!enableClientOled()) return;
  drawClientInfoScreen();

  delay(10000);

  display.clearBuffer();
  display.sendBuffer();
}

// Short-press PWR wakes the screen for CLIENT_SCREEN_WAKE_MS (non-blocking).
// The loop() redraws/refreshes it and turns the rail back off when it expires.
static void wakeClientScreen() {
  if (!enableClientOled()) return;
  clientOledAwake = true;
  clientOledOffMs = millis() + CLIENT_SCREEN_WAKE_MS;
  nextClientOledRefreshMs = 0;  // force an immediate redraw
}
#endif

void setup() {
  Serial.begin(115200);
  delay(1200);
  bootMs = millis();
  nodeId = derivedNodeId();

  if (!initRadio()) {
    while (true) {
      delay(1000);
    }
  }

#if defined(ROLE_CLIENT)
  loadClientSettings();
  radio.setOutputPower(currentTxPowerDbm);
  nextAtpcEvalMs = millis() + ATPC_EVAL_MS;

  pinMode(GPS_EN_PIN, OUTPUT);
  digitalWrite(GPS_EN_PIN, HIGH);

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

  Serial.println(F("[CLIENT] mode active: send position packets"));
  Serial.print(F("[CLIENT] node id (chip MAC last 2 bytes) = 0x"));
  Serial.println(nodeId, HEX);
  Serial.println(F("[CLIENT] Add this id to SERVER CLIENT_WHITELIST to authorise."));
  Serial.print(F("[CLIENT] GPS UART baud="));
  Serial.println(GPS_BAUD);
  Serial.print(F("[CLIENT] TX power="));
  Serial.print(currentTxPowerDbm);
  Serial.print(F(" dBm (ATPC="));
  Serial.print(atpcEnabled ? F("on") : F("off"));
  Serial.println(F(")"));
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
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  envSensorOnline = initEnvSensor();
  sampleEnvSensor();
  nextEnvMs = millis() + ENV_UPDATE_MS;
  magOnline = initMag();           // QMC6310 station heading (shared I2C bus 0)
  nextMagMs = millis() + MAG_SAMPLE_MS;
  initServo();                     // LEDC PWM on IO21, centre the camera
  display.setI2CAddress(OLED_I2C_ADDR << 1);
  display.begin();
  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);
  display.drawStr(0, 12, "SHORE SPOTTER");
  display.drawStr(0, 30, "SERVER booting...");
  display.sendBuffer();

  cachedBatteryMv = readBatteryMilliVolts();
  if (batteryCriticallyLow()) showLowBatteryAndPowerOff();  // refuse to boot empty

  nextServerIdleLogMs = millis() + SERVER_IDLE_LOG_MS;
  nextDisplayMs = millis() + DISPLAY_REFRESH_MS;
  Serial.println(F("[SERVER] mode active: receive position packets"));
  loadServerSettings();
  Serial.print(F("[SERVER] whitelist ("));
  Serial.print(clientWhitelistCount);
  Serial.print(F(" entries): "));
  for (size_t i = 0; i < clientWhitelistCount; i++) {
    Serial.print(F("0x"));
    Serial.print(clientWhitelist[i], HEX);
    if (i + 1 < clientWhitelistCount) Serial.print(' ');
  }
  Serial.println();

  // Connect to the phone-provided hotspot in station mode.
  // Credentials come from include/wifi_config.h (WIFI_SSID / WIFI_PASSWORD).
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print(F("[WiFi] Connecting to hotspot \""));
  Serial.print(WIFI_SSID);
  Serial.print(F("\" "));
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiStart < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);
  display.drawStr(0, 12, "SHORE SPOTTER");
  display.drawHLine(0, 14, 128);
  if (WiFi.status() == WL_CONNECTED) {
    cachedApIp = WiFi.localIP().toString();
    Serial.print(F("[WiFi] Connected. IP: "));
    Serial.println(cachedApIp);
    Serial.print(F("[WiFi] Open browser -> http://"));
    Serial.println(cachedApIp);
    display.drawStr(0, 32, "WiFi connected");
    display.drawStr(0, 48, cachedApIp.c_str());
  } else {
    cachedApIp = "";
    Serial.println(F("[WiFi] Hotspot connect FAILED."));
    Serial.println(F("[WiFi] Check SSID/password in include/wifi_config.h."));
    Serial.println(F("[WiFi] LoRa tracking still runs; WiFi will auto-retry."));
    display.drawStr(0, 32, "WiFi FAILED");
    display.drawStr(0, 48, "see wifi_config.h");
  }
  display.sendBuffer();
  delay(2000);

  initWebServer();

  // Arm non-blocking, interrupt-driven reception.
  radio.setDio1Action(onLoRaDio1);
  radio.startReceive();
#endif
}

void loop() {
#if defined(ROLE_CLIENT)
  serviceGps();

  if (millis() >= nextBatteryMs) {
    nextBatteryMs = millis() + BATTERY_UPDATE_MS;
    cachedBatteryMv = readBatteryMilliVolts();
    Serial.print(F("[CLIENT] Battery update mV="));
    Serial.println(cachedBatteryMv);
    checkLowBatteryAndMaybeShutdown();
  }

  if (millis() >= nextEnvMs) {
    nextEnvMs = millis() + ENV_UPDATE_MS;
    sampleEnvSensor();
  }

  if (millis() >= nextSendMs) {
    nextSendMs = millis() + SEND_INTERVAL_MS;

    uint8_t buf[DATA_PACKET_LEN];
    size_t len = buildDataPacket(buf);
    int state = radio.transmit(buf, len);

    if (state == RADIOLIB_ERR_NONE) {
      Serial.print(F("[CLIENT] TX ok seq="));
      Serial.print((uint16_t)(txSeq - 1));
      Serial.print(F(" | GPS fix="));
      Serial.print(gps.location.isValid() ? 1 : 0);
      Serial.print(F(" sats="));
      Serial.print(gps.satellites.isValid() ? (int)gps.satellites.value() : -1);
      Serial.print(F(" hdop="));
      if (gps.hdop.isValid()) Serial.print(gps.hdop.hdop(), 1);
      else Serial.print(F("--"));
      Serial.print(F(" age="));
      Serial.print(gps.location.age());
      Serial.print(F("ms spd="));
      Serial.print(gps.speed.isValid() ? gps.speed.mps() : 0.0, 1);
      Serial.print(F("m/s crs="));
      Serial.print(gps.course.isValid() ? gps.course.deg() : 0.0, 0);
      Serial.print(F(" | LoRa up(srv heard us) rssi="));
      Serial.print(lastAckRssiDbm10 / 10.0f, 1);
      Serial.print(F(" snr="));
      Serial.print(lastAckSnrDb10 / 10.0f, 1);
      Serial.print(F(" txpwr="));
      Serial.print(currentTxPowerDbm);
      Serial.print(F("dBm acks="));
      Serial.print(ackRxCount);
      Serial.print(F(" miss="));
      Serial.println(ackMissCount);
    } else {
      Serial.print(F("[CLIENT] TX failed, code="));
      Serial.println(state);
    }
  }

  if (millis() >= nextTelemetryMs) {
    nextTelemetryMs = millis() + TELEMETRY_INTERVAL_MS;
    uint8_t tbuf[TELEMETRY_PACKET_LEN];
    size_t tlen = buildTelemetryPacket(tbuf);
    int tstate = radio.transmit(tbuf, tlen);
    if (tstate == RADIOLIB_ERR_NONE) {
      Serial.print(F("[CLIENT] TELEMETRY TX ok batt_mV="));
      Serial.println(cachedBatteryMv);
    } else {
      Serial.print(F("[CLIENT] TELEMETRY TX failed, code="));
      Serial.println(tstate);
    }
  }

  uint8_t ackBuf[ACK_PACKET_LEN];
  int ackState = radio.receive(ackBuf, sizeof(ackBuf));
  if (ackState == RADIOLIB_ERR_NONE) {
    size_t n = radio.getPacketLength();
    if (n > sizeof(ackBuf)) n = sizeof(ackBuf);
    AckPayload ack{};
    if (parseAckPacket(ackBuf, n, ack)) {
      lastAckedSeq = ack.ackSeq;
      lastAckRxMs = millis();
      ackRxCount++;
      lastAckRssiDbm10 = ack.rssiDbm10;
      lastAckSnrDb10 = ack.snrDb10;
      Serial.print(F("[CLIENT] ACK seq="));
      Serial.print(ack.ackSeq);
      Serial.print(F(" | up(srv heard us) rssi="));
      Serial.print(ack.rssiDbm10 / 10.0f, 1);
      Serial.print(F(" snr="));
      Serial.print(ack.snrDb10 / 10.0f, 1);
      Serial.print(F(" | down(we heard ack) rssi="));
      Serial.print(radio.getRSSI(), 1);
      Serial.print(F(" snr="));
      Serial.println(radio.getSNR(), 1);
    }
  }
  if (txSeq > 5 && (millis() - lastAckRxMs > 15000) &&
      (millis() - lastAckMissMarkMs > 5000)) {
    ackMissCount++;
    lastAckMissMarkMs = millis();
  }

  evaluateAtpc();

  // PWR key: short-press wakes the screen 10 s, long-press shuts down.
  if (pmuOnline) {
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

  // Keep the woken screen refreshed, then power the rail back off when it expires.
  if (clientOledAwake) {
    if (millis() >= clientOledOffMs) {
      display.clearBuffer();
      display.sendBuffer();
      clientOledAwake = false;
    } else if (millis() >= nextClientOledRefreshMs) {
      nextClientOledRefreshMs = millis() + DISPLAY_REFRESH_MS;
      drawClientInfoScreen();
    }
  }
#endif

#if defined(ROLE_SERVER)
  httpServer.handleClient();
  serviceGps();  // keep the server's own GPS position fresh

  if (millis() >= nextEnvMs) {
    nextEnvMs = millis() + ENV_UPDATE_MS;
    sampleEnvSensor();
  }

  if (millis() >= nextMagMs) {
    nextMagMs = millis() + MAG_SAMPLE_MS;
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
    if (cachedApIp.isEmpty()) {
      cachedApIp = WiFi.localIP().toString();
    }
  } else {
    cachedApIp = "";
    if (millis() >= nextWifiRetryMs) {
      nextWifiRetryMs = millis() + WIFI_RETRY_INTERVAL_MS;
      wifiReconnectingUntilMs = millis() + 2000;  // show "reconnecting" briefly
      WiFi.reconnect();
    }
  }

  // Long-press PWR → show shutdown screen then power off
  if (pmuOnline) {
    pmu.getIrqStatus();
    if (pmu.isPekeyLongPressIrq()) {
      pmu.clearIrqStatus();
      showShutdownAndPowerOff();
    }
  }

  // Interrupt-driven RX: the DIO1 ISR sets rxDoneFlag; the loop stays
  // non-blocking so httpServer.handleClient() above replies instantly.
  if (rxDoneFlag) {
    rxDoneFlag = false;
    uint8_t buf[DATA_PACKET_LEN];
    int state = radio.readData(buf, sizeof(buf));

    if (state == RADIOLIB_ERR_NONE) {
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
      updateTracking();  // steer the camera servo toward the surfer (if tracking)

      uint8_t ackBuf[ACK_PACKET_LEN];
      size_t ackLen = buildAckPacket(ackBuf, d.srcId, d.seq, lastRssi, lastSnr);
      if (radio.transmit(ackBuf, ackLen) == RADIOLIB_ERR_NONE) {
        ackTxCount++;
      }

      Serial.print(F("[SERVER] RX DATA | from=0x"));
      Serial.print(d.srcId, HEX);
      Serial.print(F(" seq="));
      Serial.print(d.seq);
      Serial.print(F(" lat="));
      Serial.print(d.lat, 6);
      Serial.print(F(" lon="));
      Serial.print(d.lon, 6);
      Serial.print(F(" fix="));
      Serial.print(d.fix);
      Serial.print(F(" sats="));
      Serial.print(d.satellites != 0xFF ? (int)d.satellites : -1);
      Serial.print(F(" hdop="));
      if (d.hdop10 != 0xFF) Serial.print(d.hdop10 / 10.0f, 1);
      else Serial.print(F("--"));
      Serial.print(F(" spd="));
      Serial.print(d.speedCmS);
      Serial.print(F("cm/s crs="));
      Serial.print(d.courseDeg10 / 10.0f, 0);
      Serial.print(F(" acc="));
      Serial.print(d.accelCmS2 / 100.0f, 2);
      Serial.print(F("m/s2 | LoRa rssi="));
      Serial.print(lastRssi);
      Serial.print(F(" snr="));
      Serial.print(lastSnr);
      Serial.print(F(" | SRV-GPS fix="));
      Serial.print(gps.location.isValid() ? 1 : 0);
      Serial.print(F(" sats="));
      Serial.print(gps.satellites.isValid() ? (int)gps.satellites.value() : -1);
      Serial.print(F(" hdop="));
      if (gps.hdop.isValid()) Serial.print(gps.hdop.hdop(), 1);
      else Serial.print(F("--"));
      Serial.print(F(" | drop="));
      Serial.print(rxDropCount);
      Serial.print(F(" err="));
      Serial.println(rxErrorCount);
    } else {
      DecodedTelemetry t{};
      if (parseTelemetryPacket(buf, n, t)) {
        lastTelemetry = t;
        haveTelemetry = true;
        lastTelemetryRxMs = millis();
        rxTelemetryCount++;
        Serial.print(F("[SERVER] RX TELEMETRY | from=0x"));
        Serial.print(t.srcId, HEX);
        Serial.print(F(" batt_mV="));
        Serial.print(t.batteryMv);
        Serial.print(F(" temp="));
        if (t.tempC != INT8_MIN) {
          Serial.print((int)t.tempC);
          Serial.print(F("C hum="));
          Serial.print(t.humidityPct);
          Serial.println('%');
        } else {
          Serial.println(F("N/A"));
        }
      } else {
        rxDropCount++;
        Serial.println(F("[SERVER] RX dropped (foreign/invalid packet)"));
      }
    }
    } else {
      rxErrorCount++;
      Serial.print(F("[SERVER] RX failed, code="));
      Serial.println(state);
    }
    rxDoneFlag = false;       // ignore the ACK's own TxDone pulse on DIO1
    radio.startReceive();     // re-arm for the next packet
  }

  // Periodic idle log when no packet has arrived for a while.
  if (millis() >= nextServerIdleLogMs &&
      (!havePkt || millis() - lastRxMs > 2500)) {
    nextServerIdleLogMs = millis() + SERVER_IDLE_LOG_MS;
    Serial.print(F("[SERVER] idle | mode="));
    Serial.print(trackModeStr(trackMode));
    Serial.print(F(" SRV-GPS fix="));
    Serial.print(gps.location.isValid() ? 1 : 0);
    Serial.print(F(" sats="));
    Serial.print(gps.satellites.isValid() ? (int)gps.satellites.value() : -1);
    Serial.print(F(" hdop="));
    if (gps.hdop.isValid()) Serial.print(gps.hdop.hdop(), 1);
    else Serial.print(F("--"));
    Serial.print(F(" head="));
    if (magOnline && magHeadingDeg >= 0) Serial.print(magHeadingDeg, 1);
    else Serial.print(F("N/A"));
    Serial.print(F(" servo="));
    Serial.print(servoAngleDeg, 1);
    Serial.println(F(" (waiting for client packets)"));
  }

  if (millis() >= nextDisplayMs) {
    nextDisplayMs = millis() + DISPLAY_REFRESH_MS;
    renderServerDisplay();
  }
#endif
}