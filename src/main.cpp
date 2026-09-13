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
#include "firmware_version.h"
#include "geo_math.h"  // pure maths (angles / bearing / circle fit / grading)
#include "tracking_policy.h"
#include "loop_metrics.h"
#include "gnss_snapshot.h"
#include "lora_schedule.h"
#include "async_lora_ack.h"

#if defined(ROLE_SERVER)
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include "alerts.h"    // 現場提醒的門檻與分級
#include "uart_servo_mode.h"  // UART input, shared with GPS tracking
#include "servo_motion.h"
#include "control_cadence.h"
#include "command_freshness.h"
#include "client_binding.h"
#include "packet_diagnostics.h"
#include "http_timing.h"
#include "magnetic_declination.h"
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

constexpr uint32_t SEND_INTERVAL_MS = 500;  // RF 2 Hz; GNSS epochs are measured separately
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

// DATA owns its sequence. Telemetry cannot consume an ACK slot; selection stays
// aligned even through packet loss and uint16 wrap (N must divide 65536).
constexpr uint16_t ACK_EVERY_N = 8;
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
// The GNSS collector discards queued input after a >200 ms servicing gap.
// RF TX is non-blocking; this buffer is a bound, not a guarantee against loss.
constexpr size_t GPS_RX_BUFFER_BYTES = 1024;
constexpr uint32_t GPS_BACKLOG_GUARD_MS = 200;
constexpr uint16_t I2C_TRANSACTION_TIMEOUT_MS = 10;

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
constexpr uint32_t SERVO_PWM_HZ = servo_profile::kPwmHz;
// ESP32-S3 LEDC timers support at most 14-bit resolution.  A 16-bit attach is
// rejected by Arduino-ESP32 3.x, leaving the pin with no PWM output.
constexpr uint8_t SERVO_PWM_RES_BITS = servo_profile::kPwmResolutionBits;
constexpr int SERVO_LEDC_CH = 0;     // LEDC channel (arduino-esp32 2.x)
// Camera tracking loop. The servo is refreshed far faster than position packets
// arrive (2 Hz RF, independently of GNSS epochs): between fresh packets the
// surfer's position is dead-reckoned from the
// velocity vector already carried in PositionPayload, so the camera pans
// continuously instead of stepping once per received packet — and keeps panning
// through a dropped packet instead of freezing for a whole second.
constexpr uint32_t TRACK_UPDATE_MS = control_cadence::kGpsPeriodMs;
constexpr float DR_MIN_SPEED_CMS = 30.0f;     // below this the GPS course is noise
constexpr float DR_MAX_AGE_S = 2.0f;          // source + RF + receive age ceiling
constexpr const char *GPS_PREDICTION_KEY = "gpspredict";

#endif

// GPS UART defaults (common on T-Beam family, override if your board differs).
// Freshness comes from the coherent NMEA collector, not TinyGPS's latched
// isValid(). Repeated epochs do not renew it; the caller also budgets the
// UART age uncertainty. RF retransmission never makes an old fix new.
constexpr uint32_t GPS_FIX_MAX_AGE_MS = tracking_policy::kGpsFreshMs;

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
// 任何錯誤訊息。detectOledAddress() 獨立偵測螢幕，不初始化板上磁力計。
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
  uint8_t  satellites;  // class lower bound for gates only; never exposed as an exact count
  uint8_t  satelliteClass;
  uint8_t  hdop10;
  uint8_t  age10ms;
  bool     velocityValid;
};

struct DecodedTelemetry {
  uint16_t srcId;
  uint16_t batteryMv;
  int8_t   tempC;       // INT8_MIN = no sensor
  uint8_t  humidityPct; // 0xFF = no sensor
  uint8_t  satellites;  // exact low-rate diagnostic; never used to gate live DATA
};

// Exactly one bound GPS client. srcId remains in the wire protocol so future
// multi-client support can reuse this filter and select a ClientState explicitly.
constexpr uint16_t DEFAULT_GPS_CLIENT_ID = 0;  // a new station must be explicitly paired
static uint16_t gpsClientId = DEFAULT_GPS_CLIENT_ID;  // 0 = intentionally unbound
static bool isClientAllowed(uint16_t id) {
  return gpsClientId != 0 && id == gpsClientId;
}
#endif

uint16_t txSeq = 0;
uint16_t telemetrySeq = 0;
uint16_t diagnosticSeq = 0;
uint32_t nextDiagnosticMs = 0;
uint32_t nextSendMs = 0;
uint32_t lastSendMs = 0;  // start of the last position TX (telemetry slot anchor)
uint32_t nextTelemetryMs = 0;
uint32_t nextPmuKeyMs = 0;
uint32_t nextBatteryMs = 0;
// Telemetry quiet slot, filled in by computeAirtimeBudget() at boot.
uint32_t telemetrySlotMinMs = 0;
uint32_t telemetrySlotMaxMs = 0;
uint32_t dataAirtimeMs = 0, ackAirtimeMs = 0, telemetryAirtimeMs = 0;
uint32_t diagnosticAirtimeMs = 0;
uint32_t dataSkippedSlots = 0;
static gnss_snapshot::Collector gnssCollector;
static uint32_t gpsLastServiceMs = 0, gpsBacklogDrops = 0;
static bool gpsServiceStarted = false;
uint16_t cachedBatteryMv = 0;
bool pmuOnline = false;
uint16_t nodeId = 0;  // set in setup() from chip MAC last 2 bytes

static uint32_t bootMs = 0;

#if defined(ROLE_CLIENT)
static uint32_t lastAckRxMs = 0;
static uint32_t ackRxCount = 0;
static uint32_t ackMissCount = 0;
static uint32_t lastAckMissMarkMs = 0;
static int8_t currentTxPowerDbm = TX_POWER_DBM;
static bool atpcEnabled = true;
static int16_t lastAckRssiDbm10 = -1270;
static int8_t lastAckSnrQuarterDb = -128;
static lora_schedule::AckWindow expectedAck;
static uint32_t ackRejectedCount = 0, clientTxCount = 0, clientTxErrors = 0;
static uint32_t nextClientRxRetryMs = 0;
static bool clientRxReady = false, haveDataSent = false, lastDataAckCycle = false;
static bool clientSendingData = false;
static uint16_t sendingDataSeq = 0;
static uint8_t clientTxBuffer[DATA_PACKET_LEN];
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
static async_lora_ack::Transmitter<SX1262> clientTransmitter(
    radio, clientRxFlag, RADIOLIB_SX126X_IRQ_TX_DONE,
    RADIOLIB_SX126X_IRQ_TIMEOUT, RADIOLIB_ERR_TX_TIMEOUT);
#endif

// Derive a node id from the last 2 bytes of the ESP32's factory-burned MAC.
// 65536 possible values — collision probability negligible for any real deployment.
static uint16_t derivedNodeId() {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  uint16_t id = ((uint16_t)mac[4] << 8) | mac[5];
  // The 16-bit suffix is not globally unique. Reserved values need remapping;
  // the operator still pairs the actual Client ID explicitly.
  return id == 0 || id == ID_BROADCAST || id == SERVER_ID ? id ^ 0x0100 : id;
}

#if defined(ROLE_SERVER)
// Per-client records are grouped for future extension; one instance is active.
struct ClientState {
  DecodedData position{};
  DecodedTelemetry telemetry{0, 0, INT8_MIN, 0xFF, 0xFF};
  bool havePosition = false;
  bool haveTelemetry = false;
  uint32_t positionRxMs = 0;
  uint32_t telemetryRxMs = 0;
  float rssi = 0;
  float snr = 0;
  int humidityBaselinePct = -1;
};
static ClientState gpsClient;
// Existing formatters use aliases to the one active record.
static DecodedData &lastData = gpsClient.position;
static bool &havePkt = gpsClient.havePosition;
static uint32_t &lastRxMs = gpsClient.positionRxMs;
static float &lastRssi = gpsClient.rssi;
static float &lastSnr = gpsClient.snr;
// Raw event measurements may come from another client or a rejected frame.
static float receivedPacketRssi = 0;
static float receivedPacketSnr = 0;
uint32_t nextDisplayMs = 0;
uint32_t nextWifiRetryMs = 0;
uint32_t wifiReconnectingUntilMs = 0;
bool otaReady = false;
uint32_t nextServerIdleLogMs = 0;

static DecodedTelemetry &lastTelemetry = gpsClient.telemetry;
static bool &haveTelemetry = gpsClient.haveTelemetry;
static uint32_t &lastTelemetryRxMs = gpsClient.telemetryRxMs;
static int &clientHumBaselinePct = gpsClient.humidityBaselinePct;
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

// Increasing Servo angle turns the camera CCW. The fixed-tripod magnetic
// reference is entered from the compass on the lens, and kept only in RAM.
using TrackMode = tracking_policy::Mode;
static TrackMode trackMode = TrackMode::Manual;
static TrackMode lastTrackingMode = TrackMode::Uart;
static tracking_policy::Source controlSource = tracking_policy::Source::Hold;
static tracking_policy::Selector sourceSelector;
static float servoAngleDeg = 90.0f;
static float servoTargetDeg = 90.0f;
static float mountOffsetDeg = 0.0f;
static float declinationDeg = 0.0f;
static bool declinationReady = false;
static bool mountCalibrated = false;
static bool gpsPredictionEnabled = true;  // alpha = 1; false selects alpha = 0
static bool servoPwmReady = false;
static control_cadence::GpsCadence gpsCadence;
static uint32_t servoLastDuty=UINT32_MAX;
static uint8_t oledNextRow=8;
static uint32_t oledFrames=0;
static servo_motion::Controller servoMotion;
static command_freshness::HttpGate commandGate;
static command_freshness::RadioSequence gpsSequence;
static uint32_t controlBootId = 0;
static uint32_t rejectedMotionCommands = 0, rejectedGpsSequence = 0;
static uart_servo_mode::Endpoint uartServoMode;

struct MetricsClock {
  static uint32_t micros() { return ::micros(); }
  static uint32_t nowUs() { return ::micros(); }
};
using MeasureDuration = loop_metrics::Measure<MetricsClock>;
static loop_metrics::Gap controlGap;
static loop_metrics::Duration loopDuration, httpDuration, envDuration;
static loop_metrics::Duration pmuDuration, oledDuration, loraDuration, otaDuration, motionDuration;

static uint32_t rxDataCount = 0;
static uint32_t rxTelemetryCount = 0;
static uint32_t rxDropCount = 0;
static uint32_t rxErrorCount = 0;
static uint32_t ackTxCount = 0;
static uint32_t ackErrorCount = 0;
static uint32_t ackSkippedCount = 0;
static packet_diagnostics::Ring<64> packetEvents;
static uint32_t rejectedLength = 0, rejectedFormat = 0, rejectedBinding = 0;
static uint32_t invalidFixPackets = 0, invalidVelocityPackets = 0;
static uint32_t lastDataIntervalMs = 0, maxDataIntervalMs = 0;
static uint32_t sourceEpochUpdates = 0, lastSourceEpochIntervalMs = 0;
static uint32_t lastEstimatedSourceMs = 0;
static bool haveSourceEstimate = false;
static DiagnosticPayload lastClientDiagnostic{};
static bool haveClientDiagnostic = false;
static uint32_t lastClientDiagnosticMs = 0, rxDiagnosticCount = 0;
static uint32_t sequenceMissing = 0, sequenceResyncs = 0, rxWinMissing = 0;
static int16_t lastAckError = 0;
static bool serverRxReady = true;
static uint32_t nextServerRxRetryMs = 0;
constexpr uint32_t ACK_START_MAX_AGE_MS = 50;

// Per-minute RX summary window (see RX_SUMMARY_MS).
static uint32_t nextRxSummaryMs = 0;
static uint32_t rxWinData = 0, rxWinTelem = 0, rxWinDrop = 0, rxWinErr = 0;
static uint32_t rxWinAck = 0;
static int      rxWinLastErr = 0;
static uint16_t rxWinFirstSeq = 0, rxWinLastSeq = 0;
static bool     rxWinHaveSeq = false;
static float    rxWinRssiMin = 0, rxWinRssiMax = 0, rxWinSnrMin = 0, rxWinSnrMax = 0;
static double   rxWinRssiSum = 0, rxWinSnrSum = 0;

http_timing::Server<WebServer, MetricsClock> httpServer(80);

// DIO1 reports either RxDone or TxDone. The ACK state owns it while sending.
volatile bool serverRadioIrq = false;
volatile uint32_t serverRadioIrqMs = 0;
void IRAM_ATTR onLoRaDio1() {
  serverRadioIrqMs = millis();
  serverRadioIrq = true;
}
static async_lora_ack::Transmitter<SX1262> ackTransmitter(
    radio, serverRadioIrq, RADIOLIB_SX126X_IRQ_TX_DONE,
    RADIOLIB_SX126X_IRQ_TIMEOUT, RADIOLIB_ERR_TX_TIMEOUT);
static uint8_t ackPacketBuffer[ACK_PACKET_LEN];
#endif

#if defined(ROLE_SERVER)
// Decode helpers enforce exact v4 wire lengths before any payload is used.
static bool parseDataPacket(const uint8_t *buf, size_t n, DecodedData &out) {
  PacketHeader hdr{}; PositionPayload pos{};
  if (!protocol::decodeData(buf, n, hdr, pos) || !isClientAllowed(hdr.clientId)) return false;
  out.srcId = hdr.clientId; out.seq = hdr.seq;
  out.fix = pos.fix; out.lat = pos.latE6 / 1e6; out.lon = pos.lonE6 / 1e6;
  out.speedCmS = pos.speedDmS == 255 ? UINT16_MAX : uint16_t(pos.speedDmS) * 10;
  out.courseDeg10 = pos.courseDeg10;
  out.satelliteClass = pos.satelliteClass;
  out.satellites = protocol::satLowerBound(pos.satelliteClass);
  out.hdop10 = pos.hdop10; out.age10ms = pos.age10ms;
  out.velocityValid = pos.velocityValid;
  return true;
}

static bool parseTelemetryPacket(const uint8_t *buf, size_t n, DecodedTelemetry &out) {
  PacketHeader hdr{}; TelemetryPayload tel{};
  if (!protocol::decodeTelemetry(buf, n, hdr, tel) || !isClientAllowed(hdr.clientId)) return false;
  out.srcId = hdr.clientId; out.batteryMv = tel.batteryMv;
  out.tempC = tel.tempC; out.humidityPct = tel.humidityPct; out.satellites = tel.satellites;
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
#if defined(ROLE_SERVER)
  MeasureDuration timing(pmuDuration);
#endif
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
#if defined(ROLE_SERVER)
  MeasureDuration timing(pmuDuration);
#endif
  return pmuOnline && pmu.isVbusIn();
}

static bool initPmu() {
  PMUWire.begin(PMU_SDA_PIN, PMU_SCL_PIN);
  PMUWire.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);
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
  const uint32_t now = millis();
  if (!gpsServiceStarted || now - gpsLastServiceMs > GPS_BACKLOG_GUARD_MS) {
    // Discard bytes accumulated while blocked (including the boot screen).
    // The collector waits for a new '$' and a newer epoch after this reset.
    gnssCollector.invalidate(now);
    while (GPSSerial.available() > 0) GPSSerial.read();
    if (gpsServiceStarted) ++gpsBacklogDrops;
    gpsServiceStarted = true;
  }
  gpsLastServiceMs = now;
  while (GPSSerial.available() > 0) {
    const char c = static_cast<char>(GPSSerial.read());
    gps.encode(c);  // retained for UTC date and legacy display metadata
    gnssCollector.feed(c, millis());
  }
}

// 「現在真的有定位」。見 GPS_FIX_MAX_AGE_MS —— isValid() 單獨用是不夠的。
static bool gpsFixFresh() {
  gnss_snapshot::Snapshot sample;
  return gnssCollector.sample(millis(), sample) && sample.fix &&
         sample.sourceAgeMs < GPS_FIX_MAX_AGE_MS &&
         sample.sourceAgeMs + sample.ageUncertaintyMs < GPS_FIX_MAX_AGE_MS;
}
#if defined(ROLE_SERVER)
static double stationLatitude() {
  gnss_snapshot::Snapshot sample; gnssCollector.sample(millis(), sample); return sample.lat;
}
static double stationLongitude() {
  gnss_snapshot::Snapshot sample; gnssCollector.sample(millis(), sample); return sample.lon;
}
#endif

// OLED address detection must remain independent of the removed magnetometer.
// On the N board the OLED is 0x3D and 0x3C belongs to the unused QMC sensor.
static void detectOledAddress() {
  Wire.beginTransmission(OLED_ADDR_ALT);
  oledI2CAddr = Wire.endTransmission() == 0 ? OLED_ADDR_ALT : OLED_ADDR_DEFAULT;
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
#if defined(ROLE_SERVER)
  MeasureDuration timing(envDuration);
#endif
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

static void loadGpsClientBinding() {
  if (prefs.isKey("gpsclient")) {
    const uint16_t saved = prefs.getUShort("gpsclient", 0);
    gpsClientId = client_binding::validId(saved) ? saved : 0;
    return;
  }
  // Migrate once, preserving an explicitly empty legacy whitelist.
  if (prefs.isKey("wl")) {
    const String packed = prefs.getString("wl", "");
    gpsClientId = client_binding::fromLegacyList(packed.c_str(), packed.length());
  } else {
    gpsClientId = DEFAULT_GPS_CLIENT_ID;
  }
  if (prefs.putUShort("gpsclient", gpsClientId) != sizeof(uint16_t)) {
    Log.println(F("[CLIENT] binding migration could not be saved"));
  }
}

static void loadServerSettings() {
  prefs.begin("shorespotter", false);
  loadGpsClientBinding();
  double speed=servo_motion::kDefaultSpeed;
  servo_motion::decodeSpeed(prefs.getUInt(servo_motion::kSpeedKey,0),speed);
  servoMotion.setSpeed(speed);
  const uint32_t prediction = prefs.getUInt(GPS_PREDICTION_KEY, 1);
  gpsPredictionEnabled = prediction <= 1 ? prediction == 1 : true;
  // Old acceleration/jerk/deadband profiles are deliberately ignored.
  // Old mount* and mag* NVS keys are ignored. No calibration is persisted.
  mountOffsetDeg = 0.0f;
  mountCalibrated = false;
}

static size_t buildAckPacket(uint8_t *buf, uint16_t dstId, uint16_t ackSeq,
                             float rssi, float snr) {
  const PacketHeader hdr{dstId, ackSeq, MSG_ACK};
  AckPayload ack{}; ack.ackSeq = ackSeq;
  ack.rssiDbm10 = static_cast<int16_t>(constrain(lroundf(rssi * 10), -32768L, 32767L));
  ack.snrQuarterDb = static_cast<int8_t>(constrain(lroundf(snr * 4), -128L, 127L));
  return protocol::encodeAck(buf, ACK_PACKET_LEN, hdr, ack);
}
#endif

#if defined(ROLE_CLIENT)
static bool parseAckPacket(const uint8_t *buf, size_t n, AckPayload &out) {
  PacketHeader hdr{};
  return protocol::decodeAck(buf, n, hdr, out) && hdr.clientId == nodeId &&
         expectedAck.accept(out.ackSeq, millis());
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
  clientRxReady = radio.startReceive() == RADIOLIB_ERR_NONE;
  if (!clientRxReady) nextClientRxRetryMs = millis() + 100;
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
  if (!loop_metrics::due(millis(), nextAtpcEvalMs)) return;
  nextAtpcEvalMs = millis() + ATPC_EVAL_MS;

  // No ACK for a while: push one step up.
  if (millis() - lastAckRxMs > ATPC_NO_ACK_MS) {
    applyTxPower(currentTxPowerDbm + 1);
    return;
  }

  // Strong link -> step down, weak link -> step up.
  // lastAckRssiDbm10 is negative dBm * 10.
  if (lastAckRssiDbm10 > -700 && lastAckSnrQuarterDb > 32) {
    applyTxPower(currentTxPowerDbm - 1);
  } else if (lastAckRssiDbm10 < -980 || lastAckSnrQuarterDb < 8) {
    applyTxPower(currentTxPowerDbm + 1);
  }
}
#endif

#if defined(ROLE_CLIENT)
static size_t buildDataPacket(uint8_t *buf) {
  const PacketHeader hdr{nodeId, txSeq++, MSG_DATA};
  PositionPayload pos{};
  pos.speedDmS = 255; pos.courseDeg10 = 4095;
  pos.hdop10 = 255; pos.age10ms = 255;
  gnss_snapshot::Snapshot sample;
  if (gnssCollector.sample(millis(), sample)) {
    pos.age10ms = protocol::quantizeAge(sample.sourceAgeMs);
    const bool fresh = sample.sourceAgeMs < tracking_policy::kGpsFreshMs &&
        sample.sourceAgeMs + sample.ageUncertaintyMs < tracking_policy::kGpsFreshMs;
    pos.fix = sample.fix && fresh && isfinite(sample.lat) && isfinite(sample.lon);
    if (pos.fix) {
      pos.latE6 = static_cast<int32_t>(lround(sample.lat * 1e6));
      pos.lonE6 = static_cast<int32_t>(lround(sample.lon * 1e6));
      if (!protocol::coordinatesFit(pos.latE6, pos.lonE6)) pos.fix = false;
    }
    pos.satelliteClass = fresh ? protocol::satClass(sample.satellites) : 0;
    pos.hdop10 = fresh ? protocol::quantizeHdop(sample.hdop) : 255;
    if (fresh && sample.velocityValid) {
      pos.speedDmS = protocol::quantizeSpeed(sample.speedMps);
      if (isfinite(sample.courseDeg) && sample.courseDeg >= 0 && sample.courseDeg < 360)
        pos.courseDeg10 = static_cast<uint16_t>(lround(sample.courseDeg * 10)) % 3600;
      pos.velocityValid = pos.fix && pos.speedDmS != 255 &&
          sample.speedMps >= 0.3 && pos.courseDeg10 < 3600;
    }
  }
  return protocol::encodeData(buf, DATA_PACKET_LEN, hdr, pos);
}

static size_t buildTelemetryPacket(uint8_t *buf) {
  const PacketHeader hdr{nodeId, telemetrySeq++, MSG_TELEMETRY};
  TelemetryPayload tel{};
  tel.batteryMv = cachedBatteryMv;
  tel.tempC = cachedTempC10 == INT16_MIN ? INT8_MIN :
      static_cast<int8_t>(constrain(lround(cachedTempC10 / 10.0), -127L, 127L));
  tel.humidityPct = cachedHumidityPct;
  gnss_snapshot::Snapshot sample;
  tel.satellites = gnssCollector.sample(millis(), sample) && sample.haveGga &&
      sample.sourceAgeMs + sample.ageUncertaintyMs < tracking_policy::kGpsFreshMs ? sample.satellites : 255;
  return protocol::encodeTelemetry(buf, TELEMETRY_PACKET_LEN, hdr, tel);
}
#endif

#if defined(ROLE_CLIENT)
static size_t buildDiagnosticPacket(uint8_t *buf) {
  const PacketHeader hdr{nodeId, diagnosticSeq++, MSG_DIAGNOSTIC};
  DiagnosticPayload diag{};
  const auto &stats = gnssCollector.stats();
  auto clipped = [](uint32_t n) { return static_cast<uint16_t>(n > 65535 ? 65535 : n); };
  diag.epochIntervalMs = clipped(stats.lastEpochIntervalMs);
  diag.backlogDrops = clipped(gpsBacklogDrops);
  diag.nmeaErrors = clipped(stats.rejectedSentences);
  diag.txErrors = clipped(clientTxErrors);
  diag.skippedSlots = clipped(dataSkippedSlots);
  gnss_snapshot::Snapshot sample;
  if (gnssCollector.sample(millis(), sample)) {
    const bool fresh = gpsFixFresh();
    const bool vector = fresh && sample.velocityValid && sample.speedMps >= 0.3 &&
        protocol::quantizeSpeed(sample.speedMps) != 255;
    diag.status = 1 | (fresh ? 2 : 0) | (vector ? 4 : 0) |
        (sample.haveGga ? 8 : 0) | (sample.haveRmc ? 16 : 0) | 32;
  }
  return protocol::encodeDiagnostic(buf, DIAGNOSTIC_PACKET_LEN, hdr, diag);
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
  dataAirtimeMs = (radio.getTimeOnAir(DATA_PACKET_LEN) + 999) / 1000;
  ackAirtimeMs = (radio.getTimeOnAir(ACK_PACKET_LEN) + 999) / 1000;
  telemetryAirtimeMs = (radio.getTimeOnAir(TELEMETRY_PACKET_LEN) + 999) / 1000;
  diagnosticAirtimeMs = (radio.getTimeOnAir(DIAGNOSTIC_PACKET_LEN) + 999) / 1000;
  telemetrySlotMinMs = dataAirtimeMs + TELEMETRY_SLOT_GUARD_MS;
  telemetrySlotMaxMs = SEND_INTERVAL_MS > telemetryAirtimeMs + TELEMETRY_SLOT_GUARD_MS ?
      SEND_INTERVAL_MS - telemetryAirtimeMs - TELEMETRY_SLOT_GUARD_MS : 0;
  Log.print(F("[LoRa] v4 bytes DATA/ACK/TEL="));
  Log.print(DATA_PACKET_LEN); Log.print('/'); Log.print(ACK_PACKET_LEN); Log.print('/'); Log.println(TELEMETRY_PACKET_LEN);
  Log.print(F("[LoRa] airtime_ms DATA/ACK/TEL="));
  Log.print(dataAirtimeMs); Log.print('/'); Log.print(ackAirtimeMs); Log.print('/'); Log.println(telemetryAirtimeMs);
  Log.print(F("[LoRa] RF interval_ms=")); Log.print(SEND_INTERVAL_MS);
  Log.print(F(" ACK every DATA=")); Log.println(ACK_EVERY_N);
  if (telemetrySlotMaxMs < telemetrySlotMinMs)
    Log.println(F("[LoRa] no telemetry slot: defer, never cross DATA deadline"));
}

// --- 無線電回復 ------------------------------------------------------------
// SX1262 若因為 SPI 干擾或狀態機卡住而停止工作，原本兩端都只會印一行 log 然後
// 安靜地永遠壞下去。CLIENT 平時螢幕是關的，沒有任何外部徵兆；SERVER 則是站在
// 沙灘上的人完全不知道為什麼鏡頭不動了。這裡在連續失敗到一定次數後重新初始化。
constexpr uint8_t RADIO_TX_FAIL_LIMIT = 5;    // client：連續 5 次 TX／RX 恢復失敗
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
  serverRadioIrq = false;
#endif
  if (radio.startReceive() != RADIOLIB_ERR_NONE) {
    Log.println(F("[LoRa] ERROR: RX restart failed after re-init"));
    return false;
  }
  radioFailStreak = 0;
  Log.println(F("[LoRa] radio re-init ok"));
  return true;
}

#if defined(ROLE_CLIENT)
static void handleClientTxResult(const async_lora_ack::Result &result) {
  using async_lora_ack::Event;
  if (result.event == Event::None || result.event == Event::Started) return;
  clientRxReady = result.rxStatus == RADIOLIB_ERR_NONE;
  if (!clientRxReady) nextClientRxRetryMs = millis() + 100;
  if (result.event == Event::Sent) {
    radioFailStreak = 0;
    if (clientSendingData) { ++clientTxCount; haveDataSent = true; }
  } else {
    ++clientTxErrors;
    if (clientSendingData) { expectedAck.clear(); haveDataSent = false; }
    Log.print(F("[CLIENT] TX error=")); Log.println(result.txStatus);
    if (++radioFailStreak >= RADIO_TX_FAIL_LIMIT) clientRxReady = recoverRadio();
  }
}

static void serviceClientRadio() {
  handleClientTxResult(clientTransmitter.service(millis()));
  if (clientTransmitter.active()) return;
  if (!clientRxReady && loop_metrics::due(millis(), nextClientRxRetryMs)) {
    clientRxFlag = false;
    clientRxReady = radio.startReceive() == RADIOLIB_ERR_NONE;
    nextClientRxRetryMs = millis() + 100;
    if (!clientRxReady && ++radioFailStreak >= RADIO_TX_FAIL_LIMIT) clientRxReady = recoverRadio();
  }
  if (!clientRxReady || !clientRxFlag) return;
  clientRxFlag = false;
  uint8_t buf[255];
  const size_t actualLength = radio.getPacketLength();
  const int state = radio.readData(buf, actualLength <= sizeof(buf) ? actualLength : sizeof(buf));
  AckPayload ack{};
  if (state == RADIOLIB_ERR_NONE && actualLength == ACK_PACKET_LEN &&
      parseAckPacket(buf, actualLength, ack)) {
    lastAckRxMs = millis(); ++ackRxCount;
    lastAckRssiDbm10 = ack.rssiDbm10; lastAckSnrQuarterDb = ack.snrQuarterDb;
  } else ++ackRejectedCount;
  clientRxReady = radio.startReceive() == RADIOLIB_ERR_NONE;
  if (!clientRxReady) nextClientRxRetryMs = millis() + 100;
}

static void serviceClientTransmit() {
  serviceClientRadio();
  const uint32_t now = millis();
  if (lora_schedule::claim(now, SEND_INTERVAL_MS, nextSendMs, dataSkippedSlots)) {
    if (clientTransmitter.active() || !clientRxReady) { ++dataSkippedSlots; return; }
    serviceGps();  // never package bytes still waiting in the UART queue
    sendingDataSeq = txSeq;
    lastDataAckCycle = sendingDataSeq % ACK_EVERY_N == 0;
    if (!lora_schedule::dataFits(millis(), nextSendMs, dataAirtimeMs, ackAirtimeMs,
                                TELEMETRY_SLOT_GUARD_MS, lastDataAckCycle)) {
      ++dataSkippedSlots; return;
    }
    const size_t length = buildDataPacket(clientTxBuffer);
    if (!length) { ++clientTxErrors; return; }
    lastSendMs = millis(); haveDataSent = false;
    clientSendingData = true; clientRxReady = false;
    if (lastDataAckCycle) expectedAck.expect(sendingDataSeq, lastSendMs,
        dataAirtimeMs + ackAirtimeMs + TELEMETRY_SLOT_GUARD_MS);
    else expectedAck.clear();
    handleClientTxResult(clientTransmitter.start(clientTxBuffer, length, lastSendMs,
                                                dataAirtimeMs + TELEMETRY_SLOT_GUARD_MS));
    return;
  }
  if (!clientTransmitter.active() && clientRxReady) {
    const bool telemetryDue = loop_metrics::due(now, nextTelemetryMs);
    const bool diagnosticDue = loop_metrics::due(now, nextDiagnosticMs);
    const bool sendDiagnostic = diagnosticDue && !telemetryDue;
    const uint32_t extraMs = sendDiagnostic ? diagnosticAirtimeMs : telemetryAirtimeMs;
    if ((telemetryDue || diagnosticDue) && lora_schedule::telemetryFits(now, lastSendMs,
        nextSendMs, dataAirtimeMs, extraMs, TELEMETRY_SLOT_GUARD_MS, haveDataSent, lastDataAckCycle)) {
      const size_t length = sendDiagnostic ? buildDiagnosticPacket(clientTxBuffer) : buildTelemetryPacket(clientTxBuffer);
      if (!length) return;
      if (sendDiagnostic) nextDiagnosticMs = now + TELEMETRY_INTERVAL_MS;
      else nextTelemetryMs = now + TELEMETRY_INTERVAL_MS;
      clientSendingData = false; clientRxReady = false;
      handleClientTxResult(clientTransmitter.start(clientTxBuffer, length, now,
                                                  extraMs + TELEMETRY_SLOT_GUARD_MS));
    }
  }
}
#endif

#if defined(ROLE_SERVER)
static void recordPacketEvent(packet_diagnostics::Kind kind, uint32_t ms,
                              const uint8_t *raw = nullptr, size_t length = 0,
                              const DecodedData *data = nullptr, int16_t code = 0) {
  packet_diagnostics::Event event;
  event.kind = kind; event.ms = ms; event.length = length; event.code = code;
  event.rssiDbm10 = static_cast<int16_t>(lroundf(receivedPacketRssi * 10));
  event.snrQuarterDb = static_cast<int16_t>(lroundf(receivedPacketSnr * 4));
  if (raw) {
    event.rawLength = length < sizeof(event.raw) ? length : sizeof(event.raw);
    memcpy(event.raw, raw, event.rawLength);
    PacketHeader header{};
    if (protocol::decodeHeader(raw, length, header)) {
      event.clientId = header.clientId; event.seq = header.seq;
    }
  }
  if (data) {
    event.clientId = data->srcId; event.seq = data->seq;
    event.sourceAgeMs = data->age10ms == 255 ? UINT16_MAX : uint16_t(data->age10ms) * 10;
    event.flags = (data->fix ? 1 : 0) | (data->velocityValid ? 2 : 0);
  }
  packetEvents.push(event);
}

static void handleAckResult(const async_lora_ack::Result &result) {
  using async_lora_ack::Event;
  if (result.event == Event::None || result.event == Event::Started) return;
  if (result.event == Event::Sent) {
    ++ackTxCount;
    ++rxWinAck;
  } else if (result.event == Event::Failed || result.event == Event::Timeout) {
    ++ackErrorCount;
    lastAckError = result.txStatus;
    recordPacketEvent(packet_diagnostics::Kind::AckError, millis(), nullptr, 0, nullptr, result.txStatus);
  }
  serverRxReady = result.rxStatus == RADIOLIB_ERR_NONE;
  if (!serverRxReady) {
    ++rxErrorCount;
    ++rxWinErr;
    rxWinLastErr = result.rxStatus;
    nextServerRxRetryMs = millis() + 100;
  }
}

static void serviceServerRadio() {
  MeasureDuration timing(loraDuration);
  handleAckResult(ackTransmitter.service(millis()));
  if (!ackTransmitter.active() && !serverRxReady &&
      loop_metrics::due(millis(), nextServerRxRetryMs)) {
    serverRadioIrq = false;
    const int16_t status = radio.startReceive();
    serverRxReady = status == RADIOLIB_ERR_NONE;
    nextServerRxRetryMs = millis() + 100;
    if (!serverRxReady && ++radioFailStreak >= RADIO_RX_ERR_LIMIT) {
      serverRxReady = recoverRadio();
    }
  }
}
#endif

#if defined(ROLE_SERVER)
static const char *trackModeStr(TrackMode mode) {
  switch (mode) {
    case TrackMode::Gps: return "gps";
    case TrackMode::Uart: return "uart";
    case TrackMode::Paused: return "paused";
    default: return "manual";
  }
}

static bool gpsTrackingUsable();
static bool gpsModeAvailable();
static void updateTracking();

static bool setServoAngle(float deg) {
  if (!servoPwmReady || !isfinite(deg)) return false;
  deg = constrain(deg, 0.0f, 180.0f);
  const uint32_t duty = servo_profile::dutyForAngle(deg);
  if (duty == servoLastDuty) {
    servoAngleDeg = deg;
    return true;  // PWM hardware keeps sending the existing pulse
  }
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  if (!ledcWrite(SERVO_PIN, duty)) {
    servoPwmReady = false;
    return false;
  }
#else
  ledcWrite(SERVO_LEDC_CH, duty);
#endif
  servoLastDuty = duty;
  servoAngleDeg = deg;
  return true;
}

static void enterManual() {
  uartServoMode.leave();
  sourceSelector.reset();
  controlSource = tracking_policy::Source::Hold;
  servoTargetDeg = servoAngleDeg;
  servoMotion.holdUs(micros());
  commandGate.reset(esp_random());
  trackMode = TrackMode::Manual;
}

static bool setGpsClientBinding(uint16_t id) {
  if (id != 0 && !client_binding::validId(id)) return false;
  if (prefs.putUShort("gpsclient", id) != sizeof(uint16_t)) return false;
  if (id == gpsClientId) return true;
  enterManual();
  handleAckResult(ackTransmitter.cancel());
  gpsClientId = id;
  gpsClient = ClientState{};
  gpsSequence = command_freshness::RadioSequence{};
  haveClientDiagnostic = false; haveSourceEstimate = false;
  lastDataIntervalMs = lastSourceEpochIntervalMs = 0;
  rssiRingIdx = rssiRingCount = 0;
  pktsThisWindow = pktWindowStartMs = 0;
  cachedPktRate = 0.0f;
  rxWinData = rxWinTelem = rxWinDrop = rxWinErr = rxWinAck = rxWinMissing = 0;
  rxWinHaveSeq = false;
  rxWinRssiSum = rxWinSnrSum = 0;
  return true;
}

static void serviceControl() {
  const uint32_t now = millis();
  controlGap.observe(now);
  if (trackMode == TrackMode::Uart) uartServoMode.poll();
  const auto next = sourceSelector.update(
      now, trackMode,
      servoPwmReady && trackMode == TrackMode::Gps && gpsTrackingUsable(),
      servoPwmReady && trackMode == TrackMode::Uart && uartServoMode.ready());
  if (next != controlSource) {
    controlSource = next;
    servoMotion.holdUs(micros());
    Log.print(F("[SERVO] source -> "));
    Log.println(tracking_policy::sourceName(controlSource));
  }
  if (trackMode == TrackMode::Uart && controlSource == tracking_policy::Source::Uart) {
    if (!servoMotion.target(uartServoMode.targetMdeg() / 1000.0)) servoMotion.holdUs(micros());
  }
  const bool permitted = servoPwmReady && (trackMode == TrackMode::Manual ||
      controlSource == tracking_policy::Source::Gps ||
      controlSource == tracking_policy::Source::Uart);
  if (!permitted) servoMotion.holdUs(micros());
  if(gpsCadence.poll(now)) updateTracking();
  const uint32_t nowUs = micros();
  {
    MeasureDuration timing(motionDuration);
    if (permitted && servoMotion.tickUs(nowUs) &&
        !setServoAngle(static_cast<float>(servoMotion.position()))) {
      servoMotion.initializeUs(servoAngleDeg, nowUs);  // retain last successful PWM command
      servoMotion.faultUs(nowUs);
    }
  }
  servoTargetDeg = static_cast<float>(servoMotion.requested());
  if ((!servoPwmReady || servoMotion.faulted()) && trackMode != TrackMode::Paused) {
    enterManual();
    trackMode = TrackMode::Paused;
    Log.println(F("[SERVO] control fault; motion paused"));
  }
}

static bool initServo() {
  // Diagnostics must distinguish reboots even if PWM initialization fails.
  controlBootId = esp_random();
  if (controlBootId == 0) controlBootId = 1;
  commandGate.reset(esp_random());
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
  // First PWM command: physical position is unknown without servo feedback.
  // Runtime slew starts from this commanded home; boot motion cannot be bounded.
  if (!setServoAngle(90.0f)) {
    servoPwmReady = false;
    Log.println(F("[SERVO] ERROR: boot centre PWM write failed"));
    return false;
  }
  servoTargetDeg = servoAngleDeg;
  servoMotion.initializeUs(servoAngleDeg, micros());
  Log.print(F("[SERVO] LEDC ready on IO"));
  Log.print(SERVO_PIN);
  Log.print(F(" at "));
  Log.print(SERVO_PWM_HZ);
  Log.print(F(" Hz/"));
  Log.print(SERVO_PWM_RES_BITS);
  Log.print(F(" bit; centred at 90 deg"));
  Log.println();
  return true;
}

static uint32_t clientSampleAgeMs() {
  if (!havePkt || lastData.age10ms == 255) return UINT32_MAX;
  const uint64_t age = uint64_t(lastData.age10ms) * 10 + dataAirtimeMs + uint32_t(millis() - lastRxMs);
  return age > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(age);
}
static bool clientFixFresh() {
  return havePkt && lastData.fix && clientSampleAgeMs() <
      tracking_policy::kGpsFreshMs - GPS_BACKLOG_GUARD_MS;
}
static bool haveBearingFix() { return clientFixFresh() && gpsFixFresh(); }

static void updateDeclination() {
  // TinyGPS date and location share the station receiver; no network/date entry.
  // Recompute at 1 Hz, but revoke immediately when the live inputs become stale.
  if (!gpsFixFresh() || !gps.date.isValid() || gps.date.age() >= 60000) {
    declinationReady = false;
    return;
  }
  static uint32_t lastUpdateMs = 0;
  const uint32_t now = millis();
  if (declinationReady && now - lastUpdateMs < 1000) return;
  lastUpdateMs = now;
  float year = 0.0f;
  declinationReady = magnetic_declination::decimalYear(
      gps.date.year(), gps.date.month(), gps.date.day(), year) &&
      magnetic_declination::taiwanDegrees(
          stationLatitude(), stationLongitude(), year, declinationDeg);
}

// Both ends must have fresh Good or OK fixes; Bad/Miss cannot select GPS.
static bool gpsModeAvailable() {
  if (!haveBearingFix()) return false;
  gnss_snapshot::Snapshot sample;
  if (!gnssCollector.sample(millis(), sample) || sample.satellites == 255 ||
      lastData.satellites == 255 || lastData.hdop10 == 255) return false;
  return tracking_policy::usableGps(sample.fix, sample.satellites, sample.hdop,
                                    sample.sourceAgeMs + sample.ageUncertaintyMs) &&
         tracking_policy::usableGps(lastData.fix, lastData.satellites,
                                    lastData.hdop10 / 10.0f,
                                    clientSampleAgeMs() + GPS_BACKLOG_GUARD_MS);
}
static bool gpsTrackingUsable() {
  return mountCalibrated && declinationReady && gpsModeAvailable();
}

// Project the last received client position forward along its velocity vector.
//
// Alpha ON projects between packets; OFF uses the received position. The age
// includes the source epoch estimate, RF airtime and time since reception.
// GNSS internal latency is not measured; the additional local uncertainty guard
// affects expiry only, never inflates the projection time.
static void predictClientPos(double &lat, double &lon) {
  lat = lastData.lat;
  lon = lastData.lon;
  if (!gpsPredictionEnabled) return;  // alpha = 0: retain the last GPS position
  if (!lastData.velocityValid || lastData.speedCmS == UINT16_MAX ||
      lastData.courseDeg10 >= 3600 || lastData.speedCmS < DR_MIN_SPEED_CMS) return;  // course is noise when idle
  if (!clientFixFresh()) return;
  float ageS = clientSampleAgeMs() / 1000.0f;
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

// Recompute the GPS target; the elapsed-time Servo output is independent.
// Driven from loop() every TRACK_UPDATE_MS, not by packet arrival.
static void updateTracking() {
  if (trackMode != TrackMode::Gps || controlSource != tracking_policy::Source::Gps ||
      !gpsTrackingUsable()) {
    return;
  }

  double clientLat, clientLon;
  predictClientPos(clientLat, clientLon);
  float bearing = (float)computeBearing(stationLatitude(), stationLongitude(),
                                        clientLat, clientLon);
  float target = normalize360(magnetic_declination::trueBearing(
      mountOffsetDeg, declinationDeg) - bearing);
  if (target > 270.0f) target -= 360.0f;  // wrap small negatives toward 0
  servoTargetDeg = constrain(target,0.0f,180.0f);
  if (!servoMotion.target(servoTargetDeg)) servoMotion.holdUs(micros());

  // Only the common motion backend can write PWM.
  static uint32_t lastTrackLogMs = 0;
  if (millis() - lastTrackLogMs >= 3000) {
    lastTrackLogMs = millis();
    Log.print(F("[TRACK] bearing="));
    Log.print(bearing, 1);
    Log.print(F(" off="));
    Log.print(mountOffsetDeg, 1);
    Log.print(F(" servo="));
    Log.print(servoAngleDeg, 1);
    Log.print(F("->"));
    Log.println(servoTargetDeg, 1);
  }
}

// Lock the servo-to-world offset from one statement: "the camera is looking at
// camBearing while the servo reads servoDeg". Only explicit compass calibration writes this reference.
static float lockMountOffset(float compassBearing, float servoDeg) {
  mountOffsetDeg = normalize360(compassBearing + servoDeg);
  if (mountOffsetDeg > 180.0f) mountOffsetDeg -= 360.0f;
  mountCalibrated = true;
  return mountOffsetDeg;
}

// Only the explicitly selected source is enabled. Switching closes old UART
// input before opening a new UART session; calibration remains in RAM.
static bool selectTrackingMode(TrackMode mode) {
  if (!servoPwmReady || (mode != TrackMode::Gps && mode != TrackMode::Uart)) return false;
  if (mode == TrackMode::Gps && !gpsModeAvailable()) return false;
  if (trackMode == mode) return true;
  enterManual();
  trackMode = mode;
  lastTrackingMode = mode;
  if (mode == TrackMode::Uart) uartServoMode.enter(commandGate.epoch());

  Log.print(F("[TRACK] mode -> "));
  Log.println(trackModeStr(mode));
  return true;
}

// One line a minute instead of one per packet: everything you would otherwise
// have to read 60 separate lines to get. Sequence numbers carry the loss rate for
// free, so this also answers "is the link dropping packets" without arithmetic.
static void logRxSummary() {
  if (rxWinData == 0 && rxWinTelem == 0 && rxWinDrop == 0 && rxWinErr == 0) {
    return;  // nothing arrived; the idle log already covers a dead link
  }

  // Count gaps only between continuously accepted DATA; exclude reboot
  // resynchronization and the independent telemetry/diagnostic sequences.
  uint32_t expected = rxWinData + rxWinMissing;
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
  if (haveBearingFix()) {
    Log.print(F(" | dist="));
    Log.print((uint32_t)equirectDistanceM(stationLatitude(), stationLongitude(),
                                          lastData.lat, lastData.lon));
    Log.print(F("m brg="));
    Log.print((float)computeBearing(stationLatitude(), stationLongitude(),
                                    lastData.lat, lastData.lon), 0);
  }
  Log.print(F(" servo="));
  Log.print(servoAngleDeg, 1);
  Log.print(' ');
  Log.println(trackModeStr(trackMode));

  rxWinData = rxWinTelem = rxWinDrop = rxWinErr = rxWinAck = rxWinMissing = 0;
  rxWinHaveSeq = false;
  rxWinRssiSum = rxWinSnrSum = 0;
}

static void renderServerDisplay() {
  MeasureDuration timing(oledDuration);
  // Centre label column ("V" / T/H / GPS / BAT) is framed by two vertical lines;
  // Server values sit left of it, client values right of it. The 15 px labels get
  // a 1 px gap to each line; the odd rounding pixel is biased to the right.
  const int leftLineX = 55;
  const int rightLineX = 73;
  const int leftCx = 27;   // centre of the Server (left) region
  const int rightCx = 100; // centre of the Client (right) region
  const int midCx = 64;    // centre of the label column

  SigLevel sGps = serverGpsState();

  const uint16_t selectedId = gpsClientId;
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
    cGps = gpsSignal(clientFixFresh(), csats, chdop);
  }

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
  if (gpsClientId != 0) snprintf(buf, sizeof(buf), "Client %04X", gpsClientId);
  else snprintf(buf, sizeof(buf), "Unbound");
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
  } else if (wifiReconnectingUntilMs != 0 &&
             !loop_metrics::due(millis(), wifiReconnectingUntilMs)) {
    snprintf(wifiBuf, sizeof(wifiBuf), "WiFi connecting: %s ...", WIFI_SSID);
  } else {
    uint32_t remain =
        !loop_metrics::due(millis(), nextWifiRetryMs) ? (nextWifiRetryMs - millis()) / 1000 + 1 : 0;
    snprintf(wifiBuf, sizeof(wifiBuf), "reconnecting to %s in %lus", WIFI_SSID,
             (unsigned long)remain);
  }
  display.drawStr(64 - display.getStrWidth(wifiBuf) / 2, 63, wifiBuf);

  oledNextRow=0;
}
static void serviceServerDisplay() {
  if(oledNextRow>=8)return;
  MeasureDuration timing(oledDuration);
  display.updateDisplayArea(0,oledNextRow++,16,1);
  if(oledNextRow==8)++oledFrames;
}

#endif

#if defined(ROLE_SERVER)
// Shared Servo status for track and status endpoints.
static void appendCommandContext(String &js) {
  js += F("\"control_boot_id\":"); js += String(controlBootId);
  js += F(",\"control_epoch\":"); js += String(commandGate.epoch());
  js += F(",\"command_seq\":"); js += String(commandGate.sequence());
  js += F(",\"clock_ms\":"); js += String(millis());
}

static String controlReply() {
  String js = F("{\"ok\":true,\"mode\":\""); js += trackModeStr(trackMode);
  js += F("\",\"angle\":"); js += String(servoAngleDeg, 3);
  js += F(",\"target\":"); js += String(servoMotion.requested(), 3); js += ',';
  appendCommandContext(js); js += '}'; return js;
}

static String motionSettingsJson() {
  String js=F("{\"ok\":true,\"speed\":");js+=String(servoMotion.speed(),3);
  js+=F(",\"min_speed\":1,\"max_speed\":90,\"default_speed\":30,");
  appendCommandContext(js);js+='}';return js;
}

static String gpsPredictionJson() {
  String js = F("{\"ok\":true,\"enabled\":");
  js += gpsPredictionEnabled ? F("true") : F("false");
  js += F(",\"alpha\":"); js += gpsPredictionEnabled ? '1' : '0';
  js += F(",\"default_enabled\":true,");
  appendCommandContext(js); js += '}'; return js;
}

static bool saveGpsPrediction(bool enabled) {
  const uint32_t saved = enabled ? 1 : 0;
  if (prefs.getUInt(GPS_PREDICTION_KEY, UINT32_MAX) != saved &&
      (prefs.putUInt(GPS_PREDICTION_KEY, saved) != sizeof(saved) ||
       prefs.getUInt(GPS_PREDICTION_KEY, UINT32_MAX) != saved)) return false;
  // The next GPS target tick uses this setting. Mode, calibration and the
  // common Servo rate limiter remain intact; no direct PWM write here.
  gpsPredictionEnabled = enabled;
  return true;
}

// Persist a single value before applying it. No separate Apply/Save stages.
static bool saveServoSpeed(double speed) {
  if(!servo_motion::validSpeed(speed))return false;
  const uint32_t saved=servo_motion::encodeSpeed(speed);
  if(prefs.getUInt(servo_motion::kSpeedKey,0)!=saved &&
      (prefs.putUInt(servo_motion::kSpeedKey,saved)!=sizeof(saved) ||
       prefs.getUInt(servo_motion::kSpeedKey,0)!=saved))return false;
  return servoMotion.setSpeed(saved/1000.0);
}

static bool requestTrackingMode(TrackMode mode) {
  if(!servoPwmReady) {
    httpServer.send(503,"application/json","{\"ok\":false,\"error\":\"servo PWM unavailable\"}");
    return false;
  }
  if(mode==TrackMode::Gps && !gpsModeAvailable()) {
    httpServer.send(409,"application/json","{\"ok\":false,\"error\":\"GPS requires fresh Good or OK signals from Server and Client\"}");
    return false;
  }
  return selectTrackingMode(mode);
}

static void appendServoJson(String &js) {
  js += F("\"servo\":{\"angle\":");
  js += String(servoAngleDeg, 1);
  js += F(",\"target\":");
  js += String(servoTargetDeg, 1);
  js += F(",\"moving\":"); js += servoMotion.moving() ? F("true") : F("false");
  js += F(",\"speed_limit_deg_s\":"); js += String(servoMotion.speed(),3);
  js += F(",\"prediction_enabled\":"); js += gpsPredictionEnabled ? F("true") : F("false");
  js += F(",\"prediction_alpha\":"); js += gpsPredictionEnabled ? '1' : '0';
  js += F(",\"velocity_deg_s\":"); js += String(servoMotion.velocity(), 3);
  js += F(",\"motion_fault\":"); js += servoMotion.faulted() ? F("true") : F("false");
  js += F(",\"rejected_commands\":"); js += String(rejectedMotionCommands);
  js += F(",\"rejected_gps_sequence\":"); js += String(rejectedGpsSequence); js += ',';
  appendCommandContext(js);
  js += F(",\"mode\":\"");
  js += trackModeStr(trackMode);
  js += F("\",\"source\":\"");
  js += trackMode == TrackMode::Manual ? "manual" : tracking_policy::sourceName(controlSource);
  js += F("\",\"gps_available\":");
  js += gpsModeAvailable() ? F("true") : F("false");
  js += F(",\"gps_usable\":");
  js += gpsTrackingUsable() ? F("true") : F("false");
  js += F(",\"gps_ready\":");
  js += sourceSelector.gpsReady() ? F("true") : F("false");
  js += F(",\"calibrated\":");
  js += mountCalibrated ? F("true") : F("false");
  js += F(",\"pwm_ok\":");
  js += servoPwmReady ? F("true") : F("false");
  js += F(",\"uart_state\":\"");
  js += uartServoMode.stateName();
  js += F("\",\"uart_ready\":");
  js += uartServoMode.ready() ? F("true") : F("false");
  js += F(",\"mount_offset_deg\":");
  js += String(mountOffsetDeg, 1);
  js += F(",\"north_reference\":\"magnetic\",\"declination_deg\":");
  js += declinationReady ? String(declinationDeg, 2) : F("null");
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
  if (haveBearingFix()) {
    js += String(computeBearing(stationLatitude(), stationLongitude(),
                                lastData.lat, lastData.lon), 1);
  } else {
    js += F("-1");
  }
  js += F(",\"client\":{\"lat\":");
  js += havePkt ? String(lastData.lat, 6) : F("0");
  js += F(",\"lon\":");
  js += havePkt ? String(lastData.lon, 6) : F("0");
  js += F(",\"fix\":");
  js += clientFixFresh() ? F("1") : F("0");
  js += F(",\"velocity_valid\":");
  js += clientFixFresh() && lastData.velocityValid ? F("true") : F("false");
  js += F(",\"speed_cms\":");
  js += havePkt && lastData.speedCmS != UINT16_MAX ? String(lastData.speedCmS) : F("null");
  js += F(",\"course_deg10\":");
  js += havePkt && lastData.courseDeg10 < 3600 ? String(lastData.courseDeg10) : F("null");
  js += F(",\"satellite_class\":"); js += havePkt ? String(lastData.satelliteClass) : F("0");
  js += F(",\"satellites\":");
  js += haveTelemetry && lastTelemetry.satellites != 255 && millis() - lastTelemetryRxMs < 90000 ?
      String(lastTelemetry.satellites) : F("null");
  js += F(",\"satellites_age_ms\":");
  js += haveTelemetry ? String(uint32_t(millis() - lastTelemetryRxMs)) : F("null");
  js += F(",\"hdop\":");
  js += havePkt && lastData.hdop10 != 255 ? String(lastData.hdop10 / 10.0f, 1) : F("null");
  js += F(",\"rx_age_ms\":"); js += havePkt ? String(uint32_t(millis() - lastRxMs)) : F("null");
  js += F(",\"source_age_ms\":"); js += havePkt && lastData.age10ms != 255 ? String(uint32_t(lastData.age10ms) * 10) : F("null");
  js += F(",\"sample_age_ms\":"); js += clientSampleAgeMs() != UINT32_MAX ? String(clientSampleAgeMs()) : F("null");
  js += F(",\"age_basis\":\"nmea_epoch_aligned_arrival\"");
  js += F(",\"last_rx_sec\":");
  js += (havePkt && sinceRx != UINT32_MAX) ? String(sinceRx) : F("-1");
  js += F("},\"server\":{\"lat\":");
  js += gpsFixFresh() ? String(stationLatitude(), 6) : F("0");
  js += F(",\"lon\":");
  js += gpsFixFresh() ? String(stationLongitude(), 6) : F("0");
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
  appendServoJson(js);
  js += '}';
  return js;
}

// Preserve the existing HTTP shape, with an explicit one-client capacity.
static String buildWhitelistJson() {
  String js = F("{\"whitelist\":[");
  if (gpsClientId != 0) {
    char hex[5];
    snprintf(hex, sizeof(hex), "%04X", gpsClientId);
    js += '"'; js += hex; js += '"';
  }
  js += F("],\"count\":");
  js += gpsClientId != 0 ? '1' : '0';
  js += F(",\"capacity\":1}");
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
  if (trackMode == TrackMode::Gps && !havePkt) {
    add(alerts::WARN, "client_never", "還沒收到追蹤器的訊號",
        "請確認追蹤器已經開機（長按電源鍵），而且攝影站已綁定它的 ID。");
  } else if (trackMode == TrackMode::Gps) {
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
  if (trackMode == TrackMode::Gps && havePkt && sinceRx >= 0 && (uint32_t)sinceRx < alerts::kLinkWarnSec) {
    int csats = (lastData.satellites != 0xFF) ? (int)lastData.satellites : 0;
    float chdop = (lastData.hdop10 != 0xFF) ? lastData.hdop10 / 10.0f : 99.9f;
    SigLevel g = gpsSignal(clientFixFresh(), csats, chdop);
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

  if (trackMode == TrackMode::Gps &&
      (serverGpsState() == SIG_BAD || serverGpsState() == SIG_MISS)) {
    add(alerts::WARN, "srv_gps", "攝影站自己的定位不穩",
        "算出來的方位會有偏差，鏡頭容易追偏。請把攝影站移到天空開闊、沒有建築物"
        "或大樹遮住的地方。");
  }

  // --- 設定與硬體 ---------------------------------------------------------
  if (!servoPwmReady) {
    add(alerts::ERROR, "servo_fault", "雲台沒有反應",
        "鏡頭無法轉動。請檢查雲台的訊號線（IO21）和接地線有沒有鬆脫，然後重新開機。");
  }
  if (trackMode == TrackMode::Uart && !uartServoMode.ready()) {
    add(alerts::WARN, "uart_wait", "等待 UART 指令",
        "尚未收到有效指令或指令已逾時，鏡頭保持原角度。請檢查控制端程式與 UART 接線。");
  }
  if (servoMotion.faulted() && servoPwmReady) {
    add(alerts::ERROR, "motion_fault", "運動控制已保持",
        "控制器偵測到無效狀態，已停止更新角度。請重新開機；若持續發生，請保留執行紀錄。");
  }
  if (trackMode == TrackMode::Gps && !mountCalibrated) {
    add(alerts::WARN, "mount_uncal", "還沒設定鏡頭的方向",
        "請先切到手動，再到「資訊」分頁輸入鏡頭指南針角度。");
  }
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

static void appendHttpDetailJson(String &js) {
  // Only completed requests are exposed: a response cannot contain its own
  // eventual write duration. pre_handler also includes accept/dispatch work.
  const auto &stats = httpServer.timing();
  auto duration = [&](const char *name, uint32_t us) {
    js += F(",\""); js += name; js += F("\":"); js += String(us / 1000.0, 3);
  };
  auto snapshot = [&](const http_timing::Snapshot &value) {
    js += F("{\"route\":\""); js += http_timing::routeName(value.route); js += '"';
    duration("total_ms", value.totalUs);
    duration("pre_handler_ms", value.preHandlerUs);
    duration("build_ms", value.buildUs);
    duration("write_ms", value.writeUs);
    duration("other_ms", value.otherUs);
    js += F(",\"bytes_written\":"); js += String(value.bytesWritten);
    js += F(",\"short_writes\":"); js += String(value.shortWrites); js += '}';
  };
  js += F(",\"http_detail\":{\"requests\":"); js += String(stats.requests);
  js += F(",\"polls_without_request\":"); js += String(stats.pollsWithoutRequest);
  js += F(",\"slow_requests\":"); js += String(stats.slowRequests);
  duration("max_poll_without_request_ms", stats.maxPollWithoutRequestUs);
  js += F(",\"max\":{\"total_ms\":"); js += String(stats.maxTotalUs / 1000.0, 3);
  duration("pre_handler_ms", stats.maxPreHandlerUs);
  duration("build_ms", stats.maxBuildUs);
  duration("write_ms", stats.maxWriteUs);
  duration("other_ms", stats.maxOtherUs); js += '}';
  js += F(",\"last\":");
  if (stats.requests) snapshot(stats.last); else js += F("null");
  js += F(",\"slowest\":");
  if (stats.requests) snapshot(stats.slowest); else js += F("null");
  js += '}';
}

static void appendTimingJson(String &js) {
  js += F("\"timing\":{\"control_gap_max_ms\":");
  js += String(controlGap.maxMs);
  js += F(",\"control_gap_over_250ms\":");
  js += String(controlGap.over250ms);
  auto duration = [&](const char *name, const loop_metrics::Duration &value) {
    js += F(",\""); js += name; js += F("\":{\"last_ms\":");
    js += String(value.lastUs / 1000.0f, 2);
    js += F(",\"max_ms\":"); js += String(value.maxUs / 1000.0f, 2);
    js += F(",\"over_50ms\":"); js += String(value.over50ms); js += '}';
  };
  duration("loop", loopDuration);
  duration("http", httpDuration);
  duration("bme280", envDuration);
  duration("pmu", pmuDuration);
  duration("oled", oledDuration);
  duration("lora", loraDuration);
  duration("ota", otaDuration);
  duration("motion", motionDuration);
  js += F(",\"uart_late_polls\":"); js += String(uartServoMode.latePolls());
  js += F(",\"uart_discarded_bytes\":"); js += String(uartServoMode.discardedBytes());
  js += F(",\"uart_rejected_commands\":"); js += String(uartServoMode.rejectedCommands());
  appendHttpDetailJson(js);
  js += '}';
}

static String buildStatusJson() {
  String js;
  js.reserve(3072);  // 含 alerts 與 HTTP 分段計時，減少回覆組裝時 realloc

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
  js += String(havePkt && millis() - lastRxMs < 5000 ? cachedPktRate : 0.0f, 2);
  uint32_t rxTotal = rxDataCount + rxTelemetryCount + rxDiagnosticCount + rxDropCount;
  js += F(",\"rx_data\":");
  js += String(rxDataCount);
  js += F(",\"rx_telemetry\":");
  js += String(rxTelemetryCount);
  js += F(",\"rx_diagnostic\":"); js += String(rxDiagnosticCount);
  js += F(",\"rx_drop\":");
  js += String(rxDropCount);
  js += F(",\"drop_rate\":");
  js += rxTotal ? String((float)rxDropCount / rxTotal, 3) : F("0");
  js += F(",\"ack_tx\":");
  js += String(ackTxCount);
  js += F(",\"ack_busy\":");
  js += ackTransmitter.active() ? F("true") : F("false");
  js += F(",\"ack_errors\":");
  js += String(ackErrorCount);
  js += F(",\"ack_skipped\":");
  js += String(ackSkippedCount);
  js += F(",\"ack_last_error\":");
  js += String(lastAckError);
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

  js += F("\"health\":{\"firmware_version\":\"" SHORE_SPOTTER_VERSION "\",\"uptime_s\":");
  js += String((millis() - bootMs) / 1000);
  js += F(",\"protocol_version\":4");
  js += F(",\"heap_free\":");
  js += String(ESP.getFreeHeap());
  js += F(",\"heap_min\":");
  js += String(ESP.getMinFreeHeap());
  js += F(",\"reset_reason\":\"");
  js += String((int)esp_reset_reason());
  js += F("\",\"rx_error\":");
  js += String(rxErrorCount);
  js += F("},");

  appendTimingJson(js);
  js += F(",");

  // Servo / tracking state
  appendServoJson(js);
  js += F(",");
  appendAlertsJson(js);
  js += '}';
  return js;
}



// A fixed-size page of new events. The browser owns history across requests.
static String buildDebugJson(const packet_diagnostics::Selection &selection) {
  String js; js.reserve(4500);
  const uint32_t now = millis();
  const auto &g = gnssCollector.stats();
  gnss_snapshot::Snapshot sample;
  const bool sampled = gnssCollector.sample(now, sample);
  js = F("{\"schema_version\":2,\"firmware_version\":\"" SHORE_SPOTTER_VERSION
         "\",\"build\":\"" __DATE__ " " __TIME__ "\",\"protocol_version\":4,\"boot_id\":");
  js += String(controlBootId); js += F(",\"clock_ms\":"); js += String(now);
  js += F(",\"config\":{\"rf_frequency_mhz\":"); js += String(RF_FREQUENCY, 3);
  js += F(",\"bw_khz\":"); js += String(RF_BW, 1);
  js += F(",\"sf\":"); js += String(RF_SF); js += F(",\"cr\":"); js += String(RF_CR);
  js += F(",\"data_bytes\":17,\"ack_bytes\":11,\"telemetry_bytes\":11,\"diagnostic_bytes\":17");
  js += F(",\"send_interval_ms\":"); js += String(SEND_INTERVAL_MS);
  js += F(",\"ack_every_n\":"); js += String(ACK_EVERY_N);
  js += F(",\"gnss_baud\":"); js += String(GPS_BAUD);
  js += F(",\"bound_client_id\":"); js += String(gpsClientId);
  js += F(",\"gnss_age_uncertainty_ms\":"); js += String(GPS_BACKLOG_GUARD_MS);
  js += F(",\"data_airtime_ms\":"); js += String(dataAirtimeMs);
  js += F(",\"ack_airtime_ms\":"); js += String(ackAirtimeMs);
  js += F(",\"telemetry_airtime_ms\":"); js += String(telemetryAirtimeMs);
  js += F(",\"diagnostic_airtime_ms\":"); js += String(diagnosticAirtimeMs);
  js += F("},\"gps\":{\"age_basis\":\"nmea_epoch_aligned_arrival\",\"measurement_clock_synchronized\":false");
  js += F(",\"scope\":\"server_local\",\"last_epoch_interval_ms\":"); js += String(g.lastEpochIntervalMs);
  js += F(",\"source_age_ms\":"); js += sampled ? String(sample.sourceAgeMs) : F("null");
  js += F(",\"epoch_ms_of_day\":"); js += sampled ? String(sample.epochMsOfDay) : F("null");
  js += F(",\"fix\":"); js += gpsFixFresh() ? F("true") : F("false");
  js += F(",\"have_rmc\":"); js += sampled && sample.haveRmc ? F("true") : F("false");
  js += F(",\"have_gga\":"); js += sampled && sample.haveGga ? F("true") : F("false");
  js += F(",\"epochs\":"); js += String(g.snapshots);
  js += F(",\"rmc\":"); js += String(g.rmcSentences); js += F(",\"gga\":"); js += String(g.ggaSentences);
  js += F(",\"checksum_errors\":"); js += String(g.checksumErrors);
  js += F(",\"rejected_sentences\":"); js += String(g.rejectedSentences);
  js += F(",\"ignored_sentences\":"); js += String(g.ignoredSentences);
  js += F(",\"backwards_epochs\":"); js += String(g.backwardEpochs);
  js += F(",\"duplicate_epochs\":"); js += String(g.duplicateEpochs);
  js += F(",\"backlog_drops\":"); js += String(gpsBacklogDrops);
  js += F("},\"client_diagnostic\":{\"received\":"); js += haveClientDiagnostic ? F("true") : F("false");
  js += F(",\"rx_age_ms\":"); js += haveClientDiagnostic ? String(uint32_t(now - lastClientDiagnosticMs)) : F("null");
  js += F(",\"fresh\":"); js += haveClientDiagnostic && now - lastClientDiagnosticMs < 90000 ? F("true") : F("false");
  js += F(",\"epoch_interval_ms\":"); js += haveClientDiagnostic ? String(lastClientDiagnostic.epochIntervalMs) : F("null");
  js += F(",\"backlog_drops\":"); js += haveClientDiagnostic ? String(lastClientDiagnostic.backlogDrops) : F("null");
  js += F(",\"nmea_errors\":"); js += haveClientDiagnostic ? String(lastClientDiagnostic.nmeaErrors) : F("null");
  js += F(",\"tx_errors\":"); js += haveClientDiagnostic ? String(lastClientDiagnostic.txErrors) : F("null");
  js += F(",\"skipped_slots\":"); js += haveClientDiagnostic ? String(lastClientDiagnostic.skippedSlots) : F("null");
  js += F(",\"status_bits\":"); js += haveClientDiagnostic ? String(lastClientDiagnostic.status) : F("null");
  js += F(",\"counter_encoding\":\"uint16_saturating_since_client_boot\"}");
  js += F(",\"counters\":{\"rx_data\":"); js += String(rxDataCount);
  js += F(",\"rx_telemetry\":"); js += String(rxTelemetryCount);
  js += F(",\"rx_diagnostic\":"); js += String(rxDiagnosticCount);
  js += F(",\"radio_errors\":"); js += String(rxErrorCount);
  js += F(",\"rejected_length\":"); js += String(rejectedLength);
  js += F(",\"rejected_format\":"); js += String(rejectedFormat);
  js += F(",\"rejected_binding\":"); js += String(rejectedBinding);
  js += F(",\"rejected_sequence\":"); js += String(rejectedGpsSequence);
  js += F(",\"sequence_gaps\":"); js += String(sequenceMissing);
  js += F(",\"sequence_resyncs\":"); js += String(sequenceResyncs);
  js += F(",\"invalid_fix_packets\":"); js += String(invalidFixPackets);
  js += F(",\"invalid_velocity_packets\":"); js += String(invalidVelocityPackets);
  js += F(",\"ack_sent\":"); js += String(ackTxCount);
  js += F(",\"ack_errors\":"); js += String(ackErrorCount);
  js += F(",\"ack_skipped\":"); js += String(ackSkippedCount);
  js += F(",\"radio_recoveries\":"); js += String(radioRecoverCount);
  js += F(",\"last_data_interval_ms\":"); js += String(lastDataIntervalMs);
  js += F(",\"max_data_interval_ms\":"); js += String(maxDataIntervalMs);
  js += F(",\"inferred_source_updates\":"); js += String(sourceEpochUpdates);
  js += F(",\"inferred_source_interval_ms\":"); js += String(lastSourceEpochIntervalMs);
  js += F("},\"events\":{\"capacity\":64,\"total\":"); js += String(packetEvents.total());
  js += F(",\"overwritten\":"); js += String(packetEvents.overwritten());
  js += F(",\"next_id\":"); js += String(selection.nextId);
  js += F(",\"more\":"); js += selection.more ? F("true") : F("false");
  js += F(",\"dropped\":"); js += selection.dropped ? F("true") : F("false");
  js += F(",\"reset\":"); js += selection.reset ? F("true") : F("false");
  js += F(",\"items\":[");
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < selection.count; ++i) {
    const auto &event = packetEvents.at(selection.start + i);
    if (i) js += ',';
    js += F("{\"id\":"); js += String(event.id); js += F(",\"ms\":"); js += String(event.ms);
    js += F(",\"kind\":\""); js += packet_diagnostics::name(event.kind); js += '"';
    js += F(",\"client_id\":"); js += String(event.clientId);
    js += F(",\"seq\":"); js += String(event.seq);
    js += F(",\"length\":"); js += String(event.length);
    js += F(",\"source_age_ms\":"); js += event.sourceAgeMs != UINT16_MAX ? String(event.sourceAgeMs) : F("null");
    js += F(",\"rssi_dbm\":"); js += event.rawLength ? String(event.rssiDbm10 / 10.0f, 1) : F("null");
    js += F(",\"snr_db\":"); js += event.rawLength ? String(event.snrQuarterDb / 4.0f, 2) : F("null");
    js += F(",\"code\":"); js += String(event.code); js += F(",\"flags\":"); js += String(event.flags);
    js += F(",\"raw_hex\":\"");
    for (uint8_t j = 0; j < event.rawLength; ++j) { js += hex[event.raw[j] >> 4]; js += hex[event.raw[j] & 15]; }
    js += F("\"}");
  }
  js += F("]},\"limitations\":[\"GNSS internal latency is unmeasured\",\"GNSS rate is independent of RF rate\",\"Client diagnostics are low-rate snapshots\",\"Servo angle is commanded, not physical feedback\"]}");
  return js;
}

// Byte cursor is independent of the packet event cursor and may wrap.
static String buildLogText(uint32_t from, bool &dropped, uint32_t &next) {
  const uint32_t available = logWrapped ? LOG_BUF_BYTES : logTotal;
  const uint32_t delta = logTotal - from;  // unsigned byte cursor may wrap
  const uint32_t count = delta <= available ? delta : available;
  dropped = delta > available && available > 0;
  next = logTotal;

  String out;
  out.reserve(count + 8);
  const size_t start = (logHead + LOG_BUF_BYTES - count) % LOG_BUF_BYTES;
  for (uint32_t k = 0; k < count; k++) {
    const size_t idx = (start + k) % LOG_BUF_BYTES;
    out += logBuf[idx];
  }
  return out;
}

static bool parseAngleArgument(const char *name, float minimum, float maximum,
                               float &value) {
  if (!httpServer.hasArg(name)) return false;
  const String text = httpServer.arg(name);
  if (text.length() == 0 || text.length() > 16) return false;
  bool digit = false, dot = false;
  for (size_t i = 0; i < text.length(); ++i) {
    const char c = text[i];
    if (c >= '0' && c <= '9') digit = true;
    else if (c == '.' && !dot) dot = true;
    else return false;
  }
  if (!digit) return false;
  value = text.toFloat();
  return isfinite(value) && value >= minimum && value <= maximum;
}

static bool acceptMotionRequest() {
  uint32_t values[3]; const char *names[] = {"epoch", "seq", "stamp"};
  for (size_t i = 0; i < 3; ++i) {
    const String raw = httpServer.arg(names[i]);
    if (!command_freshness::parseUint32(raw.c_str(), raw.length(), values[i])) {
      ++rejectedMotionCommands;
      httpServer.send(409, "application/json", "{\"ok\":false,\"error\":\"refresh page: motion epoch, seq and stamp required\"}");
      return false;
    }
  }
  if (!commandGate.accept(values[0], values[1], values[2], millis())) {
    ++rejectedMotionCommands;
    httpServer.send(409, "application/json", "{\"ok\":false,\"error\":\"stale or out-of-order command; refresh status\"}");
    return false;
  }
  return true;
}

static void initWebServer() {
  // send() 的 const char* 多載會先 `String passStr = content` 把整份 44 KB 複製到
  // heap（arduino-esp32 WebServer.cpp 裡自己的 log_e 就寫著 "Use send_P for long
  // arrays"）。send_P 分塊送出，不做這份複製。
  httpServer.on("/", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.send_P(200, PSTR("text/html"), WEB_UI_HTML);
  });
  // Standalone log viewer. The 資訊 page opens this in a separate browser tab so
  // watching the log no longer costs you the radar view.

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
    httpServer.send(200, "application/json", httpServer.measureBuild([]() { return buildTrackJson(); }));
  });
  httpServer.on("/api/whitelist", HTTP_GET, []() {
    httpServer.send(200, "application/json", buildWhitelistJson());
  });
  // Legacy endpoint: set replaces the one binding; add refuses a second client.
  httpServer.on("/api/whitelist", HTTP_POST, []() {
    const String action = httpServer.arg("action");
    uint16_t nextId = gpsClientId;
    if (action == "clear") {
      nextId = 0;
    } else if (action == "set" || action == "add" || action == "remove") {
      const String text = httpServer.arg("id");
      uint16_t id = 0;
      if (!client_binding::parseId(text.c_str(), text.length(), id)) {
        httpServer.send(400, "application/json",
                        "{\"ok\":false,\"error\":\"id must be 1-4 hex digits and not reserved\"}");
        return;
      }
      if (action == "add" && gpsClientId != 0 && id != gpsClientId) {
        httpServer.send(409, "application/json",
                        "{\"ok\":false,\"error\":\"one GPS client supported; use action=set to replace\"}");
        return;
      }
      if (action == "remove") {
        if (id == gpsClientId) nextId = 0;
      } else nextId = id;
    } else {
      httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown action\"}");
      return;
    }
    if (!setGpsClientBinding(nextId)) {
      httpServer.send(503, "application/json",
                      "{\"ok\":false,\"error\":\"client binding could not be saved\"}");
      return;
    }
    httpServer.send(200, "application/json", buildWhitelistJson());
  });
  // The camera-mounted compass is the reference; neither GPS fix nor a
  // landmark is needed. Hold Manual while reading the compass and submitting.
  httpServer.on("/api/track/calibrate", HTTP_POST, []() {
    if (!acceptMotionRequest()) return;
    float bearing = 0.0f;
    if (!parseAngleArgument("bearing", 0.0f, 360.0f, bearing) || bearing >= 360.0f) {
      httpServer.send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bearing must be 0 <= degrees < 360\"}");
      return;
    }
  if (!servoPwmReady) {
      httpServer.send(503, "application/json",
                      "{\"ok\":false,\"error\":\"servo PWM unavailable\"}");
      return;
    }
    if (trackMode == TrackMode::Gps || trackMode == TrackMode::Uart) {
      httpServer.send(409, "application/json",
                      "{\"ok\":false,\"error\":\"select Manual before compass calibration\"}");
      return;
    }
    if (servoMotion.moving()) {
      httpServer.send(409, "application/json",
                      "{\"ok\":false,\"error\":\"wait for servo motion to finish before compass calibration\"}");
      return;
    }
    lockMountOffset(bearing, servoAngleDeg);
    String js = F("{\"ok\":true,\"method\":\"compass\",\"bearing\":");
    js += String(bearing, 2);
    js += F(",\"mount_offset_deg\":");
    js += String(mountOffsetDeg, 2);
    js += F(",\"servo_angle\":");
    js += String(servoAngleDeg, 1);
    js += '}';
    Log.println(F("[TRACK] camera compass calibration set in RAM"));
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
    String txt = httpServer.measureBuild([&]() { return buildLogText(from, dropped, next); });
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.sendHeader("X-Log-Boot", String(controlBootId));
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
    httpServer.send(200, "application/json", httpServer.measureBuild([]() { return buildStatusJson(); }));
  });
  httpServer.on("/api/debug", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "no-store");
    const bool haveBoot = httpServer.hasArg("boot_id");
    const bool haveSince = httpServer.hasArg("since");
    uint32_t cursorBoot = 0, since = 0, limit = packet_diagnostics::kMaxEventsPerResponse;
    auto argument = [](const char *name, uint32_t &value) {
      const String text = httpServer.arg(name);
      return command_freshness::parseUint32(text.c_str(), text.length(), value);
    };
    if (haveBoot != haveSince ||
        (haveBoot && (!argument("boot_id", cursorBoot) || !argument("since", since))) ||
        (httpServer.hasArg("limit") && !argument("limit", limit)) ||
        limit == 0 || limit > packet_diagnostics::kMaxEventsPerResponse) {
      httpServer.send(400, "application/json", "{\"error\":\"paired uint32 boot_id/since and limit 1..8 required\"}");
      return;
    }
    const auto selection = packetEvents.select(haveBoot, cursorBoot == controlBootId, since, limit);
    httpServer.send(200, "application/json", httpServer.measureBuild([&]() { return buildDebugJson(selection); }));
  });
  // Manual stops tracking; GPS and UART are exclusive modes.
  httpServer.on("/api/servo/mode", HTTP_POST, []() {
    if (!acceptMotionRequest()) return;
    String mode = httpServer.arg("mode");
    if (mode == "jetson") mode = "uart";  // compatibility with existing callers
    if (mode == "manual") {
      enterManual();
    } else if (mode == "gps" || mode == "uart") {
      if (!requestTrackingMode(mode == "gps" ? TrackMode::Gps : TrackMode::Uart)) return;
    } else {
      httpServer.send(400, "application/json",
                      "{\"ok\":false,\"error\":\"mode must be manual, gps or uart\"}");
      return;
    }
    httpServer.send(200, "application/json", controlReply());
  });
  httpServer.on("/api/servo", HTTP_POST, []() {
    if (!acceptMotionRequest()) return;
    float angle = 0.0f;
    if (!parseAngleArgument("angle", 0.0f, 180.0f, angle)) {
      httpServer.send(400, "application/json",
                      "{\"ok\":false,\"error\":\"angle must be 0..180\"}");
      return;
    }
    if (!servoPwmReady) {
      httpServer.send(503, "application/json",
                      "{\"ok\":false,\"error\":\"servo PWM unavailable\"}");
      return;
    }
    if (trackMode != TrackMode::Manual) {
      httpServer.send(409, "application/json", "{\"ok\":false,\"error\":\"select Manual before setting angle\"}");
      return;
    }
    if (!servoMotion.target(angle)) {
      httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"angle outside configured limits or motion fault\"}");
      return;
    }
    servoTargetDeg = static_cast<float>(servoMotion.requested());
    httpServer.send(200, "application/json", controlReply());
  });
  httpServer.on("/api/servo/settings",HTTP_GET,[](){
    httpServer.send(200,"application/json",motionSettingsJson());
  });
  httpServer.on("/api/servo/settings",HTTP_POST,[](){
    if(!acceptMotionRequest())return;
    for(int i=0;i<httpServer.args();++i) {
      const String name=httpServer.argName(i);
      if(name!="speed" && name!="epoch" && name!="seq" && name!="stamp") {
        httpServer.send(400,"application/json","{\"ok\":false,\"error\":\"only speed can be configured\"}");return;
      }
    }
    float speed=0;
    if(!parseAngleArgument("speed",servo_motion::kMinimumSpeed,servo_motion::kMaximumSpeed,speed)) {
      httpServer.send(400,"application/json","{\"ok\":false,\"error\":\"speed must be 1..90 degrees per second\"}");return;
    }
    if(!saveServoSpeed(speed)) {
      httpServer.send(503,"application/json","{\"ok\":false,\"error\":\"speed could not be saved; current limit retained\"}");return;
    }
    httpServer.send(200,"application/json",motionSettingsJson());
  });
  httpServer.on("/api/track/prediction", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.send(200, "application/json", gpsPredictionJson());
  });
  httpServer.on("/api/track/prediction", HTTP_POST, []() {
    if (!acceptMotionRequest()) return;
    bool validArgs = httpServer.args() == 4;
    for (int i = 0; i < httpServer.args(); ++i) {
      const String name = httpServer.argName(i);
      if (name != "enabled" && name != "epoch" && name != "seq" && name != "stamp") validArgs = false;
    }
    const String enabled = httpServer.arg("enabled");
    if (!validArgs || (enabled != "0" && enabled != "1")) {
      httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"enabled must be 0 or 1; no other settings accepted\"}");
      return;
    }
    if (!saveGpsPrediction(enabled == "1")) {
      httpServer.send(503, "application/json", "{\"ok\":false,\"error\":\"GPS prediction could not be saved; current setting retained\"}");
      return;
    }
    httpServer.send(200, "application/json", gpsPredictionJson());
  });
  httpServer.on("/api/track/start", HTTP_POST, []() {
    if (!acceptMotionRequest()) return;
    if (!requestTrackingMode(TrackMode::Gps)) return;
    httpServer.send(200, "application/json", controlReply());
  });
  httpServer.on("/api/track/resume", HTTP_POST, []() {
    if (!acceptMotionRequest()) return;
    if (!requestTrackingMode(lastTrackingMode)) return;
    httpServer.send(200, "application/json", controlReply());
  });
  httpServer.on("/api/track/pause", HTTP_POST, []() {
    if (!acceptMotionRequest()) return;
    enterManual();
    trackMode = TrackMode::Paused;
    httpServer.send(200, "application/json", controlReply());
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
    if (httpServer.uri().startsWith("/api/")) {
      httpServer.send(404, "application/json", "{\"ok\":false,\"error\":\"unknown API\"}");
      return;
    }
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
        enterManual();
        trackMode = TrackMode::Paused;
        Log.println(F("[OTA] update started; Servo control paused"));
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
#if defined(ROLE_SERVER)
  enterManual();
#endif
#if defined(ROLE_CLIENT)
  if (pmuOnline) {
    pmu.setALDO1Voltage(3300);
    pmu.enableALDO1();
    delay(100);
  }
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);
  detectOledAddress();
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
#if defined(ROLE_SERVER)
  enterManual();
#endif
#if defined(ROLE_CLIENT)
  if (pmuOnline) {
    pmu.setALDO1Voltage(3300);
    pmu.enableALDO1();
    delay(100);
  }
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);
  detectOledAddress();
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
  display.drawStr(0, 12, "SHORE SPOTTER v" SHORE_SPOTTER_VERSION);
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
  Wire.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);
  detectOledAddress();
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
  nextClientOledRefreshMs = millis();  // force an immediate redraw
}
#endif

#if defined(ROLE_SERVER)
// Returns true if ACK start (including its recovery path) already owns RX.
static bool acceptRadioPacket(const uint8_t *buf, size_t n, uint32_t receivedAtMs) {
  using packet_diagnostics::Kind;
  PacketHeader hdr{};
  auto reject = [&](Kind kind) {
    ++rxDropCount; ++rxWinDrop;
    recordPacketEvent(kind, receivedAtMs, buf, n);
    return false;
  };
  if (n != DATA_PACKET_LEN && n != ACK_PACKET_LEN &&
      n != TELEMETRY_PACKET_LEN && n != DIAGNOSTIC_PACKET_LEN) {
    ++rejectedLength; return reject(Kind::Length);
  }
  if (!protocol::decodeHeader(buf, n, hdr)) {
    ++rejectedFormat; return reject(Kind::Format);
  }
  if (!isClientAllowed(hdr.clientId)) { ++rejectedBinding; return reject(Kind::Binding); }
  if (hdr.msgType == MSG_TELEMETRY) {
    DecodedTelemetry tel{};
    if (!parseTelemetryPacket(buf, n, tel)) { ++rejectedFormat; return reject(Kind::Format); }
    lastTelemetry = tel; haveTelemetry = true; lastTelemetryRxMs = receivedAtMs;
    ++rxTelemetryCount; ++rxWinTelem;
    if (clientHumBaselinePct < 0 && tel.humidityPct != 255) clientHumBaselinePct = tel.humidityPct;
    recordPacketEvent(Kind::Telemetry, receivedAtMs, buf, n);
    return false;
  }
  if (hdr.msgType == MSG_DIAGNOSTIC) {
    DiagnosticPayload diag{};
    if (!protocol::decodeDiagnostic(buf, n, hdr, diag)) { ++rejectedFormat; return reject(Kind::Format); }
    lastClientDiagnostic = diag; haveClientDiagnostic = true;
    lastClientDiagnosticMs = receivedAtMs; ++rxDiagnosticCount;
    recordPacketEvent(Kind::Diagnostic, receivedAtMs, buf, n);
    return false;
  }
  DecodedData data{};
  if (!parseDataPacket(buf, n, data)) { ++rejectedFormat; return reject(Kind::Format); }
  if (!gpsSequence.accept(data.seq, receivedAtMs)) {
    ++rejectedGpsSequence;
    recordPacketEvent(Kind::Sequence, receivedAtMs, buf, n, &data);
    return false;
  }
  if (havePkt) {
    lastDataIntervalMs = receivedAtMs - lastRxMs;
    if (lastDataIntervalMs > maxDataIntervalMs) maxDataIntervalMs = lastDataIntervalMs;
    const uint16_t delta = data.seq - lastData.seq;
    if (lastDataIntervalMs < 2500 && delta > 0 && delta < 0x8000) {
      sequenceMissing += delta - 1; rxWinMissing += delta - 1;
    } else ++sequenceResyncs;
  }
  if (data.age10ms != 255) {
    const uint32_t estimate = receivedAtMs - dataAirtimeMs - uint32_t(data.age10ms) * 10;
    const int32_t delta = static_cast<int32_t>(estimate - lastEstimatedSourceMs);
    if (!haveSourceEstimate || delta > 100) {
      if (haveSourceEstimate) lastSourceEpochIntervalMs = delta;
      ++sourceEpochUpdates; lastEstimatedSourceMs = estimate; haveSourceEstimate = true;
    }
  }
  lastData = data; lastRxMs = receivedAtMs; havePkt = true; ++rxDataCount;
  lastRssi = receivedPacketRssi; lastSnr = receivedPacketSnr;
  if (!data.fix) ++invalidFixPackets;
  if (!data.velocityValid) ++invalidVelocityPackets;
  recordPacketEvent(Kind::Data, receivedAtMs, buf, n, &data);
  rssiRing[rssiRingIdx] = lastRssi; snrRing[rssiRingIdx] = lastSnr;
  rssiRingIdx = (rssiRingIdx + 1) % RSSI_WINDOW;
  if (rssiRingCount < RSSI_WINDOW) ++rssiRingCount;
  ++pktsThisWindow;
  const uint32_t elapsed = receivedAtMs - pktWindowStartMs;
  if (elapsed >= 5000) {
    cachedPktRate = pktsThisWindow * 1000.0f / elapsed;
    pktsThisWindow = 0; pktWindowStartMs = receivedAtMs;
  }
  ++rxWinData;
  if (!rxWinHaveSeq) {
    rxWinFirstSeq = data.seq; rxWinHaveSeq = true;
    rxWinRssiMin = rxWinRssiMax = lastRssi; rxWinSnrMin = rxWinSnrMax = lastSnr;
  } else {
    if (lastRssi < rxWinRssiMin) rxWinRssiMin = lastRssi;
    if (lastRssi > rxWinRssiMax) rxWinRssiMax = lastRssi;
    if (lastSnr < rxWinSnrMin) rxWinSnrMin = lastSnr;
    if (lastSnr > rxWinSnrMax) rxWinSnrMax = lastSnr;
  }
  rxWinLastSeq = data.seq; rxWinRssiSum += lastRssi; rxWinSnrSum += lastSnr;
  if (data.seq % ACK_EVERY_N != 0) return false;
  if (millis() - receivedAtMs > ACK_START_MAX_AGE_MS) {
    ++ackSkippedCount; recordPacketEvent(Kind::AckSkipped, receivedAtMs, nullptr, 0, &data);
    return false;
  }
  const size_t length = buildAckPacket(ackPacketBuffer, data.srcId, data.seq, lastRssi, lastSnr);
  serverRxReady = false;
  handleAckResult(ackTransmitter.start(ackPacketBuffer, length, millis(),
                                      ackAirtimeMs + TELEMETRY_SLOT_GUARD_MS));
  return true;
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
  Log.println(F("[BOOT] Shore Spotter v" SHORE_SPOTTER_VERSION));

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
  Wire.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);
  envSensorOnline = initEnvSensor();
  sampleEnvSensor();

  showClientBootScreen();  // show MAC / batt / temp for 10 s then turn off OLED

  Log.println(F("[CLIENT] mode active: send position packets"));
  Log.print(F("[CLIENT] node id (chip MAC last 2 bytes) = 0x"));
  Log.println(nodeId, HEX);
  Log.println(F("[CLIENT] Bind this id on SERVER using /api/whitelist action=set."));
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
  clientRxReady = radio.startReceive() == RADIOLIB_ERR_NONE;
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
  Wire.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);
  envSensorOnline = initEnvSensor();
  sampleEnvSensor();
  nextEnvMs = millis() + ENV_UPDATE_MS;
  initServo();                     // restore boot centre at 90 degrees
  detectOledAddress();
  display.setI2CAddress(oledI2CAddr << 1);
  display.begin();
  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);
  display.drawStr(0, 12, "SHORE SPOTTER v" SHORE_SPOTTER_VERSION);
  display.drawStr(0, 30, "SERVER booting...");
  display.sendBuffer();

  cachedBatteryMv = readBatteryMilliVolts();
  if (batteryCriticallyLow()) showLowBatteryAndPowerOff();  // refuse to boot empty

  nextServerIdleLogMs = millis() + SERVER_IDLE_LOG_MS;
  nextRxSummaryMs = millis() + RX_SUMMARY_MS;
  nextDisplayMs = millis() + DISPLAY_REFRESH_MS;
  Log.println(F("[SERVER] mode active: receive position packets"));
  loadServerSettings();
  Log.print(F("[SERVER] GPS client: "));
  if (gpsClientId != 0) Log.println(gpsClientId, HEX);
  else Log.println(F("unbound"));

  // Connect to the phone-provided hotspot in station mode.
  // Credentials come from include/wifi_config.h (WIFI_SSID / WIFI_PASSWORD).
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("shore-spotter-server");
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Log.print(F("[WiFi] Connecting to configured hotspot "));
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiStart < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Log.print('.');
  }
  Log.println();

  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);
  display.drawStr(0, 12, "SHORE SPOTTER v" SHORE_SPOTTER_VERSION);
  display.drawHLine(0, 14, 128);
  if (WiFi.status() == WL_CONNECTED) {
    cachedApIpAddr = WiFi.localIP();
    cachedApIp = cachedApIpAddr.toString();
    Log.println(F("[WiFi] Connected. Open the address shown on OLED."));
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
  serverRadioIrq = false;
  serverRxReady = radio.startReceive() == RADIOLIB_ERR_NONE;
  if (!serverRxReady) nextServerRxRetryMs = millis() + 100;
  selectTrackingMode(TrackMode::Uart);  // default source; wait for fresh UART input
#endif
}

void loop() {
#if defined(ROLE_CLIENT)
  serviceGps();

  serviceClientTransmit();

  if (loop_metrics::due(millis(), nextBatteryMs)) {
    nextBatteryMs = millis() + BATTERY_UPDATE_MS;
    cachedBatteryMv = readBatteryMilliVolts();
    Log.print(F("[CLIENT] Battery update mV="));
    Log.println(cachedBatteryMv);
    checkLowBatteryAndMaybeShutdown();
  }

  if (loop_metrics::due(millis(), nextEnvMs)) {
    nextEnvMs = millis() + ENV_UPDATE_MS;
    sampleEnvSensor();
  }

  serviceGps();
  serviceClientTransmit();
  if (clientTxCount > 5 && millis() - lastAckRxMs > ACK_STALE_MS &&
      millis() - lastAckMissMarkMs > 5000) {
    ++ackMissCount; lastAckMissMarkMs = millis();
  }
  if (!clientTransmitter.active() && !expectedAck.pending(millis())) evaluateAtpc();
  static uint32_t nextClientSummaryMs = 0;
  if (loop_metrics::due(millis(), nextClientSummaryMs)) {
    nextClientSummaryMs = millis() + 10000;
    Log.print(F("[CLIENT] tx=")); Log.print(clientTxCount);
    Log.print(F(" errors=")); Log.print(clientTxErrors);
    Log.print(F(" skipped=")); Log.print(dataSkippedSlots);
    Log.print(F(" ACK=")); Log.print(ackRxCount);
    Log.print(F(" rejected=")); Log.print(ackRejectedCount);
    Log.print(F(" GNSS_epoch_ms=")); Log.print(gnssCollector.stats().lastEpochIntervalMs);
    Log.print(F(" backlog_drops=")); Log.println(gpsBacklogDrops);
  }

  // PWR key: short-press wakes the screen 10 s, long-press shuts down.
  // Polled on a timer (see PMU_KEY_POLL_MS) — loop() no longer blocks, so every
  // pass would otherwise cost two I2C transactions on the PMU bus.
  if (pmuOnline && loop_metrics::due(millis(), nextPmuKeyMs)) {
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
    if (loop_metrics::due(millis(), clientOledOffMs)) {
      sleepClientOled();
      clientOledAwake = false;
    } else if (loop_metrics::due(millis(), nextClientOledRefreshMs)) {
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
  MeasureDuration loopTiming(loopDuration);
  serviceServerRadio();
  updateDeclination();
  serviceControl();
  if (otaReady && WiFi.status() == WL_CONNECTED) {
    MeasureDuration timing(otaDuration);
    ArduinoOTA.handle();
  }
  serviceControl();
  {
    MeasureDuration timing(httpDuration);
    httpServer.handleClient();
  }
  serviceServerRadio();
  serviceControl();
  serviceGps();  // keep the server's own GPS position fresh
  updateDeclination();

  if (loop_metrics::due(millis(), nextEnvMs)) {
    nextEnvMs = millis() + ENV_UPDATE_MS;
    sampleEnvSensor();
  }
  serviceControl();

  if (loop_metrics::due(millis(), nextBatteryMs)) {
    nextBatteryMs = millis() + BATTERY_UPDATE_MS;
    cachedBatteryMv = readBatteryMilliVolts();
    checkLowBatteryAndMaybeShutdown();
  }
  serviceControl();

  if (wifiReconnectingUntilMs != 0 &&
      loop_metrics::due(millis(), wifiReconnectingUntilMs)) wifiReconnectingUntilMs = 0;

  // WiFi watchdog: re-attempt the hotspot every WIFI_RETRY_INTERVAL_MS while
  // offline, and refresh the cached IP once (re)connected.
  if (WiFi.status() == WL_CONNECTED) {
    nextWifiRetryMs = millis();  // keep retry deadline fresh during a long connection
    // Re-read on change, not just when empty: a DHCP renewal can hand out a
    // different address without the link ever reporting disconnected, and the
    // old code would then show a stale IP on the OLED forever. Compared as an
    // IPAddress so the common case allocates no String — loop() runs at ~1 kHz.
    IPAddress ip = WiFi.localIP();
    if (ip != cachedApIpAddr) {
      cachedApIpAddr = ip;
      cachedApIp = ip.toString();
      Log.print(F("[WiFi] address is now "));
      Log.println(F("(address shown on OLED)"));
    }
    initArduinoOta();
  } else {
    cachedApIp = "";
    cachedApIpAddr = IPAddress();
    if (loop_metrics::due(millis(), nextWifiRetryMs)) {
      nextWifiRetryMs = millis() + WIFI_RETRY_INTERVAL_MS;
      wifiReconnectingUntilMs = millis() + 2000;  // show "reconnecting" briefly
      WiFi.reconnect();
    }
  }

  // Long-press PWR → show shutdown screen then power off
  if (pmuOnline && loop_metrics::due(millis(), nextPmuKeyMs)) {
    MeasureDuration timing(pmuDuration);
    nextPmuKeyMs = millis() + PMU_KEY_POLL_MS;
    pmu.getIrqStatus();
    if (pmu.isPekeyLongPressIrq()) {
      pmu.clearIrqStatus();
      showShutdownAndPowerOff();
    }
    pmu.clearIrqStatus();  // don't let unrelated latched IRQs accumulate
  }

  serviceControl();
  // DIO1 drives RX / TX completion; HTTP and I2C calls are measured separately.
  serviceServerRadio();
  if (!ackTransmitter.active() && serverRxReady && serverRadioIrq) {
    MeasureDuration timing(loraDuration);
    const uint32_t receivedAtMs = serverRadioIrqMs;
    serverRadioIrq = false;
    bool ackHandledRx = false;
    uint8_t buf[255];
    // Preserve the real RF length. Never truncate a long frame into a valid one.
    const size_t n = radio.getPacketLength();
    const int state = radio.readData(buf, n <= sizeof(buf) ? n : sizeof(buf));
    if (state == RADIOLIB_ERR_NONE && n <= sizeof(buf)) {
      radioFailStreak = 0;
      receivedPacketRssi = radio.getRSSI(); receivedPacketSnr = radio.getSNR();
      ackHandledRx = acceptRadioPacket(buf, n, receivedAtMs);
    } else {
      ++rxErrorCount; ++rxWinErr; rxWinLastErr = state;
      recordPacketEvent(packet_diagnostics::Kind::RadioError, receivedAtMs, nullptr, n, nullptr, state);
      if (++radioFailStreak >= RADIO_RX_ERR_LIMIT && recoverRadio()) {
        ackHandledRx = true; serverRxReady = true;
      }
    }
    if (!ackHandledRx) {
      // Do not clear the software IRQ after restarting RX: preserve a new packet.
      serverRxReady = radio.startReceive() == RADIOLIB_ERR_NONE;
      if (!serverRxReady) nextServerRxRetryMs = millis() + 100;
    }
  }

  serviceServerRadio();

  serviceControl();

  if (loop_metrics::due(millis(), nextRxSummaryMs)) {
    nextRxSummaryMs = millis() + RX_SUMMARY_MS;
    logRxSummary();
  }

  // Advance even while the link is healthy, so this deadline never lies dormant
  // for longer than half the millis range before the next disconnection.
  if (loop_metrics::due(millis(), nextServerIdleLogMs)) {
    nextServerIdleLogMs = millis() + SERVER_IDLE_LOG_MS;
    if (!havePkt || millis() - lastRxMs > 2500) {
      Log.print(F("[SERVER] idle | mode="));
      Log.print(trackModeStr(trackMode));
      Log.print(F(" SRV-GPS fix="));
      Log.print(gpsFixFresh() ? 1 : 0);
      Log.print(F(" sats="));
      Log.print(gps.satellites.isValid() ? (int)gps.satellites.value() : -1);
      Log.print(F(" hdop="));
      if (gps.hdop.isValid()) Log.print(gps.hdop.hdop(), 1);
      else Log.print(F("--"));
      Log.print(F(" servo="));
      Log.print(servoAngleDeg, 1);
      Log.println(F(" (waiting for client packets)"));
    }
  }

  if (oledNextRow>=8 && loop_metrics::due(millis(), nextDisplayMs)) {
    nextDisplayMs = millis() + DISPLAY_REFRESH_MS;
    renderServerDisplay();
  }
  serviceControl();
  serviceServerDisplay();
  serviceControl();
#endif
}
