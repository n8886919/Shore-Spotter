#include "axiom_log.h"
#include "firmware_version.h"
#include "tracking_policy.h"
#include "gnss_rate.h"
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <time.h>

namespace axiom_log {
namespace {
// Bounded writer; all expensive formatting runs on the worker, never loop().
class Json {
 public:
  Json(char *out, size_t capacity) : out_(out), cap_(capacity) {}
  void raw(const char *format, ...) {
    if (!ok_) return;
    va_list args; va_start(args, format);
    const int n = vsnprintf(out_ + size_, cap_ > size_ ? cap_ - size_ : 0, format, args);
    va_end(args);
    if (n < 0 || static_cast<size_t>(n) >= cap_ - size_) { ok_ = false; return; }
    size_ += n;
  }
  void quoted(const char *s) {
    raw("\"");
    for (; *s; ++s) {
      const unsigned char c = *s;
      if (c == '"' || c == '\\') raw("\\%c", c);
      else if (c < 32) raw("\\u%04x", c);
      else raw("%c", c);
    }
    raw("\"");
  }
  void key(const char *name) {
    if (depth_) { if (!first_[depth_ - 1]) raw(","); first_[depth_ - 1] = false; }
    if (name) { quoted(name); raw(":"); }
  }
  void object(const char *name = nullptr) { key(name); raw("{"); first_[depth_++] = true; }
  void array(const char *name) { key(name); raw("["); first_[depth_++] = true; }
  void end(bool array = false) { --depth_; raw(array ? "]" : "}"); }
  void number(const char *name, int64_t value) { key(name); raw("%lld", static_cast<long long>(value)); }
  void optionalNumber(const char *name, int64_t value, bool known) {
    key(name); if (known) raw("%lld", static_cast<long long>(value)); else raw("null");
  }
  void real(const char *name, double value, int precision = 6) {
    key(name); if (std::isfinite(value)) raw("%.*f", precision, value); else raw("null");
  }
  void boolean(const char *name, bool value) { key(name); raw(value ? "true" : "false"); }
  void string(const char *name, const char *value) { key(name); quoted(value); }
  void age(const char *name, uint32_t value, uint32_t unknown = UINT32_MAX) {
    key(name); if (value == unknown) raw("null"); else raw("%lu", static_cast<unsigned long>(value));
  }
  size_t size() const { return ok_ ? size_ : 0; }
 private:
  char *out_; size_t cap_, size_ = 0; bool ok_ = true;
  unsigned depth_ = 0; bool first_[8]{};
};
void gnss(Json &j, const char *name, const gnss_diagnostics::Report &r, bool known = true) {
  j.object(name);
  j.string("state", known ? gnss_diagnostics::state(r) : "unknown");
  j.age("source_age_ms", r.sourceAgeMs); j.age("epoch_ms_of_day", r.utcMs);
  j.age("byte_age_ms", r.byteAgeMs, UINT16_MAX);
  j.age("sentence_age_ms", r.sentenceAgeMs, UINT16_MAX);
  j.age("advance_age_ms", r.advanceAgeMs, UINT16_MAX);
  j.age("satellites", r.satellites, 255); j.optionalNumber("flags", r.flags, known);
  j.optionalNumber("epochs", r.epochs, known); j.optionalNumber("resyncs", r.resyncs, known);
  j.optionalNumber("missing_time", r.missingTime, known); j.optionalNumber("backwards", r.backwards, known);
  j.optionalNumber("duplicates", r.duplicates, known); j.optionalNumber("rejected", r.rejected, known);
  j.optionalNumber("checksum", r.checksum, known); j.end();
}
const char *linkName(uint8_t link) {
  return link == 1 ? "receiving" : link == 2 ? "stale" : "not_received";
}
const char *linkText(uint8_t link) {
  return link == 1 ? "Client receiving" : link == 2 ? "Client stale" : "No Client data";
}
LogHistory observe(const Sample &s) {
  LogHistory h;
  h.valid = true; h.bootId = s.bootId; h.generation = s.generation; h.clientId = s.clientId; h.ms = s.ms;
  h.link = !s.clientPresent ? 0 : s.clientRxAge < tracking_policy::kGpsFreshMs ? 1 : 2;
  snprintf(h.mode, sizeof(h.mode), "%s", s.mode); snprintf(h.source, sizeof(h.source), "%s", s.source);
  h.gnss = gnss_diagnostics::state(s.stationGnss);
  h.gpsUsable = s.gpsUsable; h.pwmOk = s.pwmOk; h.fault = s.motionFault;
  h.radioErrors = s.counters[4];
  h.controlGaps = s.controlGapOver250; h.checksumErrors = s.gnssCounters.checksumErrors;
  return h;
}
struct Change { const char *field, *label, *from, *to; };
size_t changes(const LogHistory &a, const LogHistory &b, Change (&out)[7], bool cloudLegacyKeys) {
  size_t n = 0;
  auto add = [&](const char *field, const char *label, const char *from, const char *to) {
    if (strcmp(from, to)) out[n++] = {field, label, from, to};
  };
  add("client_link", "Client link", linkName(a.link), linkName(b.link));
  add(cloudLegacyKeys ? "server_gnss" : "station_gnss", "Station GPS", a.gnss, b.gnss);
  add("mode", "Mode", a.mode, b.mode);
  add("source", "Source", a.source, b.source);
  add("gps_usable", "GPS tracking", a.gpsUsable ? "ready" : "unavailable", b.gpsUsable ? "ready" : "unavailable");
  add("pwm_ok", "PWM", a.pwmOk ? "ok" : "fault", b.pwmOk ? "ok" : "fault");
  add("motion_fault", "Motion fault", a.fault ? "fault" : "ok", b.fault ? "fault" : "ok");
  return n;
}
}

size_t encodeSample(char *out, size_t capacity, const Sample &s, int64_t unixMs,
                    uint32_t droppedSamples, uint32_t captureMaxUs,
                    LogHistory *history, size_t *records, bool cloudLegacyKeys) {
  if (records) *records = 0;
  if (!capacity || s.eventCount > 8) return 0;
  const LogHistory current = observe(s);
  const LogHistory previous = history ? *history : LogHistory{};
  const bool comparable = previous.valid && previous.bootId == s.bootId &&
      previous.generation == s.generation && previous.clientId == s.clientId;
  const uint32_t radioDelta = comparable ? current.radioErrors - previous.radioErrors : 0;
  const uint32_t gapDelta = comparable ? current.controlGaps - previous.controlGaps : 0;
  const uint32_t checksumDelta = comparable ? current.checksumErrors - previous.checksumErrors : 0;
  const bool gpsMode = !strcmp(s.mode, "gps");
  const char *level = !s.pwmOk || s.motionFault ? "error" :
      current.link == 2 || (gpsMode && !s.gpsUsable) || radioDelta || gapDelta || checksumDelta ? "warn" : "info";
  char message[512];
  const int summaryLength = snprintf(message, sizeof(message), "%s / %s | %s | Station GPS %s | Commanded %.1f deg%s%s",
      s.mode, !strcmp(s.mode, "uart") && !strcmp(s.source, "hold") ? "waiting" : s.source,
      linkText(current.link), current.gnss, s.angle, s.pwmOk ? "" : " | PWM unavailable",
      s.motionFault ? " | Motion fault" : "");
  if (summaryLength < 0 || size_t(summaryLength) >= sizeof(message)) return 0;
  if (radioDelta || gapDelta || checksumDelta) {
    const int n = snprintf(message + summaryLength, sizeof(message) - summaryLength,
        " | New errors: RF %lu / control gap %lu / GPS checksum %lu",
        static_cast<unsigned long>(radioDelta),
        static_cast<unsigned long>(gapDelta), static_cast<unsigned long>(checksumDelta));
    if (n < 0 || size_t(n) >= sizeof(message) - summaryLength) return 0;
  }
  Change changed[7];
  const size_t changeCount = comparable ? changes(previous, current, changed, cloudLegacyKeys) : 0;
  const uint8_t schemaVersion = cloudLegacyKeys ? 2 : 3;
  Json j(out, capacity);
  j.object();
  const time_t seconds = unixMs / 1000;
  struct tm utc{};
  const bool haveUtc = unixMs >= 0;
  if (haveUtc && !gmtime_r(&seconds, &utc)) return 0;
  char date[24]{}, timestamp[32]{};
  strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc);
  snprintf(timestamp, sizeof(timestamp), "%s.%03uZ", date, unsigned(unixMs % 1000));
  if (haveUtc) j.string("_time", timestamp);
  else { j.key("_time"); j.raw("null"); }
  j.string("kind", "station_snapshot"); j.number("schema_version", schemaVersion);
  j.string("level", level); j.string("message", message);
  j.string("client_link", linkName(current.link));
  j.object("delta");
  j.optionalNumber("interval_ms", uint32_t(s.ms - previous.ms), comparable);
  j.optionalNumber("ack_errors", 0, false);
  j.optionalNumber("radio_errors", radioDelta, comparable);
  j.optionalNumber("control_gaps", gapDelta, comparable);
  j.optionalNumber("gps_checksum_errors", checksumDelta, comparable); j.end();
  j.string("firmware", SHORE_SPOTTER_VERSION); j.string("build", s.build);
  j.number("protocol", PROTO_VERSION); j.number("node_id", s.nodeId);
  j.number("boot_id", s.bootId); j.number("sample_id", s.id); j.number("sample_ms", s.ms);
  j.number("bound_client_id", s.clientId);
  j.object("health");
  j.number("heap_free", s.heapFree); j.number("heap_min", s.heapMin);
  j.number("heap_largest", s.heapLargest); j.number("reset_reason", s.resetReason);
  j.number("wifi_rssi", s.wifiRssi); j.number("battery_mv", s.batteryMv);
  j.optionalNumber("client_battery_mv", s.clientBatteryMv, s.clientBatteryMv != 0); j.end();
  j.object(cloudLegacyKeys ? "server_gps" : "station_gps");
  j.boolean("fresh", s.gpsFresh); j.boolean("raw_fix", s.gps.fix);
  j.real("lat", s.gps.fix ? s.gps.lat : NAN, 7); j.real("lon", s.gps.fix ? s.gps.lon : NAN, 7);
  j.real("hdop", s.gps.hdop); j.age("source_age_ms", s.gps.sourceAgeMs); j.age("arrival_age_ms", s.gps.arrivalAgeMs);
  j.number("backlog_drops", s.gpsBacklogDrops);
  j.number("epoch_interval_ms", s.gnssCounters.lastEpochIntervalMs);
  j.number("rmc", s.gnssCounters.rmcSentences); j.number("gga", s.gnssCounters.ggaSentences);
  j.number("checksum_errors", s.gnssCounters.checksumErrors);
  j.number("rejected_sentences", s.gnssCounters.rejectedSentences);
  j.number("duplicate_epochs", s.gnssCounters.duplicateEpochs);
  j.number("backward_epochs", s.gnssCounters.backwardEpochs);
  j.number("time_resyncs", s.gnssCounters.timeResyncs); j.end();
  j.object(cloudLegacyKeys ? "station" : "station_average"); j.number("window_ms", 30000); j.number("samples", s.stationSamples);
  j.real("mean_lat", s.stationSamples ? s.stationLat : NAN, 7);
  j.real("mean_lon", s.stationSamples ? s.stationLon : NAN, 7);
  j.real("rms_m", s.stationSamples ? s.stationRmsM : NAN);
  j.boolean("warning", s.stationWarning); j.end();
  j.object("gnss_rate"); j.number("requested_hz", 1000 / gnss_rate::kTargetIntervalMs);
  j.real("epoch_hz", s.gpsHz >= 0 ? s.gpsHz : NAN);
  j.real("rmc_hz", s.gpsRmcHz >= 0 ? s.gpsRmcHz : NAN);
  j.real("gga_hz", s.gpsGgaHz >= 0 ? s.gpsGgaHz : NAN); j.end();
  gnss(j, cloudLegacyKeys ? "server_gnss" : "station_gnss", s.stationGnss);
  gnss(j, "client_gnss", s.clientGnss, s.clientGnssAge != UINT32_MAX);
  j.age("client_gnss_rx_age_ms", s.clientGnssAge);
  j.object("client_diag"); j.age("rx_age_ms", s.clientDiagAge);
  j.optionalNumber("epoch_interval_ms", s.clientDiag.epochIntervalMs, s.clientDiagAge != UINT32_MAX);
  j.optionalNumber("backlog_drops", s.clientDiag.backlogDrops, s.clientDiagAge != UINT32_MAX); j.optionalNumber("nmea_errors", s.clientDiag.nmeaErrors, s.clientDiagAge != UINT32_MAX);
  j.optionalNumber("tx_errors", s.clientDiag.txErrors, s.clientDiagAge != UINT32_MAX); j.optionalNumber("skipped_slots", s.clientDiag.skippedSlots, s.clientDiagAge != UINT32_MAX);
  j.optionalNumber("status_bits", s.clientDiag.status, s.clientDiagAge != UINT32_MAX); j.end();
  j.object("client"); j.boolean("received", s.clientPresent);
  j.real("lat", s.clientPresent && s.clientFix ? s.clientLat : NAN, 7);
  j.real("lon", s.clientPresent && s.clientFix ? s.clientLon : NAN, 7);
  j.age("rx_age_ms", s.clientRxAge); j.age("source_age_ms", s.clientSourceAge);
  j.optionalNumber("seq", s.clientSeq, s.clientPresent); j.optionalNumber("fix", s.clientFix, s.clientPresent);
  j.optionalNumber("satellite_class", s.clientSatClass, s.clientPresent); j.age("hdop10", s.clientHdop10, 255);
  j.age("speed_cm_s", s.clientSpeedCmS, UINT16_MAX); j.optionalNumber("course_deg10", s.clientCourseDeg10, s.clientPresent);
  j.real("rssi_dbm", s.clientPresent ? s.rssi : NAN); j.real("snr_db", s.clientPresent ? s.snr : NAN); j.end();
  j.object("control");
  j.boolean("finishing_last_gps_target", s.finishingGpsTarget);
  j.string("mode", s.mode); j.string("source", s.source); j.string("uart_state", s.uartState);
  j.real("commanded_angle", s.angle); j.real("target", s.target);
  j.real("speed_limit", s.speed); j.real("commanded_velocity", s.velocity);
  j.boolean("pwm_ok", s.pwmOk); j.boolean("fault", s.motionFault);
  j.boolean("calibrated", s.calibrated); j.boolean("gps_usable", s.gpsUsable);
  j.boolean("prediction", s.prediction); j.boolean("prediction_active", s.predictionActive); j.real("mount_offset", s.mountOffset);
  j.real("declination", s.declinationReady ? s.declination : NAN);
  j.number("uart_late_polls", s.uartLate); j.number("uart_discarded_bytes", s.uartDiscarded);
  j.number("uart_rejected", s.uartRejected); j.number("motion_rejected", s.motionRejected); j.end();
  j.object("timing"); j.number("control_gap_max_ms", s.controlGapMax);
  j.number("control_gap_over_250ms", s.controlGapOver250);
  const char *durations[] = {"loop", "http", "lora", "motion", "bme280", "pmu", "oled", "ota"};
  for (size_t i = 0; i < 8; ++i) {
    j.object(durations[i]); j.number("last_us", s.durations[i].lastUs);
    j.number("max_us", s.durations[i].maxUs); j.number("over_50ms", s.durations[i].over50ms); j.end();
  }
  j.number("http_requests", s.httpRequests); j.number("http_slow_requests", s.httpSlowRequests);
  j.object("http_slowest"); j.string("route", http_timing::routeName(s.httpSlowest.route));
  j.number("total_us", s.httpSlowest.totalUs); j.number("pre_handler_us", s.httpSlowest.preHandlerUs);
  j.number("build_us", s.httpSlowest.buildUs); j.number("write_us", s.httpSlowest.writeUs);
  j.number("other_us", s.httpSlowest.otherUs); j.number("bytes", s.httpSlowest.bytesWritten);
  j.number("short_writes", s.httpSlowest.shortWrites); j.end(); j.end();
  j.object("radio"); j.boolean("ack_enabled", false);
  const char *counters[] = {"rx_data", "rx_telemetry", "rx_diagnostic", "rx_gnss_diagnostic",
    "errors", "rejected_length", "rejected_format", "rejected_binding", "rejected_sequence",
    "sequence_missing", "sequence_resyncs", "invalid_fix", "invalid_velocity", "ack_sent",
    "ack_errors", "ack_skipped", "recoveries", "last_data_interval_ms", "max_data_interval_ms"};
  for (size_t i = 0; i < 19; ++i) j.optionalNumber(counters[i], s.counters[i], i < 13 || i > 15);
  j.end();
  j.object("upload"); j.number("dropped_samples", droppedSamples);
  j.number("capture_max_us", captureMaxUs); j.number("event_total", s.eventTotal);
  j.number("event_lost", s.eventLost); j.end();
  j.array("events");
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < s.eventCount; ++i) {
    const auto &e = s.events[i];
    if (e.rawLength > sizeof(e.raw)) return 0;
    char raw[2 * sizeof(e.raw) + 1]{};
    for (size_t k = 0; k < e.rawLength; ++k) { raw[2*k] = hex[e.raw[k] >> 4]; raw[2*k+1] = hex[e.raw[k] & 15]; }
    j.object(); j.number("id", e.id); j.number("ms", e.ms);
    j.string("kind", packet_diagnostics::name(e.kind));
    j.number("client_id", e.clientId); j.number("seq", e.seq); j.number("length", e.length);
    j.age("source_age_ms", e.sourceAgeMs, UINT16_MAX);
    j.real("rssi_dbm", e.rawLength ? e.rssiDbm10 / 10.0 : NAN);
    j.real("snr_db", e.rawLength ? e.snrQuarterDb / 4.0 : NAN);
    j.number("code", e.code); j.number("flags", e.flags); j.string("raw_hex", raw); j.end();
  }
  j.end(true); j.end(); j.raw("\n");
  if (changeCount) {
    j.object();
    if (haveUtc) j.string("_time", timestamp);
    else { j.key("_time"); j.raw("null"); }
    j.string("kind", "state_change"); j.number("schema_version", schemaVersion);
    j.string("level", level); j.number("node_id", s.nodeId); j.number("boot_id", s.bootId);
    j.number("sample_id", s.id); j.number("sample_ms", s.ms); j.number("bound_client_id", s.clientId);
    // All strings are fixed labels or bounded mode/source fields; this buffer
    // covers all seven simultaneous changes without cutting a UTF-8 character.
    char description[1024]{};
    size_t used = 0;
    for (size_t i = 0; i < changeCount; ++i) {
      const int n = snprintf(description + used, sizeof(description) - used, "%s%s: %s -> %s",
          i ? " | " : "", changed[i].label, changed[i].from, changed[i].to);
      if (n < 0 || size_t(n) >= sizeof(description) - used) return 0;
      used += n;
    }
    j.string("message", description); j.array("changes");
    for (size_t i = 0; i < changeCount; ++i) {
      j.object(); j.string("field", changed[i].field); j.string("from", changed[i].from); j.string("to", changed[i].to); j.end();
    }
    j.end(true); j.end(); j.raw("\n");
  }
  const size_t length = j.size();
  if (length) {
    if (history) *history = current;
    if (records) *records = 1 + (changeCount ? 1 : 0);
  }
  return length;
}
} // namespace axiom_log
