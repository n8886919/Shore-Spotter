#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "gnss_diagnostics.h"
#include "http_timing.h"
#include "loop_metrics.h"
#include "packet_diagnostics.h"
#if defined(ARDUINO) && defined(ROLE_STATION)
#include <Arduino.h>
#endif

// Only value copies cross into the uploader task. No radio, GPS, PWM, String,
// web server, or Preferences instance is shared with the control loop.
namespace axiom_log {
constexpr uint32_t kSampleMs = 1000, kFlushMs = 5000, kMaxAgeMs = 15000;
constexpr size_t kQueueSamples = 8, kBatchSamples = 5, kBatchBytes = 32768;
constexpr uint32_t kMinUploadHeap = 100 * 1024, kMinCaptureHeap = 48 * 1024;
constexpr uint32_t kConfigMagic = 0x41584c31;
struct Config {
  uint32_t magic = kConfigMagic;
  uint8_t enabled = 0, region = 0; // 0 US, 1 EU; no arbitrary credential destination
  char dataset[64] = "shore-spotter";
  char token[256]{};
};
inline bool validDataset(const char *s) {
  const size_t n = strnlen(s, sizeof(Config::dataset));
  if (!n || n >= sizeof(Config::dataset)) return false;
  for (size_t i = 0; i < n; ++i)
    if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
          (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_')) return false;
  return true;
}
inline bool validToken(const char *s) {
  const size_t n = strnlen(s, sizeof(Config::token));
  if (n >= sizeof(Config::token)) return false;
  for (size_t i = 0; i < n; ++i) if (s[i] <= 32 || s[i] >= 127) return false;
  return true;
}
inline bool validConfig(const Config &c) {
  return c.magic == kConfigMagic && c.enabled <= 1 && c.region <= 1 &&
      validDataset(c.dataset) && validToken(c.token) && (!c.enabled || c.token[0]);
}
inline const char *host(uint8_t region) {
  return region == 1 ? "eu-central-1.aws.edge.axiom.co" : "us-east-1.aws.edge.axiom.co";
}
inline uint32_t backoffMs(uint32_t failures, uint32_t retryAfterSeconds = 0) {
  uint32_t delay = 5000;
  for (uint32_t i = 1; i < failures && delay < 300000; ++i) delay *= 2;
  if (delay > 300000) delay = 300000;
  const uint32_t retry = retryAfterSeconds > 86400 ? 86400000 : retryAfterSeconds * 1000;
  return retry > delay ? retry : delay;
}

struct Sample {
  uint32_t generation = 0, bootId = 0, id = 0, ms = 0;
  char build[24]{}; // main.cpp build identity, shared with /api/debug
  uint16_t nodeId = 0, clientId = 0;
  uint32_t heapFree = 0, heapMin = 0, heapLargest = 0, resetReason = 0;
  int32_t wifiRssi = 0;
  uint16_t batteryMv = 0, clientBatteryMv = 0;
  gnss_snapshot::Snapshot gps;
  gnss_snapshot::Counters gnssCounters;
  gnss_diagnostics::Report stationGnss, clientGnss;
  uint32_t gpsBacklogDrops = 0, clientGnssAge = UINT32_MAX, clientDiagAge = UINT32_MAX;
  DiagnosticPayload clientDiag{};
  bool gpsFresh = false, clientPresent = false;
  double clientLat = 0, clientLon = 0;
  uint32_t clientRxAge = UINT32_MAX, clientSourceAge = UINT32_MAX;
  uint16_t clientSeq = 0, clientSpeedCmS = UINT16_MAX, clientCourseDeg10 = 0;
  uint8_t clientFix = 0, clientSatClass = 0, clientHdop10 = 255;
  float rssi = 0, snr = 0, angle = 0, target = 0, speed = 0, velocity = 0;
  float mountOffset = 0, declination = 0;
  char mode[8]{}, source[8]{}, uartState[16]{};
  bool pwmOk = false, motionFault = false, calibrated = false, declinationReady = false;
  bool prediction = false, predictionActive = false, gpsUsable = false;
  bool finishingGpsTarget = false, stationWarning = false;
  uint16_t stationSamples = 0;
  double stationLat = 0, stationLon = 0;
  float stationRmsM = 0;
  float gpsHz = -1, gpsRmcHz = -1, gpsGgaHz = -1;
  uint32_t uartLate = 0, uartDiscarded = 0, uartRejected = 0, motionRejected = 0;
  uint32_t controlGapMax = 0, controlGapOver250 = 0;
  loop_metrics::Duration durations[8]; // loop, http, lora, motion, gps-independent I2C, OTA
  http_timing::Snapshot httpSlowest;
  uint32_t httpRequests = 0, httpSlowRequests = 0;
  uint32_t counters[19]{};
  uint32_t eventTotal = 0, eventLost = 0;
  uint8_t eventCount = 0;
  packet_diagnostics::Event events[8];
};
static_assert(sizeof(Sample) < 1800, "Keep loop snapshot copies bounded");

// Worker-only baseline: compare consecutive encoded observations, never mutate
// control state. Reset across boot, settings generation or client binding changes.
struct LogHistory {
  bool valid = false, gpsUsable = false, pwmOk = false, fault = false;
  uint32_t bootId = 0, generation = 0, ms = 0;
  uint16_t clientId = 0;
  uint8_t link = 0;
  char mode[8]{}, source[8]{};
  const char *gnss = "unknown";
  uint32_t radioErrors = 0, controlGaps = 0, checksumErrors = 0;
};

// One snapshot, plus at most one state-change record. Zero on overflow;
// history advances only after both records fit. No network or control effects.
// Cloud keeps schema 2 field paths to avoid consuming the existing dataset's
// field budget. Local SD exports use the default Station schema 3 names.
size_t encodeSample(char *out, size_t capacity, const Sample &sample,
                    int64_t unixMs, uint32_t droppedSamples, uint32_t captureMaxUs,
                    LogHistory *history = nullptr, size_t *records = nullptr,
                    bool cloudLegacyKeys = false);

#if defined(ARDUINO) && defined(ROLE_STATION)
void begin(uint32_t bootId = 0);
// Called only from loop / web handlers. Submission and sampling never wait on TLS.
bool getConfig(Config &out);
bool configure(const Config &config);
bool captureDue(uint32_t now, bool busy);
void submit(Sample &sample, uint32_t captureStartedUs);
void pauseForOta(bool paused);
String statusJson();
#endif
} // namespace axiom_log
