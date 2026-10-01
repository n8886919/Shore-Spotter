#pragma once
#include "protocol.h"
#include "gnss_snapshot.h"

// v5 type 6: one complete 36-byte report, independently decodable.
namespace gnss_diagnostics {
constexpr uint32_t kFreshMs = 90000;
struct Report {
  uint32_t sourceAgeMs = UINT32_MAX, utcMs = UINT32_MAX;
  // 65535 unknown; 65534 means >=65534 ms. Counters saturate at 65535.
  uint16_t byteAgeMs = UINT16_MAX, sentenceAgeMs = UINT16_MAX, advanceAgeMs = UINT16_MAX;
  uint16_t epochs = 0, resyncs = 0, missingTime = 0, backwards = 0;
  uint16_t duplicates = 0, rejected = 0, checksum = 0;
  // bits 0..4: sample present, raw fix, RMC, GGA, recovering; 5..7 reserved.
  uint8_t flags = 0, satellites = 255;
};
inline uint16_t counter(uint32_t n) { return n > 65535 ? 65535 : n; }
inline uint16_t age(uint32_t n) { return n == UINT32_MAX ? 65535 : n > 65534 ? 65534 : n; }
inline const char *state(const Report &r) {
  if (r.byteAgeMs == 65535 || r.byteAgeMs >= 2000) return "no_uart";
  if (r.sentenceAgeMs == 65535 || r.sentenceAgeMs >= 2000) return "no_nmea";
  if (r.flags & 16) return "recovering";
  if (!(r.flags & 1)) return "no_epoch";
  if (r.advanceAgeMs == UINT16_MAX || r.advanceAgeMs >= 2000) return "stale_epoch";
  return (r.flags & 2) ? "fresh_fix" : "no_fix";
}
inline Report capture(const gnss_snapshot::Collector &c, uint32_t now) {
  Report r;
  gnss_snapshot::Snapshot s;
  if (c.sample(now, s)) {
    r.sourceAgeMs = s.sourceAgeMs; r.utcMs = s.epochMsOfDay;
    r.flags = 1 | (s.fix ? 2 : 0) | (s.haveRmc ? 4 : 0) | (s.haveGga ? 8 : 0);
    r.satellites = s.satellites;
  }
  if (c.recovering()) r.flags |= 16;
  r.byteAgeMs = age(c.byteAgeMs(now)); r.sentenceAgeMs = age(c.sentenceAgeMs(now));
  r.advanceAgeMs = age(c.advanceAgeMs(now));
  const auto &g = c.stats();
  r.epochs = counter(g.snapshots); r.resyncs = counter(g.timeResyncs);
  r.missingTime = counter(g.missingTime); r.backwards = counter(g.backwardEpochs);
  r.duplicates = counter(g.duplicateEpochs); r.rejected = counter(g.rejectedSentences);
  r.checksum = counter(g.checksumErrors);
  return r;
}
inline void put32(uint8_t *out, uint32_t n) {
  protocol::putU16(out, n); protocol::putU16(out + 2, n >> 16);
}
inline uint32_t get32(const uint8_t *p) {
  return uint32_t(protocol::getU16(p)) | (uint32_t(protocol::getU16(p + 2)) << 16);
}
inline bool validReport(const Report &r) {
  return !(r.flags & 0xE0) && (r.utcMs == UINT32_MAX || r.utcMs < gnss_snapshot::kDayMs);
}
inline size_t encode(uint8_t *buf, size_t capacity, const PacketHeader &h, const Report &r) {
  if (!protocol::canEncode(buf, capacity, h, MSG_GNSS_DIAGNOSTIC) || !validReport(r)) return 0;
  protocol::encodeHeader(buf, h);
  put32(buf + 6, r.sourceAgeMs); put32(buf + 10, r.utcMs);
  const uint16_t values[10] = {r.byteAgeMs, r.sentenceAgeMs, r.advanceAgeMs, r.epochs, r.resyncs,
      r.missingTime, r.backwards, r.duplicates, r.rejected, r.checksum};
  for (unsigned i = 0; i < 10; ++i) protocol::putU16(buf + 14 + 2 * i, values[i]);
  buf[34] = r.flags; buf[35] = r.satellites;
  return GNSS_DIAGNOSTIC_PACKET_LEN;
}
inline bool decode(const uint8_t *buf, size_t n, PacketHeader &h, Report &out) {
  PacketHeader header{};
  if (!protocol::decodeHeader(buf, n, header) || header.msgType != MSG_GNSS_DIAGNOSTIC) return false;
  Report r;
  r.sourceAgeMs = get32(buf + 6); r.utcMs = get32(buf + 10);
  uint16_t *fields[10] = {&r.byteAgeMs, &r.sentenceAgeMs, &r.advanceAgeMs, &r.epochs, &r.resyncs,
      &r.missingTime, &r.backwards, &r.duplicates, &r.rejected, &r.checksum};
  for (unsigned i = 0; i < 10; ++i) *fields[i] = protocol::getU16(buf + 14 + 2 * i);
  r.flags = buf[34]; r.satellites = buf[35];
  if (!validReport(r)) return false;
  h = header; out = r; return true;
}
// Single complete report: no assembly, old sequence holdoff, or reboot handshake.
// Caller validates the bound Client. Low-rate diagnostics never refresh DATA.
class Latest {
 public:
  bool accept(const PacketHeader &h, const Report &r, uint32_t now) {
    if (h.msgType != MSG_GNSS_DIAGNOSTIC || !protocol::validClientId(h.clientId) || !validReport(r)) return false;
    report_ = r; received_ = true; rxMs_ = now; return true;
  }
  bool received() const { return received_; }
  uint32_t rxAgeMs(uint32_t now) const { return received_ ? now - rxMs_ : UINT32_MAX; }
  const Report &report() const { return report_; }
 private:
  Report report_;
  bool received_ = false;
  uint32_t rxMs_ = 0;
};
}  // namespace gnss_diagnostics
