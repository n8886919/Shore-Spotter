#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#if defined(ROLE_SERVER)
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>
#include "web_ui.h"
#endif
#include "secrets.h"

// IMPORTANT:
// This project keeps one main.cpp and splits behavior by build flags:
// ROLE_CLIENT (water side) / ROLE_SERVER (shore side).
// Upload env:tbeam-client or env:tbeam-server to each board.

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

constexpr uint32_t SEND_INTERVAL_MS = 1000;
constexpr uint32_t TELEMETRY_INTERVAL_MS = 10000;  // battery + env packet rate
constexpr uint32_t BATTERY_UPDATE_MS = 5000;
constexpr uint32_t SERVER_IDLE_LOG_MS = 5000;
constexpr uint32_t LINK_TIMEOUT_MS = 5000;
constexpr uint32_t DISPLAY_REFRESH_MS = 500;

#if defined(ROLE_SERVER)
constexpr uint32_t TRACK_INTERVAL_MS = 10000;  // 1 pt / 10 s -> 720 pts = 2 hr
constexpr size_t   TRACK_CAP = 720;
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

// Shared OLED object — client enables it only during boot-info and shutdown screens.
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

struct __attribute__((packed)) PacketHeader {
  uint8_t magic;
  uint8_t version;
  uint8_t networkId;
  uint16_t srcId;
  uint16_t dstId;
  uint8_t msgType;
  uint16_t seq;
  uint8_t payloadLen;
};

struct __attribute__((packed)) PositionPayload {
  int32_t  latE7;        // latitude  * 1e7 (fixed-point, exact and compact)
  int32_t  lonE7;        // longitude * 1e7
  uint8_t  fix;          // GPS fix (0/1)
  uint16_t speedCmS;     // ground speed, cm/s
  uint16_t courseDeg10;  // track angle 0.1° units (0–3599)
  int16_t  accelCmS2;    // smoothed acceleration, cm/s²
};

// Battery + environment sent separately every 10 s to reduce 1 Hz packet size.
struct __attribute__((packed)) TelemetryPayload {
  uint16_t batteryMv;    // battery voltage mV (0 = unknown)
  int16_t  tempC10;      // temperature × 10 (0.1 °C resolution, INT16_MIN = no sensor)
  uint8_t  humidityPct;  // relative humidity 0–100 %, 0xFF = no sensor
};

// ---------------------------------------------------------------------------
// Application protocol (stage 1: binary header, ready for 1->many / many->many)
// ---------------------------------------------------------------------------
// Grouping works on two layers:
//   * RF_SYNC_WORD (above) = PHY-layer coarse filter, cheap power saving.
//   * NETWORK_ID (below)   = logical group, authoritative and scalable.
constexpr uint8_t PROTO_MAGIC = 0x53;
constexpr uint8_t PROTO_VERSION = 1;
constexpr uint16_t ID_BROADCAST = 0xFFFF;
constexpr uint8_t MAC_LEN = 4;  // reserved for HMAC (stage 2), zero-filled now

constexpr uint8_t NETWORK_ID = 0x01;  // group id; different groups => different value
constexpr uint16_t SERVER_ID = 0x0010;   // shore station id
// nodeId is derived at runtime from the ESP32 chip MAC (last 2 bytes).
// No need to change source code per board — each chip is already unique.

enum MsgType : uint8_t {
  MSG_DATA      = 1,
  MSG_ACK       = 2,
  MSG_HELLO     = 3,
  MSG_TELEMETRY = 4,  // battery + temp/humidity, 10 s interval
};

constexpr size_t DATA_PACKET_LEN =
    sizeof(PacketHeader) + sizeof(PositionPayload) + MAC_LEN;
constexpr size_t TELEMETRY_PACKET_LEN =
    sizeof(PacketHeader) + sizeof(TelemetryPayload) + MAC_LEN;

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
};

struct DecodedTelemetry {
  uint16_t srcId;
  uint16_t batteryMv;
  int16_t  tempC10;     // INT16_MIN = no sensor
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

// Telemetry (slow path, MSG_TELEMETRY every 10 s)
DecodedTelemetry lastTelemetry{0, 0, INT16_MIN, 0xFF};
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

// Servo bearing calibration
// User centres servo, places camera station, then calls POST /api/calibrate/servo.
// The supplied compass heading is stored here; servo 90° corresponds to this bearing.
static float servoOriginDeg = 0.0f;

struct TrackPoint { float lat; float lon; };
static TrackPoint trackBuf[TRACK_CAP];
static size_t trackHead  = 0;
static size_t trackCount = 0;
static uint32_t lastTrackMs = 0;

WebServer httpServer(80);
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
  out.tempC10     = tel.tempC10;
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
  tel.tempC10     = INT16_MIN;  // TODO: read from SHT3x / BME280 via I2C
  tel.humidityPct = 0xFF;       // TODO: read from SHT3x / BME280 via I2C

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

static void renderServerDisplay() {
  uint32_t sinceRx = havePkt ? (millis() - lastRxMs) : 0;
  bool linked = havePkt && (sinceRx <= LINK_TIMEOUT_MS);

  char buf[26];
  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);

  display.drawStr(0, 10, "SHORE SPOTTER");
  display.drawHLine(0, 13, 128);

  display.drawStr(0, 26, linked ? "LINK: ONLINE" : "LINK: NO SIGNAL");

  if (havePkt) {
    snprintf(buf, sizeof(buf), "ID:%02X  %lus ago", lastData.srcId,
             (unsigned long)(sinceRx / 1000));
  } else {
    snprintf(buf, sizeof(buf), "ID: --");
  }
  display.drawStr(0, 38, buf);

  if (havePkt && lastData.fix && gps.location.isValid()) {
    double brg = computeBearing(gps.location.lat(), gps.location.lng(),
                                lastData.lat, lastData.lon);
    snprintf(buf, sizeof(buf), "Bearing: %d deg", (int)(brg + 0.5));
  } else {
    snprintf(buf, sizeof(buf), "Bearing: --");
  }
  display.drawStr(0, 50, buf);

  if (haveTelemetry) {
    snprintf(buf, sizeof(buf), "Batt: %u mV", lastTelemetry.batteryMv);
  } else {
    snprintf(buf, sizeof(buf), "Batt: --");
  }
  display.drawStr(0, 62, buf);

  display.sendBuffer();
}
#endif

#if defined(ROLE_SERVER)
static void pushTrackPoint(float lat, float lon) {
  trackBuf[trackHead] = {lat, lon};
  trackHead = (trackHead + 1) % TRACK_CAP;
  if (trackCount < TRACK_CAP) {
    trackCount++;
  }
}

static String buildTrackJson() {
  bool linked = havePkt && ((millis() - lastRxMs) <= LINK_TIMEOUT_MS);
  uint32_t sinceRx = havePkt ? (uint32_t)((millis() - lastRxMs) / 1000) : UINT32_MAX;

  String js;
  js.reserve(18000);
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
  js += F(",\"last_rx_sec\":");
  js += (havePkt && sinceRx != UINT32_MAX) ? String(sinceRx) : F("-1");
  js += F("},\"server\":{\"lat\":");
  js += gps.location.isValid() ? String(gps.location.lat(), 6) : F("0");
  js += F(",\"lon\":");
  js += gps.location.isValid() ? String(gps.location.lng(), 6) : F("0");
  js += F(",\"fix\":");
  js += gps.location.isValid() ? F("1") : F("0");
  js += F("},\"telemetry\":{\"batt_mv\":");
  js += haveTelemetry ? String(lastTelemetry.batteryMv) : F("0");
  js += F(",\"temp_c\":");
  if (haveTelemetry && lastTelemetry.tempC10 != INT16_MIN) {
    js += String(lastTelemetry.tempC10 / 10.0f, 1);
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
  js += F("},\"history\":[");
  size_t start = (trackCount == TRACK_CAP) ? trackHead : 0;
  for (size_t i = 0; i < trackCount; i++) {
    if (i > 0) {
      js += ',';
    }
    size_t idx = (start + i) % TRACK_CAP;
    js += F("{\"lat\":");
    js += String(trackBuf[idx].lat, 6);
    js += F(",\"lon\":");
    js += String(trackBuf[idx].lon, 6);
    js += '}';
  }
  js += F("]}");
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
  js.reserve(512);

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
  js += F("},");

  // Servo calibration state
  js += F("\"servo_origin_deg\":");
  js += String(servoOriginDeg, 1);
  js += '}';
  return js;
}

static void initWebServer() {
  httpServer.on("/", HTTP_GET, []() {
    httpServer.send(200, "text/html", WEB_UI_HTML);
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
      }
    } else if (action == "remove") {
      uint16_t rmId = (uint16_t)strtoul(idStr.c_str(), nullptr, 16);
      for (size_t i = 0; i < clientWhitelistCount; i++) {
        if (clientWhitelist[i] == rmId) {
          for (size_t j = i; j < clientWhitelistCount - 1; j++) {
            clientWhitelist[j] = clientWhitelist[j + 1];
          }
          clientWhitelistCount--;
          break;
        }
      }
    } else if (action == "clear") {
      clientWhitelistCount = 0;
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
  // POST /api/calibrate/servo?heading=<degrees>
  // Steps: centre servo mechanically → place camera station → call this endpoint.
  // The supplied heading (read from compass) becomes the servo "zero" reference.
  // TODO: replace query param with auto-read from compass module (e.g. QMC5883L).
  httpServer.on("/api/calibrate/servo", HTTP_POST, []() {
    if (httpServer.hasArg("heading")) {
      servoOriginDeg = httpServer.arg("heading").toFloat();
      httpServer.send(200, "application/json",
                      "{\"ok\":true,\"servo_origin_deg\":" +
                          String(servoOriginDeg, 1) + "}");
    } else {
      httpServer.send(400, "application/json",
                      "{\"ok\":false,\"error\":\"missing heading param\"}");
    }
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

// ---------------------------------------------------------------------------
// Client boot-info screen: show MAC last 4 hex, battery voltage, temp/hum.
// Enables OLED rail, displays for 10 s, then disables rail to save power.
// ---------------------------------------------------------------------------
#if defined(ROLE_CLIENT)
static void showClientBootScreen() {
  if (!pmuOnline) return;
  pmu.setALDO1Voltage(3300);
  pmu.enableALDO1();
  delay(100);
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  display.setI2CAddress(OLED_I2C_ADDR << 1);
  if (!display.begin()) { pmu.disableALDO1(); return; }

  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char line1[20], line2[20], line3[20], line4[20];
  snprintf(line1, sizeof(line1), "ID: %02X%02X", mac[4], mac[5]);
  snprintf(line2, sizeof(line2), "Batt: %u mV", cachedBatteryMv);
  snprintf(line3, sizeof(line3), "Temp: N/A");   // TODO: SHT3x/BME280
  snprintf(line4, sizeof(line4), "Hum:  N/A");

  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);
  display.drawStr(0, 12, "SHORE SPOTTER");
  display.drawHLine(0, 14, 128);
  display.drawStr(0, 28, line1);
  display.drawStr(0, 40, line2);
  display.drawStr(0, 52, line3);
  display.drawStr(0, 64, line4);
  display.sendBuffer();

  delay(10000);

  display.clearBuffer();
  display.sendBuffer();
  pmu.disableALDO1();
}
#endif

void setup() {
  Serial.begin(115200);
  delay(1200);
  nodeId = derivedNodeId();

#if !defined(ROLE_CLIENT) && !defined(ROLE_SERVER)
  Serial.println(F("Define ROLE_CLIENT or ROLE_SERVER in build flags."));
  while (true) {
    delay(1000);
  }
#endif

  if (!initRadio()) {
    while (true) {
      delay(1000);
    }
  }

#if defined(ROLE_CLIENT)
  pinMode(GPS_EN_PIN, OUTPUT);
  digitalWrite(GPS_EN_PIN, HIGH);

  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  pmuOnline = initPmu();
  if (pmuOnline) {
    pmu.disableALDO1();  // OLED / sensor bus off on client to save power
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_LONG_IRQ);  // long-press = shutdown
  }
  cachedBatteryMv = readBatteryMilliVolts();
  nextBatteryMs = millis() + BATTERY_UPDATE_MS;

  showClientBootScreen();  // show MAC / batt / temp for 10 s then turn off OLED

  Serial.println(F("[CLIENT] mode active: send position packets"));
  Serial.print(F("[CLIENT] node id (chip MAC last 2 bytes) = 0x"));
  Serial.println(nodeId, HEX);
  Serial.println(F("[CLIENT] Add this id to SERVER CLIENT_WHITELIST to authorise."));
  Serial.print(F("[CLIENT] GPS UART baud="));
  Serial.println(GPS_BAUD);
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
  display.setI2CAddress(OLED_I2C_ADDR << 1);
  display.begin();
  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);
  display.drawStr(0, 12, "SHORE SPOTTER");
  display.drawStr(0, 30, "SERVER booting...");
  display.sendBuffer();

  nextServerIdleLogMs = millis() + SERVER_IDLE_LOG_MS;
  nextDisplayMs = millis() + DISPLAY_REFRESH_MS;
  Serial.println(F("[SERVER] mode active: receive position packets"));
  // Seed runtime whitelist from compile-time defaults
  clientWhitelistCount = 0;
  for (uint16_t id : DEFAULT_WHITELIST) {
    clientWhitelist[clientWhitelistCount++] = id;
  }
  Serial.print(F("[SERVER] whitelist ("));
  Serial.print(clientWhitelistCount);
  Serial.print(F(" entries): "));
  for (size_t i = 0; i < clientWhitelistCount; i++) {
    Serial.print(F("0x"));
    Serial.print(clientWhitelist[i], HEX);
    if (i + 1 < clientWhitelistCount) Serial.print(' ');
  }
  Serial.println();

  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print(F("[WiFi] AP SSID: "));
  Serial.println(F(AP_SSID));
  Serial.print(F("[WiFi] AP IP:   "));
  Serial.println(WiFi.softAPIP());
  Serial.println(F("[WiFi] Open browser -> 192.168.4.1"));
  initWebServer();
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
  }

  if (millis() >= nextSendMs) {
    nextSendMs = millis() + SEND_INTERVAL_MS;

    uint8_t buf[DATA_PACKET_LEN];
    size_t len = buildDataPacket(buf);
    int state = radio.transmit(buf, len);

    if (state == RADIOLIB_ERR_NONE) {
      Serial.print(F("[CLIENT] TX ok seq="));
      Serial.print((uint16_t)(txSeq - 1));
      Serial.print(F(" fix="));
      Serial.print(gps.location.isValid() ? 1 : 0);
      Serial.print(F(" spd="));
      Serial.print(gps.speed.isValid() ? gps.speed.mps() : 0.0f);
      Serial.print(F("m/s crs="));
      Serial.println(gps.course.isValid() ? gps.course.deg() : 0.0f);
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

  // Long-press PWR → show shutdown screen then power off
  if (pmuOnline) {
    pmu.getIrqStatus();
    if (pmu.isPekeyLongPressIrq()) {
      pmu.clearIrqStatus();
      showShutdownAndPowerOff();
    }
  }
#endif

#if defined(ROLE_SERVER)
  httpServer.handleClient();
  serviceGps();  // keep the server's own GPS position fresh

  // Long-press PWR → show shutdown screen then power off
  if (pmuOnline) {
    pmu.getIrqStatus();
    if (pmu.isPekeyLongPressIrq()) {
      pmu.clearIrqStatus();
      showShutdownAndPowerOff();
    }
  }

  uint8_t buf[DATA_PACKET_LEN];
  int state = radio.receive(buf, sizeof(buf));

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
      if (d.fix && (millis() - lastTrackMs >= TRACK_INTERVAL_MS)) {
        lastTrackMs = millis();
        pushTrackPoint((float)d.lat, (float)d.lon);
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
      Serial.print(F(" spd="));
      Serial.print(d.speedCmS);
      Serial.print(F("cm/s rssi="));
      Serial.print(lastRssi);
      Serial.print(F(" snr="));
      Serial.println(lastSnr);
    } else {
      DecodedTelemetry t{};
      if (parseTelemetryPacket(buf, n, t)) {
        lastTelemetry = t;
        haveTelemetry = true;
        lastTelemetryRxMs = millis();
        Serial.print(F("[SERVER] RX TELEMETRY | from=0x"));
        Serial.print(t.srcId, HEX);
        Serial.print(F(" batt_mV="));
        Serial.print(t.batteryMv);
        Serial.print(F(" temp="));
        if (t.tempC10 != INT16_MIN) {
          Serial.print(t.tempC10 / 10.0f, 1);
          Serial.print(F("C hum="));
          Serial.print(t.humidityPct);
          Serial.println('%');
        } else {
          Serial.println(F("N/A"));
        }
      } else {
        Serial.println(F("[SERVER] RX dropped (foreign/invalid packet)"));
      }
    }
  } else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
    if (millis() >= nextServerIdleLogMs) {
      nextServerIdleLogMs = millis() + SERVER_IDLE_LOG_MS;
      Serial.println(F("[SERVER] waiting for client packets..."));
    }
  } else if (state != RADIOLIB_ERR_RX_TIMEOUT) {
    Serial.print(F("[SERVER] RX failed, code="));
    Serial.println(state);
  }

  if (millis() >= nextDisplayMs) {
    nextDisplayMs = millis() + DISPLAY_REFRESH_MS;
    renderServerDisplay();
  }
#endif
}