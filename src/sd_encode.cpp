#include "sd_log.h"
#include <stdio.h>
#include <stdarg.h>

namespace sd_log {
namespace {
struct Writer {
  char *out; size_t cap, used = 0; bool ok = true;
  void add(const char *format, ...) {
    if (!ok) return;
    va_list args; va_start(args, format);
    const int n = vsnprintf(out + used, cap - used, format, args);
    va_end(args);
    if (n < 0 || size_t(n) >= cap - used) { ok = false; return; }
    used += n;
  }
};
}
size_t encodePacket(char *out, size_t capacity, uint32_t boot, const Packet &p) {
  if (!capacity || p.rawLength > sizeof(p.raw)) return 0;
  Writer w{out, capacity}; const auto &e = p.event;
  const bool rfKnown = e.kind != packet_diagnostics::Kind::RadioError || e.code == -7;
  w.add("{\"kind\":\"lora_packet\",\"schema_version\":1,\"boot_id\":%lu,\"event_id\":%lu,\"ms\":%lu,"
        "\"result\":\"%s\",\"length\":%u,\"code\":%d,\"client_id\":%u,\"seq\":%u,\"rssi_dbm\":",
        (unsigned long)boot, (unsigned long)e.id, (unsigned long)e.ms,
        packet_diagnostics::name(e.kind), e.length, e.code, e.clientId, e.seq);
  if (rfKnown) w.add("%.1f", e.rssiDbm10 / 10.0); else w.add("null");
  w.add(",\"snr_db\":");
  if (rfKnown) w.add("%.2f", e.snrQuarterDb / 4.0); else w.add("null");
  w.add(",\"raw_length\":%u,\"raw_truncated\":%s,\"raw_hex\":\"", p.rawLength,
        p.rawLength < e.length ? "true" : "false");
  for (size_t i = 0; i < p.rawLength; ++i) w.add("%02x", p.raw[i]);
  w.add("\"}\n"); return w.ok ? w.used : 0;
}
size_t encodeText(char *out, size_t capacity, uint32_t boot, const Text &t) {
  if (!capacity || t.length > sizeof(t.bytes)) return 0;
  Writer w{out, capacity};
  w.add("{\"kind\":\"text_log\",\"boot_id\":%lu,\"ms\":%lu,\"text\":\"",
        (unsigned long)boot, (unsigned long)t.ms);
  for (size_t i = 0; i < t.length; ++i) {
    const unsigned char c = t.bytes[i];
    if (c == '"' || c == '\\') w.add("\\%c", c);
    else if (c < 32) w.add("\\u%04x", c);
    else w.add("%c", c);
  }
  w.add("\"}\n"); return w.ok ? w.used : 0;
}
size_t encodeClient(char *out, size_t capacity, uint32_t boot, const ClientRecord &r) {
  if (!capacity || r.length > sizeof(r.raw)) return 0;
  const char *names[] = {"gps_epoch", "gps_resume", "gps_pause", "tx_started", "tx_sent", "tx_failed", "tx_timeout", "tx_cancelled"};
  if (unsigned(r.kind) >= sizeof(names) / sizeof(names[0])) return 0;
  Writer w{out, capacity};
  w.add("{\"kind\":\"client_event\",\"event\":\"%s\",\"boot_id\":%lu,\"ms\":%lu,\"node_id\":%u,",
        names[unsigned(r.kind)], (unsigned long)boot, (unsigned long)r.ms, r.nodeId);
  if (r.kind >= ClientKind::TxStarted) {
    PacketHeader h{}; const bool header = protocol::decodeHeader(r.raw, r.length, h);
    w.add("\"tx_started_ms\":%lu,\"tx_status\":%d,\"type\":%u,\"seq\":%u,\"header_valid\":%s,\"raw_hex\":\"",
          (unsigned long)r.txStartedMs, r.txStatus, h.msgType, h.seq, header ? "true" : "false");
    for (unsigned i = 0; i < r.length; ++i) w.add("%02x", r.raw[i]);
    w.add("\"}\n");
    return w.ok ? w.used : 0;
  }
  const auto &s = r.gps; const auto &g = r.gnss;
  const char *reason = !s.haveEpoch ? "no_epoch" : s.arrivalAgeMs >= 2000 ? "stale_epoch" :
      !s.fix ? "no_fix" : !s.haveGga ? "missing_gga" :
      (s.satellites == 255 || s.satellites < 6 || !isfinite(s.hdop) || s.hdop < 0 || s.hdop > 3) ? "poor_quality" : "usable";
  w.add("\"gps_state\":\"%s\",\"have_epoch\":%s,\"fix\":%s,\"velocity_valid\":%s,\"epoch_ms\":%lu,\"arrival_age_ms\":%lu,\"source_age_ms\":%lu,\"rmc\":%s,\"gga\":%s,",
        reason, s.haveEpoch ? "true" : "false", s.fix ? "true" : "false", s.velocityValid ? "true" : "false", (unsigned long)s.epochMsOfDay, (unsigned long)s.arrivalAgeMs,
        (unsigned long)s.sourceAgeMs, s.haveRmc ? "true" : "false", s.haveGga ? "true" : "false");
  auto number = [&](const char *key, double value) {
    w.add("\"%s\":", key); if (isfinite(value)) w.add("%.7f,", value); else w.add("null,");
  };
  number("lat", s.haveEpoch && s.fix ? s.lat : NAN); number("lon", s.haveEpoch && s.fix ? s.lon : NAN);
  number("hdop", s.hdop); number("speed_mps", s.velocityValid ? s.speedMps : NAN);
  number("course_deg", s.velocityValid ? s.courseDeg : NAN);
  w.add("\"satellites\":%u,\"recovering\":%s,\"byte_age_ms\":%lu,\"sentence_age_ms\":%lu,"
        "\"epochs\":%lu,\"epoch_interval_ms\":%lu,\"rmc_count\":%lu,\"gga_count\":%lu,"
        "\"checksum_errors\":%lu,\"rejected\":%lu,\"duplicates\":%lu,\"backwards\":%lu,\"missing_time\":%lu,\"resyncs\":%lu,"
        "\"invalidations\":%lu,\"line_overflows\":%lu,\"accepted_sentences\":%lu,"
        "\"backlog_drops\":%lu,\"tx_sent\":%lu,\"tx_errors\":%lu,\"skipped_slots\":%lu,\"radio_restarts\":%lu,"
        "\"loop_gap_max_ms\":%lu,\"loop_over_250ms\":%lu,\"heap_free\":%lu,\"battery_mv\":%u,"
        "\"epoch_hz\":%.2f,\"rmc_hz\":%.2f,\"gga_hz\":%.2f}\n",
        s.satellites, r.recovering ? "true" : "false", (unsigned long)r.byteAgeMs, (unsigned long)r.sentenceAgeMs,
        (unsigned long)g.snapshots, (unsigned long)g.lastEpochIntervalMs, (unsigned long)g.rmcSentences, (unsigned long)g.ggaSentences,
        (unsigned long)g.checksumErrors, (unsigned long)g.rejectedSentences, (unsigned long)g.duplicateEpochs,
        (unsigned long)g.backwardEpochs, (unsigned long)g.missingTime, (unsigned long)g.timeResyncs,
        (unsigned long)g.invalidations, (unsigned long)g.overflows, (unsigned long)g.acceptedSentences,
        (unsigned long)r.backlogDrops, (unsigned long)r.txCount, (unsigned long)r.txErrors, (unsigned long)r.skipped,
        (unsigned long)r.radioRestarts, (unsigned long)r.loopGapMaxMs, (unsigned long)r.loopOver250,
        (unsigned long)r.heapFree, r.batteryMv, r.epochHz, r.rmcHz, r.ggaHz);
  return w.ok ? w.used : 0;
}
}
