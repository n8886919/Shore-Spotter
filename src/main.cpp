#include <Arduino.h>
#include <algorithm>
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
#include "client_boot_animation.h"

#include "protocol.h"  // shared LoRa wire protocol (client + station)
#include "firmware_version.h"
#include "geo_math.h"  // pure maths (angles / bearing / circle fit / grading)
#include "tracking_policy.h"
#include "loop_metrics.h"
#include "power_irq.h"
#include "gnss_snapshot.h"
#include "gnss_diagnostics.h"
#include "gnss_rate.h"
#include "lora_schedule.h"
#include "client_cadence.h"
#include "async_lora_tx.h"
#include "sd_log.h"
#if defined(FIELD_DIAGNOSTIC)
#include "diagnostic_store.h"
#include "field_diagnostic.h"
#endif
#if defined(CLIENT_TRIP_LOG)
#if !defined(FIELD_DIAGNOSTIC) || !defined(ROLE_CLIENT)
#error CLIENT_TRIP_LOG requires FIELD_DIAGNOSTIC and ROLE_CLIENT
#endif
#include "trip_log.h"
#endif
#if defined(ROLE_CLIENT)
#include <atomic>
#include "client_sd_policy.h"
#endif

#if defined(ROLE_STATION)
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include "axiom_log.h"
#include "alerts.h"    // 現場提醒的門檻與分級
#include "uart_servo_mode.h"  // UART input, shared with GPS tracking
#include "servo_motion.h"
#include "control_cadence.h"
#include "station_position.h"
#include "command_freshness.h"
#include "client_binding.h"
#include "packet_diagnostics.h"
#include "packet_rate.h"
#include "http_timing.h"
#include "magnetic_declination.h"
#include "web_icon.h"  // 分頁圖示（由 tools/make_icon.py 產生）
#include "web_ui.h"
#include "wifi_config.h"  // phone hotspot SSID / password (edit there)
#endif

// IMPORTANT:
// This project keeps one main.cpp and splits behavior by build flags:
// ROLE_CLIENT (water side) / ROLE_STATION (shore side).
// Upload env:tbeam-client or env:tbeam-station to each board.
//
// Fail fast at compile time: exactly one role must be selected.
#if !defined(ROLE_CLIENT) && !defined(ROLE_STATION)
#error "No role selected: define ROLE_CLIENT or ROLE_STATION (use env:tbeam-client / env:tbeam-station)."
#endif
#if defined(ROLE_CLIENT) && defined(ROLE_STATION)
#error "Both roles defined: pick only ROLE_CLIENT or ROLE_STATION, not both."
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
constexpr int RF_SF = 10;
// SF10 trades airtime for sensitivity. DATA fits a 500 ms slot; low-rate
// diagnostics use dedicated slots interleaved with DATA. Both roles must match.
// Coding rate 4/5; v5 uplink codecs are shared by both roles.
constexpr int RF_CR = 5;
constexpr int RF_SYNC_WORD = 0x12;
// Fixed per-role power is applied by initRadio() on boot and radio recovery.
// Legacy Client NVS txpwr/atpc values are intentionally no longer read.
#if defined(ROLE_CLIENT)
constexpr int TX_POWER_DBM = 20;
#else
constexpr int TX_POWER_DBM = 17;
#endif

constexpr uint32_t SEND_INTERVAL_MS = gnss_rate::kTargetIntervalMs;  // requested GNSS period, not a DATA timer
constexpr uint32_t TELEMETRY_INTERVAL_MS = 60000;  // battery + env packet rate
// Leave bounded settling/service headroom before the next RF slot.
constexpr uint32_t TELEMETRY_SLOT_GUARD_MS = 80;
constexpr uint32_t BATTERY_UPDATE_MS = 5000;

constexpr uint32_t ENV_UPDATE_MS = 5000;
constexpr uint32_t STATION_IDLE_LOG_MS = 5000;
// One log line per received packet floods the 4 KB ring in ~16 s, so whatever you
// opened the log to look at has already scrolled out — the RX detail crowds out
// [SERVO], [MAGCAL] and [TRACK] entirely. Accumulate instead and print one
// summary a minute, which keeps roughly half an hour of history in the ring.
constexpr uint32_t RX_SUMMARY_MS = 60000;
constexpr uint32_t LINK_TIMEOUT_MS = 5000;
constexpr uint32_t LINK_WARN_MS = 15000;
constexpr uint32_t DISPLAY_REFRESH_MS = 500;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000;  // station: re-attempt hotspot every 5 s when offline
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

#if defined(ROLE_STATION)
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
constexpr const char *GPS_PREDICTION_KEY = "gpspred5";

#endif

// GPS UART defaults (common on T-Beam family, override if your board differs).
// Freshness comes from the coherent NMEA collector, not TinyGPS's latched
// isValid(). Repeated epochs do not renew it; the caller also budgets the
// UART age uncertainty. RF retransmission never makes an old fix new.
constexpr uint32_t GPS_FIX_MAX_AGE_MS = tracking_policy::kGpsFreshMs;

constexpr int GPS_RX_PIN = 9;
constexpr int GPS_TX_PIN = 8;
constexpr int GPS_EN_PIN = 7;
constexpr uint32_t GPS_BAUD = gnss_rate::kBaud;

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
#if defined(ROLE_STATION)
static bool oledOnline = false;
#endif

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
#if defined(ROLE_STATION)
IPAddress cachedApIpAddr;  // last address seen, to detect DHCP changes
#endif

// Shared OLED object — client enables it only during boot-info and shutdown screens.
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);

#if defined(ROLE_STATION)
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
// station it is additionally captured into logBuf. Deriving from Print inherits
// every print()/println() overload, so call sites change in name only.
// Safe without locking: nothing logs from an ISR (the DIO1 handler only sets a
// flag) and the web station is serviced from loop(), so this is single-threaded.
// Buffered to whole lines before touching the USB serial port. This matters far
// more than it looks: Print::print(F("...")) emits one character at a time, and
// HWCDC::write() waits up to tx_timeout_ms (100 ms by default) per call when the
// port is enumerated but nothing is draining it — exactly the state of a board
// plugged into a PC with no terminal open. That turned a ~110-character log line
// into ~11 s of blocking. Buffering makes it one bulk write per line instead of
// one per character; setTxTimeoutMs(0) in setup() then removes the wait entirely.
// Dropped serial output is acceptable because the station's ring buffer (and the
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
    if (!sd_log::usbTransferActive()
#if defined(FIELD_DIAGNOSTIC)
        && !diagnostic_store::transferActive()
#endif
    ) Serial.write(lineBuf_, lineLen_);
#if defined(ROLE_STATION)
    for (size_t i = 0; i < lineLen_; i++) logPush(lineBuf_[i]);
#endif
    sd_log::text(lineBuf_, lineLen_, millis());
    lineLen_ = 0;
  }
  uint8_t lineBuf_[256];  // longest log line here is ~300 B, so at most 2 writes
  size_t  lineLen_ = 0;
};
static LogTee Log;

#if defined(ROLE_STATION)
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
uint32_t nextGnssDiagnosticMs = 0;
uint16_t gnssDiagnosticSeq = 0;
uint32_t lastSendMs = 0;  // start of the last position TX (telemetry slot anchor)
uint32_t nextTelemetryMs = 0;
uint32_t nextPmuKeyMs = 0;
uint32_t nextBatteryMs = 0;
// Telemetry quiet slot, filled in by computeAirtimeBudget() at boot.
uint32_t telemetrySlotMinMs = 0;
uint32_t telemetrySlotMaxMs = 0;
uint32_t dataAirtimeMs = 0, telemetryAirtimeMs = 0;
uint32_t diagnosticAirtimeMs = 0, gnssDiagnosticAirtimeMs = 0;
uint32_t dataSkippedSlots = 0;
static gnss_snapshot::Collector gnssCollector{GPS_BAUD};
static gnss_rate::Monitor gpsRate;
static uint32_t gpsLastServiceMs = 0, gpsBacklogDrops = 0;
static bool gpsServiceStarted = false;
uint16_t cachedBatteryMv = 0;
bool pmuOnline = false;
uint16_t nodeId = 0;  // set in setup() from chip MAC last 2 bytes

static uint32_t bootMs = 0;
static uint32_t powerBootId = 0;
static_assert(power_irq::kStatus1 == XPOWERS_AXP2101_INTSTS1, "PMU IRQ address mismatch");
static_assert(power_irq::kShort == (XPOWERS_AXP2101_PKEY_SHORT_IRQ >> 8), "PMU short key mismatch");
static_assert(power_irq::kLong == (XPOWERS_AXP2101_PKEY_LONG_IRQ >> 8), "PMU long key mismatch");
static_assert(power_irq::kNegative == (XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ >> 8), "PMU press edge mismatch");
static_assert(power_irq::kPositive == (XPOWERS_AXP2101_PKEY_POSITIVE_IRQ >> 8), "PMU release edge mismatch");
static power_irq::State powerIrqState;
static power_irq::Sample powerIrqSample;
#if defined(ROLE_CLIENT)
static power_irq::ClientBootGuard clientPowerKeys;
static void serviceClientPowerKey();
#endif
static uint32_t powerIrqMs = 0;
static int powerVbusRaw[2] = {-1, -1};
static uint32_t powerVbusReadErrors = 0;
#if defined(ROLE_CLIENT) && defined(FIELD_DIAGNOSTIC)
// Results of the original init calls, not extra attempts to change PMU state.
static int pmuBatteryInit[4] = {-1, -1, -1, -1};
#endif

#if defined(ROLE_CLIENT)
static uint32_t clientTxCount = 0, clientTxErrors = 0, diagnosticTxCount = 0;
static client_cadence::Scheduler clientCadence;
static bool clientDataDeferred = false;
static uint32_t nextClientRadioRetryMs = 0;
static uint32_t nextClientTxMs = 0, nextClientExtraMs = 0;
static bool clientRadioReady = false, haveDataSent = false;
static bool clientSendingData = false;
static uint8_t clientTxBuffer[MAX_PACKET_LEN];
static uint8_t clientLogTxLength = 0;
static uint32_t clientLogTxStarted = 0;
static client_sd::Gate clientLogGate;
static loop_metrics::Gap clientLoopGap;
// XPowers reads span multiple Wire calls, so serialize complete PMU operations,
// including reads/IRQs. Main-loop callers try once and defer if the worker owns it.
static std::atomic_flag clientRailLock = ATOMIC_FLAG_INIT;
struct ClientRailGuard {
  bool held;
  explicit ClientRailGuard(bool wait = false) : held(false) {
    do {
      held = !clientRailLock.test_and_set(std::memory_order_acquire);
      if (held || !wait) break;
      vTaskDelay(pdMS_TO_TICKS(1));
    } while (true);
  }
  ~ClientRailGuard() { if (held) clientRailLock.clear(std::memory_order_release); }
};
static bool clientSdPower(bool on) {
  ClientRailGuard guard(true); // SD worker only, never wait in the main loop
  if (!pmuOnline) return false;
  return on ? pmu.setBLDO1Voltage(3300) && pmu.enableBLDO1() && pmu.isEnableBLDO1() && pmu.getBLDO1Voltage() == 3300 :
              pmu.disableBLDO1() && !pmu.isEnableBLDO1();
}

// Client OLED wake state (short-press PWR turns the screen on for a few seconds)
static bool clientOledAwake = false;
static uint32_t clientOledOffMs = 0;
static uint32_t nextClientOledRefreshMs = 0;

// Client uses DIO1 only for non-blocking TX completion; it never enters RX.
volatile bool clientTxFlag = false;
void IRAM_ATTR onClientDio1() { clientTxFlag = true; }
static async_lora_tx::Transmitter<SX1262> clientTransmitter(
    radio, clientTxFlag, RADIOLIB_SX126X_IRQ_TX_DONE,
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
  return id == 0 || id == ID_BROADCAST || id == STATION_ID ? id ^ 0x0100 : id;
}

#if defined(ROLE_STATION)
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
uint32_t nextStationIdleLogMs = 0;

static DecodedTelemetry &lastTelemetry = gpsClient.telemetry;
static bool &haveTelemetry = gpsClient.haveTelemetry;
static uint32_t &lastTelemetryRxMs = gpsClient.telemetryRxMs;
static int &clientHumBaselinePct = gpsClient.humidityBaselinePct;
static int stationHumBaselinePct = -1;

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
static float mountOffsetDeg = 90.0f;
static float declinationDeg = 0.0f;
static bool declinationReady = false;
static bool mountCalibrated = true; // explicit default: 90-degree reference is usable
static bool gpsPredictionEnabled = false;  // alpha = 1; false selects alpha = 0
static bool gpsFinishingTarget = false;
static station_position::Average stationAverage;
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
static packet_rate::Window10s loraDataRate;
static uint32_t rxTelemetryCount = 0;
static uint32_t rxDropCount = 0;
static uint32_t rxErrorCount = 0;
static packet_diagnostics::Ring<64> packetEvents;
static uint32_t rejectedLength = 0, rejectedFormat = 0, rejectedBinding = 0;
static uint32_t invalidFixPackets = 0, invalidVelocityPackets = 0;
static uint32_t lastDataIntervalMs = 0, maxDataIntervalMs = 0;
static DiagnosticPayload lastClientDiagnostic{};
static gnss_diagnostics::Latest clientGnssDiagnostic;
static uint32_t rxGnssDiagnosticCount = 0;
static bool haveClientDiagnostic = false;
static uint32_t lastClientDiagnosticMs = 0, rxDiagnosticCount = 0;
static uint32_t sequenceMissing = 0, sequenceResyncs = 0, rxWinMissing = 0;
static bool stationRxReady = true;
static uint32_t nextStationRxRetryMs = 0;

// Per-minute RX summary window (see RX_SUMMARY_MS).
static uint32_t nextRxSummaryMs = 0;
static uint32_t rxWinData = 0, rxWinTelem = 0, rxWinDrop = 0, rxWinErr = 0;
static int      rxWinLastErr = 0;
static uint16_t rxWinFirstSeq = 0, rxWinLastSeq = 0;
static bool     rxWinHaveSeq = false;
static float    rxWinRssiMin = 0, rxWinRssiMax = 0, rxWinSnrMin = 0, rxWinSnrMax = 0;
static double   rxWinRssiSum = 0, rxWinSnrSum = 0;

http_timing::Server<WebServer, MetricsClock> httpServer(80);

// Station stays in RX; DIO1 reports packet completion.
volatile bool stationRadioIrq = false;
volatile uint32_t stationRadioIrqMs = 0;
void IRAM_ATTR onLoRaDio1() {
  stationRadioIrqMs = millis();
  stationRadioIrq = true;
}
#endif

#if defined(ROLE_STATION)
// Decode helpers enforce exact v5 wire lengths before any payload is used.
static bool parseDataPacket(const uint8_t *buf, size_t n, DecodedData &out) {
  PacketHeader hdr{}; PositionPayload pos{};
  if (!protocol::decodeData(buf, n, hdr, pos) || !isClientAllowed(hdr.clientId)) return false;
  out.srcId = hdr.clientId; out.seq = hdr.seq;
  out.fix = pos.fix; out.lat = pos.latE7 / 1e7; out.lon = pos.lonE7 / 1e7;
  out.speedCmS = pos.speedDmS == 255 ? UINT16_MAX : uint16_t(pos.speedDmS) * 10;
  out.courseDeg10 = pos.courseDeg10;
  out.satelliteClass = pos.satelliteClass;
  out.satellites = protocol::satLowerBound(pos.satelliteClass);
  out.hdop10 = pos.hdop10;
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
#if defined(ROLE_CLIENT)
  ClientRailGuard guard;
  if (!guard.held) return cachedBatteryMv;
#endif
#if defined(ROLE_STATION)
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
#if defined(ROLE_CLIENT)
  ClientRailGuard guard;
  if (!guard.held) return false;
#endif
#if defined(ROLE_STATION)
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

#if defined(ROLE_CLIENT) && defined(FIELD_DIAGNOSTIC)
  pmuBatteryInit[0] = pmu.enableBattDetection() ? 1 : 0;
  pmuBatteryInit[1] = pmu.enableVbusVoltageMeasure() ? 1 : 0;
  pmuBatteryInit[2] = pmu.enableBattVoltageMeasure() ? 1 : 0;
  pmuBatteryInit[3] = pmu.enableSystemVoltageMeasure() ? 1 : 0;
#else
  pmu.enableBattDetection();
  pmu.enableVbusVoltageMeasure();
  pmu.enableBattVoltageMeasure();
  pmu.enableSystemVoltageMeasure();
#endif

  // GPS rail (ALDO4) is needed by both roles.
  pmu.setALDO4Voltage(3300);
  pmu.enableALDO4();

  Log.println(F("[PMU] AXP2101 init ok"));
  return true;
}

#if defined(ROLE_CLIENT) && defined(FIELD_DIAGNOSTIC)
static int readPmuBatteryDiagnosticRegister(uint8_t reg) {
  // Only set the register pointer. Do not use XPowers readRegister here: its
  // unchecked short requestFrom can enter Stream::readBytes' 1-second timeout.
  PMUWire.beginTransmission(AXP2101_SLAVE_ADDRESS);
  const bool addressed = PMUWire.write(reg) == 1;
  const uint8_t status = PMUWire.endTransmission();
  if (!addressed) return -100;
  if (status) return -100 - status;
  if (PMUWire.requestFrom(AXP2101_SLAVE_ADDRESS, uint8_t(1)) != 1) return -200;
  if (PMUWire.available() < 1) return -201;
  const int value = PMUWire.read();
  return value >= 0 && value <= 255 ? value : -202;
}

static void recordClientBatteryDiagnostic(const char *stage) {
  const uint32_t now = millis();
  // STATUS1/2, ADC enable, VBAT high then low, battery detection enable,
  // long-press power-off enable and PWR key timing configuration.
  // These are not read-clear IRQ registers. Preserve raw values for analysis;
  // none of these observations participate in the existing shutdown decision.
  static constexpr uint8_t registers[] = {0x00, 0x01, 0x30, 0x34, 0x35, 0x68, 0x22, 0x27};
  int raw[8] = {-2, -2, -2, -2, -2, -2, -2, -2};
  uint8_t failed = 0;
  bool locked = false;
  {
    ClientRailGuard guard;
    locked = guard.held;
    if (pmuOnline && locked) {
      for (size_t i = 0; i < sizeof(registers); ++i) {
        raw[i] = readPmuBatteryDiagnosticRegister(registers[i]);
        if (raw[i] < 0) failed |= uint8_t(1U << i);
      }
    }
  } // release the shared PMU/SD rail lock before queueing or USB output
  const uint32_t elapsed = millis() - now;
  char line[320];
  const int length = snprintf(line, sizeof(line),
      "{\"event\":\"pmu_battery\",\"stage\":\"%s\",\"boot_id\":%lu,\"ms\":%lu,"
      "\"online\":%s,\"lock\":%s,\"init\":[%d,%d,%d,%d],\"raw\":[%d,%d,%d,%d,%d,%d,%d,%d],"
      "\"read_fail\":%u,\"elapsed_ms\":%lu}",
      stage, (unsigned long)powerBootId, (unsigned long)now,
      pmuOnline ? "true" : "false", locked ? "true" : "false",
      pmuBatteryInit[0], pmuBatteryInit[1], pmuBatteryInit[2], pmuBatteryInit[3],
      raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], failed, (unsigned long)elapsed);
  if (length <= 0 || size_t(length) >= sizeof(line)) {
    Log.println(F("[PMU] battery diagnostic encoding overflow"));
    return;
  }
  diagnostic_store::submit(2, line, size_t(length), now);
  Log.println(line);
}
#endif

// Verification configuration: Quectel documents single-sentence output above
// 1 Hz. The user selected an explicit RMC+GGA 2 Hz experiment to retain fields.
// Do not claim success from writing commands; gpsRate reports actual arrivals.
#if defined(FIELD_DIAGNOSTIC)
static uint32_t diagnosticBootId = 0;
static field_diagnostic::Utc diagnosticUtc;
#if defined(ROLE_CLIENT)
#if defined(CLIENT_TRIP_LOG)
static trip_log::ContinuousPlan diagnosticPlan;
#else
static field_diagnostic::Plan diagnosticPlan;
#endif
#endif
static uint8_t diagnosticRaw[128];
static size_t diagnosticRawLength = 0;
static uint16_t diagnosticRawKind = 1;
static uint32_t diagnosticRawSplits = 0;
static loop_metrics::Gap diagnosticLoopGap;
#if defined(CLIENT_TRIP_LOG)
static uint32_t tripRawSequence = 0;
static void recordTripRaw(uint16_t kind, const uint8_t *data, size_t length, uint32_t now) {
  // Hex retains noise as well as valid NMEA in UTF-8 NDJSON. Sequence advances
  // even when SD cannot accept, so the next saved chunk reveals the gap.
  for (size_t offset = 0; offset < length; offset += trip_log::kRawChunk) {
    const size_t n = std::min(trip_log::kRawChunk, length-offset);
    char text[256];
    const size_t encoded = trip_log::encodeRaw(text, sizeof(text), tripRawSequence++, kind, data+offset, n);
    if (encoded) sd_log::text(reinterpret_cast<const uint8_t *>(text), encoded, now);
  }
}
#endif
static void diagnosticByte(char c, uint32_t now, bool discarded = false) {
  const uint16_t kind = discarded ? 6 : 1;
  if (diagnosticRawLength && kind != diagnosticRawKind) {
#if defined(CLIENT_TRIP_LOG)
    recordTripRaw(diagnosticRawKind, diagnosticRaw, diagnosticRawLength, now);
#else
    diagnostic_store::submit(diagnosticRawKind, diagnosticRaw, diagnosticRawLength, now);
#endif
    diagnosticRawLength = 0;
  }
  diagnosticRawKind = kind;
  diagnosticRaw[diagnosticRawLength++] = uint8_t(c);
  if (!discarded) diagnosticUtc.feed(c, now);
  if (c == '\n' || diagnosticRawLength == sizeof(diagnosticRaw)) {
    if (c != '\n') ++diagnosticRawSplits;
#if defined(CLIENT_TRIP_LOG)
    recordTripRaw(kind, diagnosticRaw, diagnosticRawLength, now);
#else
    diagnostic_store::submit(kind, diagnosticRaw, diagnosticRawLength, now);
#endif
    diagnosticRawLength = 0;
  }
}
#if defined(ROLE_CLIENT)
static void recordDiagnosticPhase(uint32_t now) {
  char line[160];
  snprintf(line, sizeof(line), "{\"phase\":%u,\"rf_target\":%s,\"sd_target\":%s,\"period_ms\":1000,\"phase_ms\":%lu}",
      diagnosticPlan.phase(), diagnosticPlan.rf() ? "true" : "false", diagnosticPlan.sd() ? "true" : "false",
#if defined(CLIENT_TRIP_LOG)
      0UL);
#else
      180000UL);
#endif
  diagnostic_store::submit(4, line, strlen(line), now);
}
#endif
static void serviceFieldDiagnostic() {
  const uint32_t now = millis();
  diagnosticLoopGap.observe(now);
  field_diagnostic::Metrics m;
#if defined(ROLE_CLIENT)
#if !defined(CLIENT_TRIP_LOG)
  gnss_snapshot::Snapshot sample;
  const bool usable = gnssCollector.sample(now, sample) && sample.fix && sample.haveGga &&
      sample.haveRmc && sample.arrivalAgeMs < 2000 && sample.satellites >= 6 &&
      isfinite(sample.hdop) && sample.hdop <= 3;
  bool changed = false;
  if (diagnostic_store::healthy()) changed = diagnosticPlan.observe(now, usable);
  else if (strcmp(diagnostic_store::stateName(), "scanning") && diagnosticPlan.phase() != 6) {
    diagnosticPlan.abort(); changed = true;
  }
  if (changed) {
    // RF gate stops NEW transmissions; an in-flight packet completes normally.
    // SD off drains/closes asynchronously before card power is removed.
    if (diagnosticPlan.sd()) sd_log::start(); else sd_log::stop();
    recordDiagnosticPhase(now);
  }
#endif
  m.phase = diagnosticPlan.phase(); m.rf = diagnosticPlan.rf(); m.sd = diagnosticPlan.sd();
  m.tx = clientTxCount; m.txErrors = clientTxErrors;
#else
  m.phase = 255; m.rf = true; m.sd = true; // Station keeps receiving throughout.
#endif
  static uint32_t nextRaw = 0;
  if (loop_metrics::due(now, nextRaw)) {
    nextRaw = now + 1000;
    if (diagnosticRawLength) {
#if defined(CLIENT_TRIP_LOG)
      recordTripRaw(diagnosticRawKind, diagnosticRaw, diagnosticRawLength, now);
#else
    diagnostic_store::submit(diagnosticRawKind, diagnosticRaw, diagnosticRawLength, now);
#endif
    diagnosticRawLength = 0; // preserve even an unterminated/noisy UART tail
    }
  }
  static uint32_t next = 0;
  if (!loop_metrics::due(now, next)) return;
#if defined(CLIENT_TRIP_LOG)
  next = now + trip_log::kSnapshotMs;
#else
  next = now + 1000;
#endif
  m.battery = cachedBatteryMv; m.heap = ESP.getFreeHeap(); m.backlog = gpsBacklogDrops;
  m.rawSplits = diagnosticRawSplits; m.loopGap = diagnosticLoopGap.maxMs;
  uint8_t bytes[80];
  field_diagnostic::encode(bytes, gnssCollector, diagnosticUtc, now, m);
  diagnostic_store::submit(3, bytes, sizeof(bytes), now);
  static uint32_t nextStatus = 0;
  if (loop_metrics::due(now, nextStatus)) {
#if defined(CLIENT_TRIP_LOG)
    nextStatus = now + trip_log::kSdStatusMs;
#else
    nextStatus = now + 10000;
#endif
    char health[144];
    snprintf(health, sizeof(health), "{\"event\":\"recorder_health\",\"write_max_us\":%lu,\"dropped\":%lu}",
        (unsigned long)diagnostic_store::writeMaxUs(), (unsigned long)diagnostic_store::dropped());
    diagnostic_store::submit(5, health, strlen(health), now);
    // Actual SD state, not merely the phase target (a card operation may fail).
    const String status = sd_log::statusJson();
    // Preserve the full status in ordered chunks; kind 8 is UTF-8 JSON stream.
    for (size_t i = 0; i < status.length(); i += 400)
      diagnostic_store::submit(8, status.c_str()+i, std::min(size_t(400), status.length()-i), now);
    const char end = '\n'; diagnostic_store::submit(8, &end, 1, now);
  }
}
#endif

static void configureGps() {
  pinMode(GPS_EN_PIN, OUTPUT);
  digitalWrite(GPS_EN_PIN, HIGH);
  GPSSerial.setRxBufferSize(GPS_RX_BUFFER_BYTES);
  GPSSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  delay(1000); // boot only, before control starts; allow the GNSS command parser to start
  GPSSerial.print("$PCAS01,5*19\r\n");
  GPSSerial.flush();
  delay(100);
  GPSSerial.updateBaudRate(GPS_BAUD);
  // Handles an MCU reboot while the powered GNSS already remains at 115200.
  GPSSerial.print("$PCAS01,5*19\r\n");
  GPSSerial.flush();
  delay(100);
  GPSSerial.print("$PCAS03,1,0,0,0,1,0,0,0,0,0,,,0,0*02\r\n");
#if defined(FIELD_DIAGNOSTIC)
  GPSSerial.print("$PCAS02,1000*2E\r\n");
#else
  GPSSerial.print("$PCAS02,500*1A\r\n");
#endif
  GPSSerial.flush();
  gnssCollector.reset(millis());
  gpsServiceStarted = false;
#if defined(FIELD_DIAGNOSTIC)
  Log.println(F("[GNSS] diagnostic baseline: requested 115200 baud, 1 Hz RMC+GGA"));
#else
  Log.println(F("[GNSS] requested 115200 baud, 2 Hz RMC+GGA verification configuration"));
#endif
  Log.println(F("[GNSS] command writes are not verification; observed rates follow every 5 s"));
}

static void serviceGps() {
  const uint32_t now = millis();
  if (!gpsServiceStarted || now - gpsLastServiceMs > GPS_BACKLOG_GUARD_MS) {
    // Discard bytes accumulated while blocked (including the boot screen).
    // The collector waits for a new '$' and a newer epoch after this reset.
    gnssCollector.invalidate(now);
#if defined(FIELD_DIAGNOSTIC)
    diagnosticUtc.invalidate();
#endif
    while (GPSSerial.available() > 0) {
      const char discarded = static_cast<char>(GPSSerial.read());
#if defined(FIELD_DIAGNOSTIC)
      diagnosticByte(discarded, millis(), true);
#else
      (void)discarded;
#endif
    }
    if (gpsServiceStarted) ++gpsBacklogDrops;
    gpsServiceStarted = true;
  }
  gpsLastServiceMs = now;
  while (GPSSerial.available() > 0) {
    const char c = static_cast<char>(GPSSerial.read());
#if defined(FIELD_DIAGNOSTIC)
    diagnosticByte(c, millis());
#endif
    gps.encode(c);  // retained for UTC date and legacy display metadata
    gnssCollector.feed(c, millis());
  }
  const auto &g = gnssCollector.stats();
  if (gpsRate.observe(now, g.snapshots, g.rmcSentences, g.ggaSentences)) {
    Log.print(F("[GNSS] ")); Log.print(gpsRate.state());
    Log.print(F(" epoch_hz=")); Log.print(gpsRate.hz(), 2);
    Log.print(F(" rmc_hz=")); Log.print(gpsRate.rmcHz(), 2);
    Log.print(F(" gga_hz=")); Log.println(gpsRate.ggaHz(), 2);
  }
#if defined(ROLE_STATION)
  gnss_snapshot::Snapshot station;
  const bool sampled = gnssCollector.sample(now, station);
  stationAverage.observe(now, sampled && station.fix &&
      station.arrivalAgeMs < GPS_FIX_MAX_AGE_MS,
      station.epochMsOfDay, station.lat, station.lon);
#endif
}

// 「現在真的有定位」。見 GPS_FIX_MAX_AGE_MS —— isValid() 單獨用是不夠的。
static bool gpsFixFresh() {
  gnss_snapshot::Snapshot sample;
  return gnssCollector.sample(millis(), sample) && sample.fix &&
         sample.arrivalAgeMs < GPS_FIX_MAX_AGE_MS;
}
#if defined(ROLE_STATION)
static double stationLatitude() {
  if (stationAverage.count()) return stationAverage.latitude();
  gnss_snapshot::Snapshot sample; gnssCollector.sample(millis(), sample); return sample.lat;
}
static double stationLongitude() {
  if (stationAverage.count()) return stationAverage.longitude();
  gnss_snapshot::Snapshot sample; gnssCollector.sample(millis(), sample); return sample.lon;
}
#endif

// OLED address detection must remain independent of the removed magnetometer.
// On the N board the OLED is 0x3D and 0x3C belongs to the unused QMC sensor.
[[maybe_unused]] static void detectOledAddress() {
  Wire.beginTransmission(OLED_ADDR_ALT);
  oledI2CAddr = Wire.endTransmission() == 0 ? OLED_ADDR_ALT : OLED_ADDR_DEFAULT;
}

#if defined(ROLE_STATION)
static bool initStationDisplay() {
  const bool powered = pmuOnline && pmu.setALDO1Voltage(3300) && pmu.enableALDO1() &&
      pmu.isEnableALDO1() && pmu.getALDO1Voltage() == 3300;
  Log.print(F("[OLED] ALDO1 3300 mV readback=")); Log.println(powered ? "ok" : "failed");
  if (!powered) return false;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    if (attempt) { Wire.end(); delay(10); }
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN, 100000);
    Wire.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);
    bool found = false;
    for (uint8_t addr : {OLED_ADDR_ALT, OLED_ADDR_DEFAULT}) {
      Wire.beginTransmission(addr);
      const uint8_t result = Wire.endTransmission();
      Log.print(F("[OLED] probe addr=")); Log.print(addr, HEX);
      Log.print(F(" result=")); Log.println(result);
      if (result == 0) { oledI2CAddr = addr; found = true; break; }
    }
    if (found) {
      display.setI2CAddress(oledI2CAddr << 1);
      display.setBusClock(100000);
      if (!display.begin()) continue;
      display.setPowerSave(0);
      display.setContrast(255);
      Log.println(F("[OLED] initialized; panel visibility requires visual confirmation"));
      return true;
    }
  }
  Log.print(F("[OLED] unavailable; SDA=")); Log.print(digitalRead(OLED_SDA_PIN));
  Log.print(F(" SCL=")); Log.println(digitalRead(OLED_SCL_PIN));
  Log.println(F("[OLED] display writes disabled; LoRa and SD continue"));
  return false;
}
#endif

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
#if defined(ROLE_STATION)
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
#if defined(ROLE_STATION)
  // 第一筆有效讀數當成基準，之後用「上升幅度」而不是絕對值判斷受潮（見 alerts.h）。
  if (stationHumBaselinePct < 0) stationHumBaselinePct = (int)cachedHumidityPct;
#endif
}

// 8x8 lightning icon (LSB-first rows for U8g2 drawXBMP), drawn when the board
// is on external/USB power.
static const uint8_t ICON_BOLT_8[] = {0x38,0x0C,0x06,0x1F,0x1C,0x0C,0x06,0x03};

#if defined(ROLE_STATION)
static uint32_t boundRfAgeMs() {
  const uint32_t now = millis();
  uint32_t age = havePkt ? uint32_t(now-lastRxMs) : UINT32_MAX;
  if (haveTelemetry) age = std::min(age, uint32_t(now-lastTelemetryRxMs));
  if (haveClientDiagnostic) age = std::min(age, uint32_t(now-lastClientDiagnosticMs));
  if (clientGnssDiagnostic.received()) age = std::min(age, clientGnssDiagnostic.rxAgeMs(now));
  return age;
}
static SigLevel stationGpsState() {
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

static void loadStationSettings() {
  prefs.begin("shorespotter", false);
  loadGpsClientBinding();
  double speed=servo_motion::kDefaultSpeed;
  servo_motion::decodeSpeed(prefs.getUInt(servo_motion::kSpeedKey,0),speed);
  servoMotion.setSpeed(speed);
  const uint32_t prediction = prefs.getUInt(GPS_PREDICTION_KEY, 0);
  gpsPredictionEnabled = prediction <= 1 ? prediction == 1 : false;
  // Old acceleration/jerk/deadband profiles are deliberately ignored.
  // Old mount* and mag* NVS keys are ignored. Boot uses a valid 90-degree
  // reference; manual calibration replaces it in RAM until the next boot.
  mountOffsetDeg = 90.0f;
  mountCalibrated = true;
}


#endif

#if defined(ROLE_CLIENT)
static bool clientFixUsable(const gnss_snapshot::Snapshot &sample) {
  return sample.fix && isfinite(sample.lat) && isfinite(sample.lon) &&
      sample.arrivalAgeMs < GPS_FIX_MAX_AGE_MS &&
      sample.lat >= -90 && sample.lat <= 90 && sample.lon >= -180 && sample.lon <= 180 &&
      protocol::coordinatesFit(static_cast<int32_t>(lround(sample.lat * 1e7)),
                               static_cast<int32_t>(lround(sample.lon * 1e7)));
}

static size_t buildDataPacket(uint8_t *buf, const gnss_snapshot::Snapshot *chosen = nullptr) {
  const PacketHeader hdr{nodeId, txSeq++, MSG_DATA};
  PositionPayload pos{};
  pos.speedDmS = 255; pos.courseDeg10 = 4095;
  pos.hdop10 = 255;
  gnss_snapshot::Snapshot sample;
  if (chosen ? (sample = *chosen, true) : gnssCollector.sample(millis(), sample)) {
    const bool fresh = sample.arrivalAgeMs < tracking_policy::kGpsFreshMs;
    pos.fix = clientFixUsable(sample);
    if (pos.fix) {
      pos.latE7 = static_cast<int32_t>(lround(sample.lat * 1e7));
      pos.lonE7 = static_cast<int32_t>(lround(sample.lon * 1e7));
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
      sample.arrivalAgeMs < tracking_policy::kGpsFreshMs ? sample.satellites : 255;
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
  // v5 DIAG rate flags in the two high bits: observed rates,
  // not a claim that receiver configuration commands were acknowledged.
  if (gpsRate.ready()) diag.status |= 64;
  if (strcmp(gpsRate.state(), "observed_2hz") == 0) diag.status |= 128;
  return protocol::encodeDiagnostic(buf, DIAGNOSTIC_PACKET_LEN, hdr, diag);
}

static size_t buildGnssDiagnosticPacket(uint8_t *buf) {
  const PacketHeader hdr{nodeId, gnssDiagnosticSeq++, MSG_GNSS_DIAGNOSTIC};
  return gnss_diagnostics::encode(buf, GNSS_DIAGNOSTIC_PACKET_LEN, hdr,
                                  gnss_diagnostics::capture(gnssCollector, millis()));
}
#endif

// ALDO3 supplies the SX1262. Reapply and read back before boot/recovery SPI access.
// Readback proves the PMU configuration, not the measured voltage/RF output.
static bool prepareRadioPower() {
#if defined(ROLE_CLIENT)
  ClientRailGuard guard;
  if (!guard.held) { Log.println(F("[LoRa] PMU busy; defer radio recovery")); return false; }
#endif
  if (!pmuOnline || !pmu.setALDO3Voltage(3300) || !pmu.enableALDO3() ||
      !pmu.isEnableALDO3() || pmu.getALDO3Voltage() != 3300) {
    Log.println(F("[LoRa] ERROR: ALDO3 3300 mV configuration/readback failed"));
    return false;
  }
  delay(10);  // rail settling before the radio reset/SPI sequence
  Log.println(F("[LoRa] ALDO3 configured 3300 mV, enabled (readback verified)"));
  return true;
}

static bool initRadio() {
  if (!prepareRadioPower()) return false;
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  int state = radio.begin(RF_FREQUENCY, RF_BW, RF_SF, RF_CR, RF_SYNC_WORD, TX_POWER_DBM);
  if (state != RADIOLIB_ERR_NONE) {
    Log.print(F("[LoRa] init failed, code="));
    Log.println(state);
    return false;
  }
#if defined(ROLE_STATION)
  state = radio.setRxBoostedGainMode(true);
  if (state != RADIOLIB_ERR_NONE) {
    Log.print(F("[LoRa] RX boosted gain failed, code="));
    Log.println(state);
    return false;
  }
  Log.println(F("[LoRa] RX boosted gain enabled"));
#endif
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
  telemetryAirtimeMs = (radio.getTimeOnAir(TELEMETRY_PACKET_LEN) + 999) / 1000;
  diagnosticAirtimeMs = (radio.getTimeOnAir(DIAGNOSTIC_PACKET_LEN) + 999) / 1000;
  gnssDiagnosticAirtimeMs = (radio.getTimeOnAir(GNSS_DIAGNOSTIC_PACKET_LEN) + 999) / 1000;
  telemetrySlotMinMs = dataAirtimeMs + TELEMETRY_SLOT_GUARD_MS;
  telemetrySlotMaxMs = SEND_INTERVAL_MS > telemetryAirtimeMs + TELEMETRY_SLOT_GUARD_MS ?
      SEND_INTERVAL_MS - telemetryAirtimeMs - TELEMETRY_SLOT_GUARD_MS : 0;
  Log.print(F("[LoRa] SF=")); Log.print(RF_SF);
  Log.println(F(" uplink only (ACK disabled)"));
  Log.print(F("[LoRa] airtime_ms DATA/TEL/DIAG/GNSS="));
  Log.print(dataAirtimeMs); Log.print('/'); Log.print(telemetryAirtimeMs);
  Log.print('/'); Log.print(diagnosticAirtimeMs); Log.print('/'); Log.println(gnssDiagnosticAirtimeMs);
  Log.print(F("[LoRa] requested GNSS interval_ms=")); Log.print(SEND_INTERVAL_MS);
  Log.println(F(" latest valid fix, no periodic DATA heartbeat/retry"));
  Log.print(F("[LoRa] diagnostics interval_ms=")); Log.println(TELEMETRY_INTERVAL_MS);
  if (telemetrySlotMaxMs < telemetrySlotMinMs)
    Log.println(F("[LoRa] diagnostics use dedicated slots, alternating with DATA"));
}

// --- 無線電回復 ------------------------------------------------------------
// SX1262 若因為 SPI 干擾或狀態機卡住而停止工作，原本兩端都只會印一行 log 然後
// 安靜地永遠壞下去。CLIENT 平時螢幕是關的，沒有任何外部徵兆；STATION 則是站在
// 沙灘上的人完全不知道為什麼鏡頭不動了。這裡在連續失敗到一定次數後重新初始化。
constexpr uint8_t RADIO_TX_FAIL_LIMIT = 5;    // client：連續 5 次 TX／RX 恢復失敗
constexpr uint16_t RADIO_RX_ERR_LIMIT = 30;   // station：連續 30 次讀取錯誤
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
  radio.setDio1Action(onClientDio1);
  clientTxFlag = false;
  if (radio.standby() != RADIOLIB_ERR_NONE) {
    Log.println(F("[LoRa] ERROR: standby failed after re-init"));
    return false;
  }
#else
  radio.setDio1Action(onLoRaDio1);
  stationRadioIrq = false;
  if (radio.startReceive() != RADIOLIB_ERR_NONE) {
    Log.println(F("[LoRa] ERROR: RX restart failed after re-init"));
    return false;
  }
#endif
  radioFailStreak = 0;
  Log.println(F("[LoRa] radio re-init ok"));
  return true;
}

#if defined(ROLE_CLIENT)
static void serviceClientSd(const gnss_snapshot::Snapshot &sample) {
  const uint32_t now = millis();
#if defined(CLIENT_TRIP_LOG)
  const bool allowed = true; // trip-only: preserve cold start and lost-fix data
#else
  const bool allowed = clientLogGate.observe(now, sample);
#endif
  static bool wasAllowed = false, haveRecordedEpoch = false;
  static uint32_t recordedEpoch = 0, nextCheck = 0;
  // Policy is cheap; detailed copies happen only on an epoch/transition or a
  // 100 ms resume check while the worker finishes draining the previous part.
  const bool newEpoch = allowed && sample.haveRmc && sample.haveGga &&
      (!haveRecordedEpoch || sample.epochMsOfDay != recordedEpoch);
  if (allowed == wasAllowed && !newEpoch && !loop_metrics::due(now, nextCheck)) return;
  nextCheck = now + 100; wasAllowed = allowed;
  sd_log::ClientRecord r;
  r.ms = now; r.nodeId = nodeId; r.gps = sample; r.gnss = gnssCollector.stats();
  r.byteAgeMs = gnssCollector.byteAgeMs(now); r.sentenceAgeMs = gnssCollector.sentenceAgeMs(now);
  r.recovering = gnssCollector.recovering(); r.backlogDrops = gpsBacklogDrops;
  r.txCount = clientTxCount; r.txErrors = clientTxErrors; r.skipped = dataSkippedSlots;
  r.radioRestarts = radioRecoverCount; r.batteryMv = cachedBatteryMv;
  r.heapFree = ESP.getFreeHeap(); r.loopGapMaxMs = clientLoopGap.maxMs; r.loopOver250 = clientLoopGap.over250ms;
  r.epochHz = gpsRate.hz(); r.rmcHz = gpsRate.rmcHz(); r.ggaHz = gpsRate.ggaHz();
  sd_log::clientGps(allowed, r);
  if (newEpoch) {
    sd_log::clientEvent(r); recordedEpoch = sample.epochMsOfDay; haveRecordedEpoch = true;
  }
  if (!allowed) haveRecordedEpoch = false;
}
static void recordClientTx(const async_lora_tx::Result &result) {
  using async_lora_tx::Event;
  if (result.event == Event::None) return;
  sd_log::ClientRecord r;
  switch (result.event) {
    case Event::Started: r.kind = sd_log::ClientKind::TxStarted; break;
    case Event::Sent: r.kind = sd_log::ClientKind::TxSent; break;
    case Event::Timeout: r.kind = sd_log::ClientKind::TxTimeout; break;
    case Event::Cancelled: r.kind = sd_log::ClientKind::TxCancelled; break;
    default: r.kind = sd_log::ClientKind::TxFailed; break;
  }
  r.ms = millis(); r.txStartedMs = clientLogTxStarted; r.nodeId = nodeId; r.txStatus = result.txStatus;
  r.length = clientLogTxLength; memcpy(r.raw, clientTxBuffer, r.length);
  sd_log::clientEvent(r);
#if defined(FIELD_DIAGNOSTIC) && !defined(CLIENT_TRIP_LOG)
  uint8_t record[MAX_PACKET_LEN + 3];
  record[0] = uint8_t(result.event); record[1] = uint8_t(result.txStatus);
  record[2] = uint8_t(uint16_t(result.txStatus) >> 8);
  memcpy(record+3, r.raw, r.length);
  diagnostic_store::submit(7, record, r.length+3, r.ms);
#endif
}
static void handleClientTxResult(const async_lora_tx::Result &result) {
  using async_lora_tx::Event;
  recordClientTx(result);
  if (result.event == Event::None || result.event == Event::Started) return;
  nextClientTxMs = millis() + TELEMETRY_SLOT_GUARD_MS;
  clientRadioReady = result.txStatus == RADIOLIB_ERR_NONE;
  if (!clientRadioReady) nextClientRadioRetryMs = millis() + 100;
  if (result.event == Event::Sent) {
    radioFailStreak = 0;
    if (clientSendingData) { ++clientTxCount; haveDataSent = true; }
  } else {
    ++clientTxErrors;
    haveDataSent = false;
    Log.print(F("[CLIENT] TX error=")); Log.println(result.txStatus);
    if (++radioFailStreak >= RADIO_TX_FAIL_LIMIT) clientRadioReady = recoverRadio();
  }
}

static void serviceClientRadio() {
  handleClientTxResult(clientTransmitter.service(millis()));
  if (clientTransmitter.active()) return;
  if (!clientRadioReady && loop_metrics::due(millis(), nextClientRadioRetryMs)) {
    clientTxFlag = false;
    clientRadioReady = radio.standby() == RADIOLIB_ERR_NONE;
    nextClientRadioRetryMs = millis() + 100;
    if (!clientRadioReady && ++radioFailStreak >= RADIO_TX_FAIL_LIMIT) clientRadioReady = recoverRadio();
  }
}

static void sendClientDiagnostic(bool sendDiagnostic, bool sendGnss, uint32_t extraMs) {
  const size_t length = sendGnss ? buildGnssDiagnosticPacket(clientTxBuffer) :
      sendDiagnostic ? buildDiagnosticPacket(clientTxBuffer) : buildTelemetryPacket(clientTxBuffer);
  if (!length) { ++clientTxErrors; return; }
  const uint32_t now = millis();
  if (sendGnss) nextGnssDiagnosticMs = now + TELEMETRY_INTERVAL_MS;
  else if (sendDiagnostic) nextDiagnosticMs = now + TELEMETRY_INTERVAL_MS;
  else nextTelemetryMs = now + TELEMETRY_INTERVAL_MS;
  // Diagnostics also run without GPS; at most one extra per second.
  haveDataSent = false; clientSendingData = false; clientRadioReady = false;
  nextClientTxMs = now + extraMs + TELEMETRY_SLOT_GUARD_MS;
  nextClientExtraMs = now + 1000;
  ++diagnosticTxCount;
  clientLogTxLength = length; clientLogTxStarted = now;
  handleClientTxResult(clientTransmitter.start(clientTxBuffer, length, now,
                                              extraMs + TELEMETRY_SLOT_GUARD_MS));
}

static void serviceClientTransmit() {
  serviceClientRadio();
  serviceGps();  // drain UART before choosing the newest coherent sample
  const uint32_t now = millis();
  gnss_snapshot::Snapshot sample;
  const bool haveSample = gnssCollector.sample(now, sample);
  serviceClientSd(sample);
#if defined(FIELD_DIAGNOSTIC)
  if (!diagnosticPlan.rf()) return; // completed TX is serviced above; no new RF
#endif
  const bool validFix = haveSample && clientFixUsable(sample);
  const bool dataDue = clientCadence.due(now, validFix, sample.epochMsOfDay,
                                       sample.haveRmc && sample.haveGga);
  if (clientTransmitter.active() || !clientRadioReady || !loop_metrics::due(now, nextClientTxMs)) {
    if (dataDue && !clientDataDeferred) { ++dataSkippedSlots; clientDataDeferred = true; }
    return;
  }
  const bool telemetryDue = loop_metrics::due(now, nextTelemetryMs);
  const bool diagnosticDue = loop_metrics::due(now, nextDiagnosticMs);
  const bool gnssDue = loop_metrics::due(now, nextGnssDiagnosticMs);
  const bool sendDiagnostic = diagnosticDue && !telemetryDue;
  const bool sendGnss = gnssDue && !telemetryDue && !diagnosticDue;
  const uint32_t extraMs = sendGnss ? gnssDiagnosticAirtimeMs : sendDiagnostic ? diagnosticAirtimeMs : telemetryAirtimeMs;
  if ((telemetryDue || diagnosticDue || gnssDue) && loop_metrics::due(now, nextClientExtraMs) &&
      (!dataDue || (validFix && haveDataSent))) {
    sendClientDiagnostic(sendDiagnostic, sendGnss, extraMs);
    return;
  }
  if (!dataDue) { clientDataDeferred = false; return; }
  clientDataDeferred = false;
  clientCadence.attempted(now, validFix, sample.epochMsOfDay);
  const size_t length = buildDataPacket(clientTxBuffer, haveSample ? &sample : nullptr);
  if (!length) { ++clientTxErrors; return; }
  lastSendMs = millis(); haveDataSent = false;
  nextClientTxMs = lastSendMs + dataAirtimeMs + TELEMETRY_SLOT_GUARD_MS;
  clientSendingData = true; clientRadioReady = false;
  clientLogTxLength = length; clientLogTxStarted = lastSendMs;
  handleClientTxResult(clientTransmitter.start(clientTxBuffer, length, lastSendMs,
                                              dataAirtimeMs + TELEMETRY_SLOT_GUARD_MS));
}

#endif

#if defined(ROLE_STATION)
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
    event.sourceAgeMs = UINT16_MAX;  // v5 DATA carries no source-age estimate
    event.flags = (data->fix ? 1 : 0) | (data->velocityValid ? 2 : 0);
  }
  packetEvents.push(event);
  event.id = packetEvents.total();
  sd_log::packet(event, raw, length);
}



static void serviceStationRadio() {
  MeasureDuration timing(loraDuration);
  if (!stationRxReady &&
      loop_metrics::due(millis(), nextStationRxRetryMs)) {
    stationRadioIrq = false;
    const int16_t status = radio.startReceive();
    stationRxReady = status == RADIOLIB_ERR_NONE;
    nextStationRxRetryMs = millis() + 100;
    if (!stationRxReady && ++radioFailStreak >= RADIO_RX_ERR_LIMIT) {
      stationRxReady = recoverRadio();
    }
  }
}
#endif

#if defined(ROLE_STATION)
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
  gpsFinishingTarget = false;
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
  gpsClientId = id;
  gpsClient = ClientState{};
  gpsSequence = command_freshness::RadioSequence{};
  haveClientDiagnostic = false;
  clientGnssDiagnostic = gnss_diagnostics::Latest{};
  lastDataIntervalMs = 0;
  rssiRingIdx = rssiRingCount = 0;
  pktsThisWindow = pktWindowStartMs = 0;
  cachedPktRate = 0.0f;
  rxWinData = rxWinTelem = rxWinDrop = rxWinErr = rxWinMissing = 0;
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
    const bool finishGps = trackMode == TrackMode::Gps &&
        controlSource == tracking_policy::Source::Gps && next == tracking_policy::Source::Hold;
    gpsFinishingTarget = finishGps;
    controlSource = next;
    if (!finishGps) servoMotion.holdUs(micros());
    Log.print(F("[SERVO] source -> "));
    Log.println(tracking_policy::sourceName(controlSource));
  }
  if (trackMode == TrackMode::Uart && controlSource == tracking_policy::Source::Uart) {
    if (!servoMotion.target(uartServoMode.targetMdeg() / 1000.0)) servoMotion.holdUs(micros());
  }
  const bool permitted = servoPwmReady && (trackMode == TrackMode::Manual ||
      controlSource == tracking_policy::Source::Gps ||
      (trackMode == TrackMode::Gps && gpsFinishingTarget) ||
      controlSource == tracking_policy::Source::Uart);
  if (!permitted) servoMotion.holdUs(micros());
  if (gpsFinishingTarget && !servoMotion.moving()) gpsFinishingTarget = false;
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
#if defined(FIELD_DIAGNOSTIC)
  controlBootId = diagnosticBootId;
#else
  controlBootId = esp_random();
#endif
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
  // This is reception age only; the Client's GNSS internal latency is unknown.
  return havePkt ? uint32_t(millis() - lastRxMs) : UINT32_MAX;
}
static bool clientFixFresh() {
  return havePkt && lastData.fix && clientSampleAgeMs() < tracking_policy::kGpsFreshMs;
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

// Valid current positions permit tracking; poor quality only blocks prediction.
static bool gpsModeAvailable() { return haveBearingFix(); }
static bool gpsPredictionAllowed() {
  gnss_snapshot::Snapshot sample;
  return gpsModeAvailable() && gnssCollector.sample(millis(), sample) &&
      tracking_policy::predictionQuality(sample.satellites, sample.hdop) &&
      tracking_policy::predictionQuality(lastData.satellites,
                                        lastData.hdop10 == 255 ? NAN : lastData.hdop10 / 10.0f) &&
      lastData.velocityValid && lastData.speedCmS != UINT16_MAX &&
      lastData.speedCmS >= DR_MIN_SPEED_CMS && lastData.courseDeg10 < 3600;
}
static bool gpsTrackingUsable() {
  return mountCalibrated && declinationReady && gpsModeAvailable();
}

// Project the last received client position forward along its velocity vector.
//
// Optional prediction uses only elapsed time since RX, not unverified GNSS age.
// It starts from the received position and is blocked by poor/unknown quality.
static void predictClientPos(double &lat, double &lon) {
  lat = lastData.lat;
  lon = lastData.lon;
  if (!gpsPredictionEnabled || !gpsPredictionAllowed()) return;
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
  Log.print(F("[STATION] RX 60s | pkt="));
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

  rxWinData = rxWinTelem = rxWinDrop = rxWinErr = rxWinMissing = 0;
  rxWinHaveSeq = false;
  rxWinRssiSum = rxWinSnrSum = 0;
}

static void renderStationDisplay() {
  if (!oledOnline) return;
  MeasureDuration timing(oledDuration);
  // Centre label column ("V" / T/H / GPS / BAT) is framed by two vertical lines;
  // Station values sit left of it, client values right of it. The 15 px labels get
  // a 1 px gap to each line; the odd rounding pixel is biased to the right.
  const int leftLineX = 55;
  const int rightLineX = 73;
  const int leftCx = 27;   // centre of the Station (left) region
  const int rightCx = 100; // centre of the Client (right) region
  const int midCx = 64;    // centre of the label column

  SigLevel sGps = stationGpsState();

  const uint16_t selectedId = gpsClientId;
  bool selectedOnline = havePkt && (lastData.srcId == selectedId) &&
                        ((millis() - lastRxMs) <= LINK_WARN_MS);
  bool selectedTelemetry = haveTelemetry && (lastTelemetry.srcId == selectedId) && millis()-lastTelemetryRxMs < 90000;

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
  drawLeft(8, "Station");
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
  snprintf(buf, sizeof(buf), "LoRa:%s", !selectedOnline && boundRfAgeMs() < 90000 ? "TEL" : sig4Text(loraState));
  drawAt(96, 52, buf);
#if defined(FIELD_DIAGNOSTIC)
  snprintf(buf, sizeof(buf), "F:%s", diagnostic_store::stateName());
  drawAt(30, 52, buf);
#endif

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
static void serviceStationDisplay() {
  if (!oledOnline) return;
  if(oledNextRow>=8)return;
  MeasureDuration timing(oledDuration);
  display.updateDisplayArea(0,oledNextRow++,16,1);
  if(oledNextRow==8)++oledFrames;
}

#endif

#if defined(ROLE_STATION)
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
  js += F(",\"default_enabled\":false,");
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
    httpServer.send(409,"application/json","{\"ok\":false,\"error\":\"GPS requires current valid positions from Station and Client\"}");
    return false;
  }
  return selectTrackingMode(mode);
}

// Exact commanded centre, with no pending movement. There is no physical
// position feedback; the operator must also wait for the camera to settle.
static bool compassCalibrationReady() {
  return servoAngleDeg == 90.0f && servoMotion.position() == 90.0 &&
         servoMotion.requested() == 90.0 && !servoMotion.moving();
}

static void appendServoJson(String &js) {
  js += F("\"servo\":{\"angle\":");
  js += String(servoAngleDeg, 1);
  js += F(",\"target\":");
  js += String(servoTargetDeg, 1);
  js += F(",\"moving\":"); js += servoMotion.moving() ? F("true") : F("false");
  js += F(",\"finishing_last_gps_target\":"); js += gpsFinishingTarget ? F("true") : F("false");
  js += F(",\"speed_limit_deg_s\":"); js += String(servoMotion.speed(),3);
  js += F(",\"prediction_enabled\":"); js += gpsPredictionEnabled ? F("true") : F("false");
  js += F(",\"prediction_alpha\":"); js += gpsPredictionEnabled ? '1' : '0';
  js += F(",\"prediction_active\":"); js += gpsPredictionEnabled && trackMode == TrackMode::Gps && gpsTrackingUsable() && gpsPredictionAllowed() ? F("true") : F("false");
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
  js += F(",\"calibration_ready\":");
  js += compassCalibrationReady() ? F("true") : F("false");
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

static void appendStationAverageJson(String &js) {
  gnss_snapshot::Snapshot raw;
  const bool haveRaw = gnssCollector.sample(millis(), raw);
  js += F("\"station_average\":{\"window_ms\":30000,\"samples\":"); js += String(stationAverage.count());
  js += F(",\"raw_lat\":"); js += haveRaw && raw.fix ? String(raw.lat, 7) : F("null");
  js += F(",\"raw_lon\":"); js += haveRaw && raw.fix ? String(raw.lon, 7) : F("null");
  js += F(",\"mean_lat\":"); js += stationAverage.count() ? String(stationAverage.latitude(), 7) : F("null");
  js += F(",\"mean_lon\":"); js += stationAverage.count() ? String(stationAverage.longitude(), 7) : F("null");
  js += F(",\"rms_m\":"); js += stationAverage.count() ? String(stationAverage.rmsM(), 2) : F("null");
  js += F(",\"warning\":"); js += stationAverage.warning() ? F("true") : F("false");
  js += F(",\"warning_rms_m\":3}");
}

static String buildTrackJson() {
  bool linked = havePkt && ((millis() - lastRxMs) <= LINK_TIMEOUT_MS);
  uint32_t sinceRx = havePkt ? (uint32_t)((millis() - lastRxMs) / 1000) : UINT32_MAX;

  String js;
  js.reserve(900);
  js += F("{\"linked\":");
  js += linked ? F("true") : F("false");
  js += F(",\"data_fresh\":"); js += linked ? F("true") : F("false");
  const uint32_t rfAge = boundRfAgeMs();
  js += F(",\"rf_alive\":"); js += rfAge < 90000 ? F("true") : F("false");
  js += F(",\"rf_age_ms\":"); js += rfAge == UINT32_MAX ? F("null") : String(rfAge);
  js += F(",\"lora_fps_10s\":");
  js += String(loraDataRate.fps(millis()), 2);
  js += F(",\"bearing\":");
  if (haveBearingFix()) {
    js += String(computeBearing(stationLatitude(), stationLongitude(),
                                lastData.lat, lastData.lon), 1);
  } else {
    js += F("-1");
  }
  js += F(",\"client\":{\"lat\":");
  js += havePkt ? String(lastData.lat, 7) : F("0");
  js += F(",\"lon\":");
  js += havePkt ? String(lastData.lon, 7) : F("0");
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
  js += F(",\"source_age_ms\":"); js += F("null");
  js += F(",\"sample_age_ms\":"); js += F("null");
  js += F(",\"age_basis\":\"rx_elapsed_only\"");
  js += F(",\"last_rx_sec\":");
  js += (havePkt && sinceRx != UINT32_MAX) ? String(sinceRx) : F("-1");
  js += F("},\"station\":{\"lat\":");
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
  appendStationAverageJson(js); js += ',';
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
  if (stationAverage.warning()) {
    add(alerts::WARN, "station_scatter", "岸端 GPS 座標散布偏大",
        String("30 秒 RMS ") + String(stationAverage.rmsM(), 1) +
        " 公尺；追蹤使用移動平均，原始資料保留。這是散布，不是真實定位誤差。");
  }
  if (gpsRate.ready() && strcmp(gpsRate.state(), gnss_rate::kTargetIntervalMs == 1000 ? "observed_1hz" : "observed_2hz") != 0) {
    add(alerts::WARN, "station_gnss_rate", "岸端 GPS 實測頻率未達設定值",
        String("最近 5 秒：epoch ") + String(gpsRate.hz(), 2) + " / RMC " +
        String(gpsRate.rmcHz(), 2) + " / GGA " + String(gpsRate.ggaHz(), 2) + " Hz。請看除錯紀錄。");
  }
  int32_t sinceRx = havePkt ? (int32_t)((millis() - lastRxMs) / 1000) : -1;
  if (trackMode == TrackMode::Gps && !havePkt) {
    add(alerts::WARN, "client_never", boundRfAgeMs() < 90000 ? "已收到遙測，尚無定位封包" : "還沒收到追蹤器的訊號",
        boundRfAgeMs() < 90000 ? "RF 有收到綁定 Client 的封包，尚未收到 DATA；請看 Client GPS 狀態。" :
        "請確認追蹤器已經開機（長按電源鍵），而且攝影站已綁定它的 ID。");
  } else if (trackMode == TrackMode::Gps) {
    alerts::Level l = alerts::linkLevel(sinceRx);
    if (l == alerts::ERROR) {
      add(l, "client_link", boundRfAgeMs() < 90000 ? "追蹤器定位資料停止更新" : "和追蹤器失去連線",
          String("已經 ") + sinceRx + " 秒沒有收到 DATA，鏡頭已停止追蹤。" +
          (boundRfAgeMs() < 90000 ? "近期仍收到遙測；請檢查 Client GPS。" :
          "近期也沒有遙測；請檢查距離、電量或裝置是否在水面下。"));
    } else if (l == alerts::WARN) {
      add(l, "client_link", "追蹤器定位資料暫停更新",
          String("已經 ") + sinceRx + " 秒沒有收到 DATA，鏡頭暫時停在原地等定位資料回來。");
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
    if (alerts::humidityLevel(hum, stationHumBaselinePct) != alerts::NONE) {
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
      (stationGpsState() == SIG_BAD || stationGpsState() == SIG_MISS)) {
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
        "請到「資訊」分頁輸入鏡頭指南針角度並校正；方向不準時可重新校正。");
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

  // Station GPS quality
  js += F("{\"station_gps\":{\"fix\":");
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
  uint32_t rxTotal = rxDataCount + rxTelemetryCount + rxDiagnosticCount + rxGnssDiagnosticCount + rxDropCount;
  js += F(",\"rx_data\":");
  js += String(rxDataCount);
  js += F(",\"rx_telemetry\":");
  js += String(rxTelemetryCount);
  js += F(",\"rx_diagnostic\":"); js += String(rxDiagnosticCount);
  js += F(",\"rx_gnss_diagnostic\":"); js += String(rxGnssDiagnosticCount);
  js += F(",\"rx_drop\":");
  js += String(rxDropCount);
  js += F(",\"drop_rate\":");
  js += rxTotal ? String((float)rxDropCount / rxTotal, 3) : F("0");
  // Deprecated fields remain explicitly unavailable for older API consumers.
  js += F(",\"ack_enabled\":false,\"ack_tx\":null,\"ack_busy\":false,\"ack_errors\":null,\"ack_skipped\":null,\"ack_last_error\":null");
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
  js += F(",\"protocol_version\":"); js += String(PROTO_VERSION);
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

static void appendGnssReportJson(String &js, const gnss_diagnostics::Report &r) {
  js += F("{\"state\":\""); js += gnss_diagnostics::state(r); js += '"';
  js += F(",\"source_age_ms\":"); js += r.sourceAgeMs == UINT32_MAX ? F("null") : String(r.sourceAgeMs);
  js += F(",\"epoch_ms_of_day\":"); js += r.utcMs == UINT32_MAX ? F("null") : String(r.utcMs);
  js += F(",\"last_byte_age_ms\":"); js += r.byteAgeMs == 65535 ? F("null") : String(r.byteAgeMs);
  js += F(",\"last_sentence_age_ms\":"); js += r.sentenceAgeMs == 65535 ? F("null") : String(r.sentenceAgeMs);
  js += F(",\"last_advance_age_ms\":"); js += r.advanceAgeMs == 65535 ? F("null") : String(r.advanceAgeMs);
  js += F(",\"raw_fix\":"); js += (r.flags & 1) ? ((r.flags & 2) ? F("true") : F("false")) : F("null");
  js += F(",\"recovering\":"); js += (r.flags & 16) ? F("true") : F("false");
  js += F(",\"satellites\":"); js += r.satellites == 255 ? F("null") : String(r.satellites);
  js += F(",\"status_bits\":"); js += String(r.flags);
  js += F(",\"epochs\":"); js += String(r.epochs);
  js += F(",\"time_resyncs\":"); js += String(r.resyncs);
  js += F(",\"missing_or_invalid_time\":"); js += String(r.missingTime);
  js += F(",\"backwards_epochs\":"); js += String(r.backwards);
  js += F(",\"duplicate_epochs\":"); js += String(r.duplicates);
  js += F(",\"rejected_sentences\":"); js += String(r.rejected);
  js += F(",\"checksum_errors\":"); js += String(r.checksum);
  js += F(",\"age16_saturation_ms\":65534,\"counter_max\":65535}");
}

static String buildDebugJson(const packet_diagnostics::Selection &selection) {
  String js; js.reserve(5600);
  const uint32_t now = millis();
  const auto &g = gnssCollector.stats();
  gnss_snapshot::Snapshot sample;
  const bool sampled = gnssCollector.sample(now, sample);
  js = F("{\"schema_version\":3,\"firmware_version\":\"" SHORE_SPOTTER_VERSION
         "\",\"build\":\"" __DATE__ " " __TIME__ "\",\"protocol_version\":");
  js += String(PROTO_VERSION); js += F(",\"boot_id\":");
  js += String(controlBootId); js += F(",\"clock_ms\":"); js += String(now);
  js += F(",\"config\":{\"rf_frequency_mhz\":"); js += String(RF_FREQUENCY, 3);
  js += F(",\"bw_khz\":"); js += String(RF_BW, 1);
  js += F(",\"sf\":"); js += String(RF_SF); js += F(",\"cr\":"); js += String(RF_CR);
  js += F(",\"data_bytes\":"); js += String(DATA_PACKET_LEN);
  js += F(",\"ack_enabled\":false,\"ack_bytes\":0,\"telemetry_bytes\":"); js += String(TELEMETRY_PACKET_LEN);
  js += F(",\"diagnostic_bytes\":"); js += String(DIAGNOSTIC_PACKET_LEN);
  js += F(",\"gnss_diagnostic_bytes\":"); js += String(GNSS_DIAGNOSTIC_PACKET_LEN);
  js += F(",\"gnss_diagnostic_pages\":1");
  js += F(",\"send_interval_ms\":"); js += String(SEND_INTERVAL_MS);
  js += F(",\"send_mode\":\"latest_valid_fix\",\"status_heartbeat_ms\":0,\"rx_boosted_gain\":true");
  js += F(",\"diagnostic_interval_ms\":"); js += String(TELEMETRY_INTERVAL_MS);
  js += F(",\"ack_every_n\":0");
  js += F(",\"gnss_baud\":"); js += String(GPS_BAUD);
  js += F(",\"bound_client_id\":"); js += String(gpsClientId);
  js += F(",\"gnss_age_uncertainty_ms\":"); js += String(GPS_BACKLOG_GUARD_MS);
  js += F(",\"data_airtime_ms\":"); js += String(dataAirtimeMs);
  js += F(",\"ack_airtime_ms\":0");
  js += F(",\"telemetry_airtime_ms\":"); js += String(telemetryAirtimeMs);
  js += F(",\"diagnostic_airtime_ms\":"); js += String(diagnosticAirtimeMs);
  js += F(",\"gnss_diagnostic_airtime_ms\":"); js += String(gnssDiagnosticAirtimeMs);
  js += F("},"); appendStationAverageJson(js);
  js += F(",\"gps\":{\"age_basis\":\"nmea_epoch_aligned_arrival\",\"measurement_clock_synchronized\":false");
  js += F(",\"requested_hz\":"); js += String(1000 / gnss_rate::kTargetIntervalMs);
  js += F(",\"rate_state\":\""); js += gpsRate.state(); js += '"';
  js += F(",\"observed_hz\":"); js += gpsRate.ready() ? String(gpsRate.hz(), 2) : F("null");
  js += F(",\"rmc_hz\":"); js += gpsRate.ready() ? String(gpsRate.rmcHz(), 2) : F("null");
  js += F(",\"gga_hz\":"); js += gpsRate.ready() ? String(gpsRate.ggaHz(), 2) : F("null");
  js += F(",\"scope\":\"station_local\",\"last_epoch_interval_ms\":"); js += String(g.lastEpochIntervalMs);
  js += F(",\"source_age_ms\":"); js += sampled ? String(sample.sourceAgeMs) : F("null");
  js += F(",\"arrival_age_ms\":"); js += sampled ? String(sample.arrivalAgeMs) : F("null");
  js += F(",\"freshness_basis\":\"new_epoch_arrival\"");
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
  js += F(",\"stream\":");
  appendGnssReportJson(js, gnss_diagnostics::capture(gnssCollector, now));
  js += F("},\"client_gnss\":{\"received\":"); js += clientGnssDiagnostic.received() ? F("true") : F("false");
  js += F(",\"fresh\":"); js += clientGnssDiagnostic.received() && clientGnssDiagnostic.rxAgeMs(now) < gnss_diagnostics::kFreshMs ? F("true") : F("false");
  js += F(",\"rx_age_ms\":"); js += clientGnssDiagnostic.received() ? String(clientGnssDiagnostic.rxAgeMs(now)) : F("null");
  js += F(",\"pages_mask\":"); js += F("null");
  js += F(",\"snapshot\":");
  if (clientGnssDiagnostic.received()) appendGnssReportJson(js, clientGnssDiagnostic.report());
  else js += F("null");
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
  js += F(",\"rx_gnss_diagnostic\":"); js += String(rxGnssDiagnosticCount);
  js += F(",\"radio_errors\":"); js += String(rxErrorCount);
  js += F(",\"rejected_length\":"); js += String(rejectedLength);
  js += F(",\"rejected_format\":"); js += String(rejectedFormat);
  js += F(",\"rejected_binding\":"); js += String(rejectedBinding);
  js += F(",\"rejected_sequence\":"); js += String(rejectedGpsSequence);
  js += F(",\"sequence_gaps\":"); js += String(sequenceMissing);
  js += F(",\"sequence_resyncs\":"); js += String(sequenceResyncs);
  js += F(",\"invalid_fix_packets\":"); js += String(invalidFixPackets);
  js += F(",\"invalid_velocity_packets\":"); js += String(invalidVelocityPackets);
  js += F(",\"ack_sent\":null,\"ack_errors\":null,\"ack_skipped\":null");
  js += F(",\"radio_recoveries\":"); js += String(radioRecoverCount);
  js += F(",\"last_data_interval_ms\":"); js += String(lastDataIntervalMs);
  js += F(",\"max_data_interval_ms\":"); js += String(maxDataIntervalMs);
  js += F(",\"inferred_source_updates\":"); js += F("null");
  js += F(",\"inferred_source_interval_ms\":"); js += F("null");
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

static void serviceAxiomLog() {
  const uint32_t now = millis();
  const bool busy = stationRadioIrq || loopDuration.lastUs > 20000;
  const bool cloudDue = axiom_log::captureDue(now, busy);
  const bool sdDue = sd_log::captureDue(now, busy);
  if (!cloudDue && !sdDue) return;
  const uint32_t started = micros();
  axiom_log::Sample s;
  strcpy(s.build, __DATE__ " " __TIME__);
  s.bootId = controlBootId; s.ms = now; s.nodeId = nodeId; s.clientId = gpsClientId;
  s.heapFree = ESP.getFreeHeap(); s.heapMin = ESP.getMinFreeHeap();
  s.heapLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s.resetReason = esp_reset_reason(); s.wifiRssi = WiFi.RSSI();
  s.batteryMv = cachedBatteryMv; s.clientBatteryMv = haveTelemetry ? lastTelemetry.batteryMv : 0;
  gnssCollector.sample(now, s.gps); s.gpsFresh = gpsFixFresh();
  s.gnssCounters = gnssCollector.stats(); s.gpsBacklogDrops = gpsBacklogDrops;
  s.stationGnss = gnss_diagnostics::capture(gnssCollector, now);
  if (clientGnssDiagnostic.received()) {
    s.clientGnss = clientGnssDiagnostic.report(); s.clientGnssAge = clientGnssDiagnostic.rxAgeMs(now);
  }
  if (haveClientDiagnostic) { s.clientDiag = lastClientDiagnostic; s.clientDiagAge = now - lastClientDiagnosticMs; }
  s.clientPresent = havePkt;
  if (havePkt) {
    s.clientLat = lastData.lat; s.clientLon = lastData.lon;
    s.clientRxAge = now - lastRxMs;
    s.clientSourceAge = UINT32_MAX;
    s.clientSeq = lastData.seq; s.clientFix = lastData.fix;
    s.clientSpeedCmS = lastData.speedCmS; s.clientCourseDeg10 = lastData.courseDeg10;
    s.clientSatClass = lastData.satelliteClass; s.clientHdop10 = lastData.hdop10;
  }
  s.rssi = lastRssi; s.snr = lastSnr;
  s.angle = servoAngleDeg; s.target = servoTargetDeg; s.speed = servoMotion.speed(); s.velocity = servoMotion.velocity();
  snprintf(s.mode, sizeof(s.mode), "%s", trackModeStr(trackMode));
  snprintf(s.source, sizeof(s.source), "%s", tracking_policy::sourceName(controlSource));
  snprintf(s.uartState, sizeof(s.uartState), "%s", uartServoMode.stateName());
  s.pwmOk = servoPwmReady; s.motionFault = servoMotion.faulted(); s.calibrated = mountCalibrated;
  s.declinationReady = declinationReady; s.declination = declinationDeg; s.mountOffset = mountOffsetDeg;
  s.prediction = gpsPredictionEnabled; s.predictionActive = gpsPredictionEnabled && trackMode == TrackMode::Gps && gpsTrackingUsable() && gpsPredictionAllowed(); s.gpsUsable = gpsTrackingUsable();
  s.finishingGpsTarget = gpsFinishingTarget;
  s.stationSamples = stationAverage.count(); s.stationLat = stationAverage.latitude();
  s.stationLon = stationAverage.longitude(); s.stationRmsM = stationAverage.rmsM();
  s.stationWarning = stationAverage.warning();
  if (gpsRate.ready()) { s.gpsHz = gpsRate.hz(); s.gpsRmcHz = gpsRate.rmcHz(); s.gpsGgaHz = gpsRate.ggaHz(); }
  s.uartLate = uartServoMode.latePolls(); s.uartDiscarded = uartServoMode.discardedBytes();
  s.uartRejected = uartServoMode.rejectedCommands(); s.motionRejected = rejectedMotionCommands;
  s.controlGapMax = controlGap.maxMs; s.controlGapOver250 = controlGap.over250ms;
  const loop_metrics::Duration durations[] = {loopDuration, httpDuration, loraDuration, motionDuration,
    envDuration, pmuDuration, oledDuration, otaDuration};
  for (size_t i = 0; i < 8; ++i) s.durations[i] = durations[i];
  s.httpSlowest = httpServer.timing().slowest;
  s.httpRequests = httpServer.timing().requests; s.httpSlowRequests = httpServer.timing().slowRequests;
  const uint32_t counters[] = {rxDataCount, rxTelemetryCount, rxDiagnosticCount, rxGnssDiagnosticCount,
    rxErrorCount, rejectedLength, rejectedFormat, rejectedBinding, rejectedGpsSequence, sequenceMissing,
    sequenceResyncs, invalidFixPackets, invalidVelocityPackets, 0, 0, 0, // reserved legacy ACK counters
    radioRecoverCount, lastDataIntervalMs, maxDataIntervalMs};
  for (size_t i = 0; i < 19; ++i) s.counters[i] = counters[i];
  // SD packet capture has its own direct queue, independent of the 64-event ring.
  if (sdDue) sd_log::submit(s, started);
  if (!cloudDue) return;
  // Independent cursor: web debug reads neither consume nor duplicate cloud events.
  static bool haveCursor = false;
  static uint32_t cursor = 0, lost = 0;
  const auto window = packetEvents.select(haveCursor, true, cursor);
  if (window.dropped) lost += packetEvents.total() - cursor - packetEvents.size();
  s.eventTotal = packetEvents.total(); s.eventLost = lost; s.eventCount = window.count;
  for (size_t i = 0; i < window.count; ++i) s.events[i] = packetEvents.at(window.start + i);
  cursor = window.nextId; haveCursor = true;
  axiom_log::submit(s, started);
}

static void handleAxiomSettings() {
  // Credentials are accepted only inside a JSON POST body, never URL parameters.
  httpServer.sendHeader("Cache-Control", "no-store");
  const String body = httpServer.arg("plain");
  if (body.length() == 0 || body.length() > 768 || httpServer.args() != 1 ||
      httpServer.header("Content-Type") != "application/json" ||
      httpServer.header("Content-Length") != String(body.length())) {
    httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"JSON body required (max 768 bytes); no query parameters\"}"); return;
  }
  cJSON *json = cJSON_ParseWithLengthOpts(body.c_str(), body.length() + 1, nullptr, true);
  const auto *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
  const auto *dataset = cJSON_GetObjectItemCaseSensitive(json, "dataset");
  const auto *region = cJSON_GetObjectItemCaseSensitive(json, "region");
  const auto *token = cJSON_GetObjectItemCaseSensitive(json, "token");
  const auto *clear = cJSON_GetObjectItemCaseSensitive(json, "clear_token");
  axiom_log::Config config;
  bool valid = cJSON_IsObject(json) && cJSON_IsBool(enabled) && cJSON_IsString(dataset) &&
      cJSON_IsString(region) && (!token || cJSON_IsString(token)) && (!clear || cJSON_IsBool(clear));
  if (valid && !axiom_log::getConfig(config)) {
    cJSON_Delete(json);
    httpServer.send(409, "application/json", "{\"ok\":false,\"error\":\"Settings are still being saved\"}"); return;
  }
  if (valid) {
    valid = strlen(dataset->valuestring) < sizeof(config.dataset) &&
        (!strcmp(region->valuestring, "us") || !strcmp(region->valuestring, "eu")) &&
        (!token || strlen(token->valuestring) < sizeof(config.token));
    if (valid) {
      config.enabled = cJSON_IsTrue(enabled); config.region = !strcmp(region->valuestring, "eu");
      strcpy(config.dataset, dataset->valuestring);
      if (token && token->valuestring[0]) strcpy(config.token, token->valuestring);
      if (cJSON_IsTrue(clear)) config.token[0] = 0;
      valid = axiom_log::validConfig(config);
    }
  }
  cJSON_Delete(json);
  if (!valid) {
    httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid Axiom settings; enabled upload requires a dataset and API token\"}"); return;
  }
  if (!axiom_log::configure(config)) {
    httpServer.send(503, "application/json", "{\"ok\":false,\"error\":\"Uploader busy or insufficient memory\"}"); return;
  }
  httpServer.send(202, "application/json", axiom_log::statusJson());
}

static void initWebServer() {
  const char *axiomHeaders[] = {"Content-Type", "Content-Length"};
  httpServer.collectHeaders(axiomHeaders, 2);
  httpServer.on("/api/axiom", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.send(200, "application/json", axiom_log::statusJson());
  });
  httpServer.on("/api/axiom", HTTP_POST, handleAxiomSettings);
  // send() 的 const char* 多載會先 `String passStr = content` 把整份 44 KB 複製到
  // heap（arduino-esp32 WebServer.cpp 裡自己的 log_e 就寫著 "Use send_P for long
  // arrays"）。send_P 分塊送出，不做這份複製。
  httpServer.on("/", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.send_P(200, PSTR("text/html; charset=utf-8"), WEB_UI_HTML);
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
  // Record a replaceable RAM reference only at the settled 90-degree command.
  httpServer.on("/api/track/calibrate", HTTP_POST, []() {
    if (!acceptMotionRequest()) return;
    float bearing = 0.0f;
    if (!parseAngleArgument("bearing", 0.0f, 360.0f, bearing) || bearing >= 360.0f) {
      httpServer.send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bearing must be 0 <= degrees < 360\"}");
      return;
    }
    if (!compassCalibrationReady()) {
      httpServer.send(409, "application/json",
                      "{\"ok\":false,\"error\":\"請先回到 90° 並等鏡頭停穩，再按校正\"}");
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
  // One fresh command cancels tracking and requests centre through the common
  // limiter. Do not write PWM here or restore the previous automatic mode.
  httpServer.on("/api/servo/center", HTTP_POST, []() {
    if (!acceptMotionRequest()) return;
    if (!servoPwmReady || servoMotion.faulted()) {
      httpServer.send(503, "application/json",
                      "{\"ok\":false,\"error\":\"servo PWM unavailable or motion fault\"}");
      return;
    }
    enterManual();
    servoMotion.target(90.0);
    servoTargetDeg = static_cast<float>(servoMotion.requested());
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

  ArduinoOTA.setHostname("shore-spotter-station");
  ArduinoOTA
      .onStart([]() {
        sd_log::stop(); // asynchronous drain; user can restart recording after an OTA error
        axiom_log::pauseForOta(true);
        enterManual();
        trackMode = TrackMode::Paused;
        Log.println(F("[OTA] update started; Servo control paused"));
      })
      .onEnd([]() { Log.println(F("[OTA] update complete; rebooting")); })
      .onError([](ota_error_t error) {
        axiom_log::pauseForOta(false);
        Log.print(F("[OTA] ERROR code="));
        Log.println((unsigned int)error);
      });
  ArduinoOTA.begin();
  otaReady = true;
  Log.print(F("[OTA] ready: shore-spotter-station.local / "));
  Log.println(WiFi.localIP());
}
#endif

// ---------------------------------------------------------------------------
// Shutdown: show message on OLED then power off via PMU.
// For CLIENT role the OLED bus is normally off; we power it briefly here.
// ---------------------------------------------------------------------------
static void recordPowerEvent(const char *event, const char *reason) {
  const uint32_t now = millis();
  uint32_t boot = powerBootId;
#if defined(ROLE_STATION)
  if (controlBootId) boot = controlBootId;
#endif
  char line[464];
  snprintf(line, sizeof(line),
      "{\"event\":\"%s\",\"reason\":\"%s\",\"boot_id\":%lu,\"ms\":%lu,\"battery_mv\":%u,"
      "\"irq_ms\":%lu,\"irq_raw\":[%d,%d,%d],\"irq_clear\":[%d,%d,%d],\"read_fail\":%u,\"clear_fail\":%u,"
      "\"suppressed\":%u,\"read_errors\":%lu,\"clear_errors\":%lu,\"suppressed_keys\":%lu,"
      "\"vbus_raw\":[%d,%d],\"vbus_read_errors\":%lu}",
      event, reason, (unsigned long)boot, (unsigned long)now, cachedBatteryMv,
      (unsigned long)powerIrqMs, powerIrqSample.raw[0], powerIrqSample.raw[1], powerIrqSample.raw[2],
      powerIrqSample.clear[0], powerIrqSample.clear[1], powerIrqSample.clear[2],
      powerIrqSample.readFailed, powerIrqSample.clearFailed, powerIrqSample.suppressed,
      (unsigned long)powerIrqState.readErrors, (unsigned long)powerIrqState.clearErrors,
      (unsigned long)powerIrqState.suppressedKeys, powerVbusRaw[0], powerVbusRaw[1],
      (unsigned long)powerVbusReadErrors);
#if defined(FIELD_DIAGNOSTIC)
  // Queue before OLED wake / drain / delays. This remains a bounded asynchronous
  // submission: full Flash or a lost supply can still prevent persistence.
  diagnostic_store::submit(2, line, strlen(line), now);
  if (!strcmp(event, "shutdown") && diagnosticRawLength) {
#if defined(CLIENT_TRIP_LOG)
    recordTripRaw(diagnosticRawKind, diagnosticRaw, diagnosticRawLength, now);
#else
    diagnostic_store::submit(diagnosticRawKind, diagnosticRaw, diagnosticRawLength, now);
#endif
    diagnosticRawLength = 0;
  }
#endif
  Log.println(line); // USB plus SD text when recording is active
}

static void recordPowerIrq() {
  static uint8_t lastReadFailure = 0, lastClearFailure = 0;
  static uint32_t nextErrorLogMs = 0;
  const bool failure = powerIrqSample.readFailed || powerIrqSample.clearFailed;
  const bool changed = powerIrqSample.readFailed != lastReadFailure ||
      powerIrqSample.clearFailed != lastClearFailure;
  const bool key = powerIrqSample.shortPress || powerIrqSample.longPress;
  if (key || changed || (failure && loop_metrics::due(millis(), nextErrorLogMs))) {
    recordPowerEvent("pmu_irq", key ? "key" : (failure ? "bus_error" : "bus_recovered"));
    nextErrorLogMs = millis() + 10000;
  }
  lastReadFailure = powerIrqSample.readFailed;
  lastClearFailure = powerIrqSample.clearFailed;
}

static void showShutdownAndPowerOff() {
  recordPowerEvent("shutdown", "pwr_long_press");
#if defined(ROLE_STATION)
  enterManual();
#endif
  sd_log::stop();
  const uint32_t stopStarted = millis();
  while (!sd_log::stopped() && millis() - stopStarted < 2000) delay(10);
  if (!sd_log::stopped()) Log.println(F("[SD] shutdown drain timed out; unsynced tail may be lost"));
#if defined(ROLE_CLIENT)
  ClientRailGuard guard(true); // shutdown only; SD drain above has completed
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
#if defined(ROLE_CLIENT)
  ClientRailGuard guard;
  if (!guard.held) return false;
#endif
  if (!pmuOnline) return false;
  uint16_t mv = cachedBatteryMv;
  if (mv < BATT_PRESENT_MIN_MV) return false;  // no / implausible battery reading
  powerVbusRaw[0] = pmu.readRegister(XPOWERS_AXP2101_STATUS1);
  powerVbusRaw[1] = pmu.readRegister(XPOWERS_AXP2101_STATUS2);
  static uint8_t lastFailure = 0;
  static uint32_t nextErrorLogMs = 0;
  const uint8_t failure = (powerVbusRaw[0] < 0 || powerVbusRaw[0] > 255 ? 1 : 0) |
      (powerVbusRaw[1] < 0 || powerVbusRaw[1] > 255 ? 2 : 0);
  if (failure) {
    ++powerVbusReadErrors;
    if (failure != lastFailure || loop_metrics::due(millis(), nextErrorLogMs)) {
      recordPowerEvent("pmu_vbus", "read_error");
      nextErrorLogMs = millis() + 10000;
    }
    lastFailure = failure;
    return false; // unknown external power is not evidence of a battery-only low state
  }
  if (lastFailure) recordPowerEvent("pmu_vbus", "bus_recovered");
  lastFailure = 0;
  if (power_irq::vbusPresent(powerVbusRaw[0], powerVbusRaw[1])) return false;
  return mv < BATT_SHUTDOWN_MV;
}

// Show a low-battery notice, then cut power. Used at boot and at runtime so a
// dead battery can neither keep running nor power the board back on.
static void showLowBatteryAndPowerOff() {
  recordPowerEvent("shutdown", "low_battery");
#if defined(ROLE_STATION)
  enterManual();
#endif
  sd_log::stop();
  const uint32_t sdStopStarted = millis();
  while (!sd_log::stopped() && millis() - sdStopStarted < 2000) delay(10);
#if defined(ROLE_CLIENT)
  ClientRailGuard guard(true); // shutdown only
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
#if defined(CLIENT_TRIP_LOG)
  snprintf(line3, sizeof(line3), "TRIP SD:%s", sd_log::stateName());
  snprintf(line4, sizeof(line4), "PowerLog:%s", diagnostic_store::stateName());
#elif defined(FIELD_DIAGNOSTIC)
  snprintf(line3, sizeof(line3), "DIAG P%u RF%u SD%u", diagnosticPlan.phase(), diagnosticPlan.rf(), diagnosticPlan.sd());
  snprintf(line4, sizeof(line4), "Flash:%s", diagnostic_store::stateName());
#endif
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
  ClientRailGuard guard; // skip this wake if a background SD rail change is active
  if (!guard.held) return false;
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

// Three seconds of animation, ten seconds of status, then panel sleep.
// GPS and key handling keep running throughout both display phases. The
// starting gesture is guarded; a fresh press after its cleared baseline works.
static void showClientBootScreen() {
  const uint32_t initStarted = millis();
  uint32_t started = initStarted;
  uint32_t nextDraw = initStarted;
  bool ready = false;
  while (millis() - (ready ? started : initStarted) < client_boot_animation::kDurationMs + 10000) {
    serviceGps();
#if defined(FIELD_DIAGNOSTIC)
    sd_log::serviceUsb(); // DIAG works before SD init; trip SD is already active
    serviceFieldDiagnostic();
#endif
    serviceClientPowerKey();
    // A USB-powered MCU may still return from the PMU shutdown request.
    // Leave the last shutdown frame alone instead of resuming the boot page.
    if (powerIrqSample.longPress) return;
    if (loop_metrics::due(millis(), nextDraw)) {
      if (!ready) {
        ready = enableClientOled();
        // Initialization is outside the visible animation. Reset this clock
        // only once, after success; failed retries retain the bounded window.
        if (ready) started = millis();
      }
      const uint32_t elapsed = millis() - started;
      const bool animation = elapsed < client_boot_animation::kDurationMs;
      nextDraw = millis() + (ready && animation ? client_boot_animation::kFrameMs : 1000);
      if (ready) {
        if (animation) {
          client_boot_animation::draw(display, elapsed);
          display.sendBuffer();
        } else {
          drawClientInfoScreen();
        }
      }
    }
    delay(1);
  }
  // A short press near the end owns the normal ten-second wake deadline.
  // Hand that visible screen to loop() instead of sleeping it prematurely.
  if (ready && (!clientOledAwake || loop_metrics::due(millis(), clientOledOffMs))) {
    sleepClientOled();
    clientOledAwake = false;
  }
  serviceGps();
#if defined(FIELD_DIAGNOSTIC)
  sd_log::serviceUsb();
  serviceFieldDiagnostic();
#endif
  // Poll once at the display boundary; do not reset an already armed guard.
  nextPmuKeyMs = millis();
  serviceClientPowerKey();
}

// Short-press PWR wakes the screen for CLIENT_SCREEN_WAKE_MS (non-blocking).
// loop() 負責重繪，到期時讓面板回去睡覺（見 sleepClientOled）。
static void wakeClientScreen() {
  if (!enableClientOled()) return;
  clientOledAwake = true;
  clientOledOffMs = millis() + CLIENT_SCREEN_WAKE_MS;
  nextClientOledRefreshMs = millis();  // force an immediate redraw
}

static void serviceClientPowerKey() {
  if (!pmuOnline || !loop_metrics::due(millis(), nextPmuKeyMs)) return;
  power_irq::ClientBootGuard::Change change = power_irq::ClientBootGuard::None;
  {
    ClientRailGuard guard;
    if (!guard.held) return;
    nextPmuKeyMs = millis() + PMU_KEY_POLL_MS;
    powerIrqMs = millis();
    powerIrqSample = power_irq::read(pmu, powerIrqState, power_irq::kAllKeys);
    // Preserve the original startup latch BEFORE acknowledging it. Repeated
    // clear failures are rate-limited; a changed latch is always recorded.
    static int lastStartupRaw[3] = {-1, -1, -1};
    static uint32_t nextStartupLogMs = 0;
    const bool key = !powerIrqSample.readFailed &&
        (powerIrqSample.raw[1] & power_irq::kAllKeys);
    const bool changed = memcmp(lastStartupRaw, powerIrqSample.raw, sizeof(lastStartupRaw)) != 0;
    if (!clientPowerKeys.armed && key &&
        (changed || loop_metrics::due(millis(), nextStartupLogMs))) {
      recordPowerEvent("pmu_startup", "captured_before_clear");
      nextStartupLogMs = millis() + 10000;
    }
    memcpy(lastStartupRaw, powerIrqSample.raw, sizeof(lastStartupRaw));
    power_irq::clear(pmu, powerIrqState, powerIrqSample);
    change = clientPowerKeys.filter(powerIrqSample);
  }
  if (change == power_irq::ClientBootGuard::BaselineReady)
    recordPowerEvent("pmu_startup", "wait_new_press");
  else if (change == power_irq::ClientBootGuard::Armed)
    recordPowerEvent("pmu_startup", "new_press_armed");
  recordPowerIrq();
  // The baseline latch is never actionable. A fresh later press is accepted
  // during both the boot display and runtime, with the same clear recovery.
  if (powerIrqSample.longPress) showShutdownAndPowerOff();
  else if (powerIrqSample.shortPress) wakeClientScreen();
}
#endif

#if defined(ROLE_STATION)
// Parse uplink only; the main loop restarts RX after every frame.
static void acceptRadioPacket(const uint8_t *buf, size_t n, uint32_t receivedAtMs) {
  using packet_diagnostics::Kind;
  PacketHeader hdr{};
  auto reject = [&](Kind kind) {
    ++rxDropCount; ++rxWinDrop;
    recordPacketEvent(kind, receivedAtMs, buf, n);
    return;
  };
  if (n != DATA_PACKET_LEN &&
      n != TELEMETRY_PACKET_LEN && n != DIAGNOSTIC_PACKET_LEN && n != GNSS_DIAGNOSTIC_PACKET_LEN) {
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
    return;
  }
  if (hdr.msgType == MSG_DIAGNOSTIC) {
    DiagnosticPayload diag{};
    if (!protocol::decodeDiagnostic(buf, n, hdr, diag)) { ++rejectedFormat; return reject(Kind::Format); }
    lastClientDiagnostic = diag; haveClientDiagnostic = true;
    lastClientDiagnosticMs = receivedAtMs; ++rxDiagnosticCount;
    recordPacketEvent(Kind::Diagnostic, receivedAtMs, buf, n);
    return;
  }
  if (hdr.msgType == MSG_GNSS_DIAGNOSTIC) {
    gnss_diagnostics::Report report{};
    if (!gnss_diagnostics::decode(buf, n, hdr, report)) {
      ++rejectedFormat; return reject(Kind::Format);
    }
    clientGnssDiagnostic.accept(hdr, report, receivedAtMs);
    ++rxGnssDiagnosticCount;
    recordPacketEvent(Kind::GnssDiagnostic, receivedAtMs, buf, n);
    return;
  }
  DecodedData data{};
  if (!parseDataPacket(buf, n, data)) { ++rejectedFormat; return reject(Kind::Format); }
  if (!gpsSequence.accept(data.seq, receivedAtMs, data.fix)) {
    ++rejectedGpsSequence;
    recordPacketEvent(Kind::Sequence, receivedAtMs, buf, n, &data);
    return;
  }
  if (havePkt) {
    lastDataIntervalMs = receivedAtMs - lastRxMs;
    if (lastDataIntervalMs > maxDataIntervalMs) maxDataIntervalMs = lastDataIntervalMs;
    const uint16_t delta = data.seq - lastData.seq;
    if (!gpsSequence.resetAfterGap() && delta > 0 && delta < 0x8000) {
      sequenceMissing += delta - 1; rxWinMissing += delta - 1;
    } else {
      ++sequenceResyncs;
      Log.print(F("[LoRa] DATA sequence baseline reset after gap_ms="));
      Log.print(lastDataIntervalMs); Log.print(F(" seq=")); Log.println(data.seq);
    }
  }
  lastData = data; lastRxMs = receivedAtMs; havePkt = true; ++rxDataCount;
  loraDataRate.record(receivedAtMs);
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
  return;  // reception remains owned by the main loop; no downlink
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
  powerBootId = esp_random();
  if (!powerBootId) powerBootId = 1;
  nodeId = derivedNodeId();
  Log.println(F("[BOOT] Shore Spotter v" SHORE_SPOTTER_VERSION));

#if defined(FIELD_DIAGNOSTIC)
  diagnosticBootId = powerBootId;
  diagnostic_store::begin(diagnosticBootId);
  char bootRecord[320];
  snprintf(bootRecord, sizeof(bootRecord),
      "{\"event\":\"boot\",\"diagnostic\":true,\"firmware\":\"%s\",\"node_id\":%u,\"reset_reason\":%d,\"gnss_interval_ms\":1000,\"gnss_baud\":115200,\"plan_resumed\":false}",
      SHORE_SPOTTER_VERSION, nodeId, int(esp_reset_reason()));
  diagnostic_store::submit(2, bootRecord, strlen(bootRecord), millis());
#if defined(CLIENT_TRIP_LOG)
  const char tripRecord[] = "{\"event\":\"trip_profile\",\"profile\":\"client-trip-1hz\",\"raw_target\":\"sd\",\"sd_policy\":\"always\",\"flash_snapshot_ms\":60000,\"flash_sd_status_ms\":300000}";
  diagnostic_store::submit(2, tripRecord, sizeof(tripRecord)-1, millis());
#endif
#endif
  pmuOnline = initPmu();
#if defined(FIELD_DIAGNOSTIC)
  snprintf(bootRecord, sizeof(bootRecord),
      "{\"event\":\"pmu_boot\",\"online\":%s,\"power_on_source\":%d,\"power_off_source\":%d}",
      pmuOnline ? "true" : "false", pmuOnline ? int(pmu.getPowerOnSource()) : -1,
      pmuOnline ? int(pmu.getPowerOffSource()) : -1);
  diagnostic_store::submit(2, bootRecord, strlen(bootRecord), millis());
#endif
#if defined(ROLE_CLIENT) && defined(FIELD_DIAGNOSTIC)
  recordClientBatteryDiagnostic("init");
#endif
  if (!initRadio()) {
#if defined(FIELD_DIAGNOSTIC)
    const char failure[] = "{\"event\":\"radio_init_failed\"}";
    diagnostic_store::submit(2, failure, sizeof(failure)-1, millis());
#endif
    while (true) {
#if defined(FIELD_DIAGNOSTIC)
      sd_log::serviceUsb(); // DIAG export remains available even before SD init
      delay(1);
#else
      delay(1000);
#endif
    }
  }
  computeAirtimeBudget();

#if defined(ROLE_CLIENT)
  configureGps();

  if (pmuOnline) {
    pmu.setALDO1Voltage(3300);
    pmu.enableALDO1();
    // short-press = wake screen 10 s, long-press = shutdown
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ |
                  XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ | XPOWERS_AXP2101_PKEY_POSITIVE_IRQ);
    delay(100);
  }
  serviceClientPowerKey();
  cachedBatteryMv = readBatteryMilliVolts();
  if (batteryCriticallyLow()) showLowBatteryAndPowerOff();  // refuse to boot empty
  nextBatteryMs = millis() + BATTERY_UPDATE_MS;
  nextEnvMs = millis() + ENV_UPDATE_MS;
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);
  envSensorOnline = initEnvSensor();
  sampleEnvSensor();

#if defined(CLIENT_TRIP_LOG)
  // Start the background SD recorder before the 10-second boot display.
  // It remains active with no fix; the normal/phase-test policies stay below.
  const bool sdReady = pmuOnline && pmu.disableBLDO1() && !pmu.isEnableBLDO1();
  pinMode(36, INPUT); pinMode(35, INPUT); pinMode(47, INPUT);
  sd_log::begin(diagnosticBootId, sdReady, clientSdPower);
  sd_log::ClientRecord initialRecord;
  initialRecord.ms = millis(); initialRecord.nodeId = nodeId;
  sd_log::clientGps(true, initialRecord);
  recordDiagnosticPhase(millis());
  Log.println(F("[CLIENT] profile=client-trip-1hz raw=SD sd_policy=always flash_snapshot_ms=60000"));
#endif
  showClientBootScreen();  // animation 3 s + status 10 s, then panel sleep
#if !defined(CLIENT_TRIP_LOG)
  // No GPS => SD stays physically off. The worker powers it only for a usable
  // GPS session or an explicit USB read/list operation.
  const bool sdReady = pmuOnline && pmu.disableBLDO1() && !pmu.isEnableBLDO1();
  pinMode(36, INPUT); pinMode(35, INPUT); pinMode(47, INPUT);
#if defined(FIELD_DIAGNOSTIC)
  sd_log::begin(diagnosticBootId, sdReady, clientSdPower);
  sd_log::stop(); // baseline starts with the physical card off
  recordDiagnosticPhase(millis());
#else
  sd_log::begin(powerBootId, sdReady, clientSdPower);
#endif
#endif

  Log.println(F("[CLIENT] mode active: send position packets"));
  Log.print(F("[CLIENT] protocol=")); Log.println(PROTO_VERSION);
  Log.print(F("[CLIENT] node id (chip MAC last 2 bytes) = 0x"));
  Log.println(nodeId, HEX);
  Log.println(F("[CLIENT] Bind this id on STATION using /api/whitelist action=set."));
  Log.print(F("[CLIENT] GPS UART baud="));
  Log.println(GPS_BAUD);
  Log.print(F("[CLIENT] TX power="));
  Log.print(TX_POWER_DBM);
  Log.println(F(" dBm (fixed, ATPC=off)"));

  // Arm TX completion and start the cadence after the boot screen.
  radio.setDio1Action(onClientDio1);
  clientRadioReady = radio.standby() == RADIOLIB_ERR_NONE;
  clientCadence.reset(millis());
#endif

#if defined(ROLE_STATION)
  if (pmuOnline) {
    pmu.setALDO1Voltage(3300);
    pmu.enableALDO1();  // power the OLED / I2C bus-0 peripherals
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_LONG_IRQ);  // long-press = shutdown
    delay(100);
  }

  // Station also reads its own GPS so it can compute bearing to the client.
  configureGps();

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);
  envSensorOnline = initEnvSensor();
  sampleEnvSensor();
  nextEnvMs = millis() + ENV_UPDATE_MS;
  initServo();                     // restore boot centre at 90 degrees
  oledOnline = initStationDisplay();
  display.clearBuffer();
  display.setFont(u8g2_font_6x12_tr);
  display.drawStr(0, 12, "SHORE SPOTTER v" SHORE_SPOTTER_VERSION);
  display.drawStr(0, 30, "STATION booting...");
  if (oledOnline) display.sendBuffer();

  // A CPU/USB reset does not remove SD power. Start with a real card power cycle
  // so an interrupted write/format cannot leave the next boot's SPI card busy.
  const bool sdWasOff = pmuOnline && pmu.disableBLDO1() && !pmu.isEnableBLDO1();
  delay(100);
  const bool sdPowered = sdWasOff && pmu.setBLDO1Voltage(3300) && pmu.enableBLDO1() &&
      pmu.isEnableBLDO1() && pmu.getBLDO1Voltage() == 3300;
  delay(10);
  sd_log::begin(controlBootId, sdPowered);
  // Replay bounded early boot text once, then LogTee supplies new text directly.
  const size_t bootLogLength = logWrapped ? LOG_BUF_BYTES : logHead;
  const size_t bootLogStart = logWrapped ? logHead : 0;
  for (size_t i = 0; i < bootLogLength; i += 256) {
    uint8_t chunk[256];
    const size_t n = bootLogLength - i < sizeof(chunk) ? bootLogLength - i : sizeof(chunk);
    for (size_t j = 0; j < n; ++j) chunk[j] = logBuf[(bootLogStart + i + j) % LOG_BUF_BYTES];
    sd_log::text(chunk, n, millis());
  }
  Log.print(F("[SD] BLDO1 3300 mV readback=")); Log.println(sdPowered ? "ok" : "failed");

  cachedBatteryMv = readBatteryMilliVolts();
  if (batteryCriticallyLow()) showLowBatteryAndPowerOff();  // refuse to boot empty

  nextStationIdleLogMs = millis() + STATION_IDLE_LOG_MS;
  nextRxSummaryMs = millis() + RX_SUMMARY_MS;
  nextDisplayMs = millis() + DISPLAY_REFRESH_MS;
  Log.println(F("[STATION] mode active: receive position packets"));
  loadStationSettings();
  Log.print(F("[STATION] GPS client: "));
  if (gpsClientId != 0) Log.println(gpsClientId, HEX);
  else Log.println(F("unbound"));

  // Connect to the phone-provided hotspot in station mode.
  // Credentials come from include/wifi_config.h (WIFI_SSID / WIFI_PASSWORD).
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("shore-spotter-station");
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Log.print(F("[WiFi] Connecting to configured hotspot "));
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiStart < WIFI_CONNECT_TIMEOUT_MS) {
#if defined(FIELD_DIAGNOSTIC)
    serviceGps(); serviceFieldDiagnostic(); delay(1);
#else
    delay(250);
    Log.print('.');
#endif
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
  if (oledOnline) display.sendBuffer();
#if defined(FIELD_DIAGNOSTIC)
  const uint32_t screenStarted = millis();
  while (millis()-screenStarted < 2000) { serviceGps(); serviceFieldDiagnostic(); delay(1); }
#else
  delay(2000);
#endif

  axiom_log::begin(controlBootId);
  initWebServer();
  initArduinoOta();

  // Arm non-blocking, interrupt-driven reception.
  radio.setDio1Action(onLoRaDio1);
  stationRadioIrq = false;
  stationRxReady = radio.startReceive() == RADIOLIB_ERR_NONE;
  if (!stationRxReady) nextStationRxRetryMs = millis() + 100;
  enterManual();  // boot in Manual; LoRa reception and SD recording remain active
#endif
}

void loop() {
#if defined(FIELD_DIAGNOSTIC)
  serviceFieldDiagnostic();
#endif
#if defined(ROLE_CLIENT)
  clientLoopGap.observe(millis());
  sd_log::serviceUsb();
  serviceGps();

  serviceClientTransmit();

  if (loop_metrics::due(millis(), nextBatteryMs)) {
    nextBatteryMs = millis() + BATTERY_UPDATE_MS;
    cachedBatteryMv = readBatteryMilliVolts();
    Log.print(F("[CLIENT] Battery update mV="));
    Log.println(cachedBatteryMv);
    checkLowBatteryAndMaybeShutdown();
  }

#if defined(FIELD_DIAGNOSTIC)
  // One post-startup read, then five-minute evidence; keep long-trip Flash use
  // bounded. Early init ADC values alone need not represent settled voltage.
  static uint32_t nextBatteryDiagnosticMs = 0;
  if (loop_metrics::due(millis(), nextBatteryDiagnosticMs)) {
    recordClientBatteryDiagnostic("runtime");
    nextBatteryDiagnosticMs = millis() + 300000;
  }
#endif

  if (loop_metrics::due(millis(), nextEnvMs)) {
    nextEnvMs = millis() + ENV_UPDATE_MS;
    sampleEnvSensor();
  }

  serviceGps();
  serviceClientTransmit();
  static uint32_t nextClientSummaryMs = 0;
  if (loop_metrics::due(millis(), nextClientSummaryMs)) {
    nextClientSummaryMs = millis() + 10000;
    Log.print(F("[CLIENT] tx=")); Log.print(clientTxCount);
    Log.print(F(" errors=")); Log.print(clientTxErrors);
    Log.print(F(" skipped=")); Log.print(dataSkippedSlots);
    Log.print(F(" diagnostic_tx=")); Log.print(diagnosticTxCount);
    Log.print(F(" GNSS_epoch_ms=")); Log.print(gnssCollector.stats().lastEpochIntervalMs);
    Log.print(F(" backlog_drops=")); Log.println(gpsBacklogDrops);
    Log.print(F("[SD] ")); Log.println(sd_log::statusJson());
  }

  // PWR key: short-press wakes the screen 10 s, long-press shuts down.
  // Polled on a timer (see PMU_KEY_POLL_MS) — loop() no longer blocks, so every
  // pass would otherwise cost two I2C transactions on the PMU bus.
  serviceClientPowerKey();

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
  // (The station gets the same yield for free from WebServer::handleClient().)
  delay(1);
#endif

#if defined(ROLE_STATION)
  MeasureDuration loopTiming(loopDuration);
  sd_log::serviceUsb();
  serviceStationRadio();
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
  serviceStationRadio();
  serviceControl();
  serviceGps();  // keep the station's own GPS position fresh
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
    powerIrqMs = millis();
    powerIrqSample = power_irq::poll(pmu, powerIrqState);
    recordPowerIrq();
    if (powerIrqSample.longPress) showShutdownAndPowerOff();
  }

  serviceControl();
  // DIO1 drives Station RX completion; HTTP and I2C are measured separately.
  serviceStationRadio();
  if (stationRxReady && stationRadioIrq) {
    MeasureDuration timing(loraDuration);
    const uint32_t receivedAtMs = stationRadioIrqMs;
    stationRadioIrq = false;
    bool rxRestarted = false;
    uint8_t buf[255];
    // Preserve the real RF length. Never truncate a long frame into a valid one.
    const size_t n = radio.getPacketLength();
    const int state = radio.readData(buf, n <= sizeof(buf) ? n : sizeof(buf));
    // RadioLib returns CRC mismatch only after it has copied the received bytes.
    // Keep those bytes for diagnosis, but never parse them as valid control data.
    const bool rawAvailable = (state == RADIOLIB_ERR_NONE || state == RADIOLIB_ERR_CRC_MISMATCH) && n <= sizeof(buf);
    if (rawAvailable) { receivedPacketRssi = radio.getRSSI(); receivedPacketSnr = radio.getSNR(); }
    if (state == RADIOLIB_ERR_NONE && n <= sizeof(buf)) {
      radioFailStreak = 0;
      acceptRadioPacket(buf, n, receivedAtMs);
    } else {
      ++rxErrorCount; ++rxWinErr; rxWinLastErr = state;
      recordPacketEvent(packet_diagnostics::Kind::RadioError, receivedAtMs, rawAvailable ? buf : nullptr, n, nullptr, state);
      if (++radioFailStreak >= RADIO_RX_ERR_LIMIT && recoverRadio()) {
        rxRestarted = true; stationRxReady = true;
      }
    }
    if (!rxRestarted) {
      // Do not clear the software IRQ after restarting RX: preserve a new packet.
      stationRxReady = radio.startReceive() == RADIOLIB_ERR_NONE;
      if (!stationRxReady) nextStationRxRetryMs = millis() + 100;
    }
  }

  serviceStationRadio();

  serviceControl();

  if (loop_metrics::due(millis(), nextRxSummaryMs)) {
    nextRxSummaryMs = millis() + RX_SUMMARY_MS;
    logRxSummary();
  }

  // Advance even while the link is healthy, so this deadline never lies dormant
  // for longer than half the millis range before the next disconnection.
  if (loop_metrics::due(millis(), nextStationIdleLogMs)) {
    nextStationIdleLogMs = millis() + STATION_IDLE_LOG_MS;
    if (!havePkt || millis() - lastRxMs > 2500) {
      Log.print(F("[STATION] idle | mode="));
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
    renderStationDisplay();
  }
  serviceControl();
  serviceStationDisplay();
  serviceControl();
  serviceAxiomLog();
  static uint32_t nextSdStatusMs = 0;
  if (loop_metrics::due(millis(), nextSdStatusMs)) {
    nextSdStatusMs = millis() + 10000;
    Log.print(F("[SD] ")); Log.println(sd_log::statusJson());
  }
#endif
}
