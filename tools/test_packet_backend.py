"""Host integration of the actual Client/Server v4 packet path, without hardware.

Run: python3 tools/test_packet_backend.py
The GNSS collector and wire codecs are real; only clock, cached sensors and the
bound device IDs are stubbed. This does not validate RF or physical GNSS timing.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("SHORE_PACKET_BACKEND_SOURCE", ROOT / "src/main.cpp")).read_text()


def block(marker):
    start = SOURCE.index(marker)
    body = SOURCE.index("{", start)
    depth = 0
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    for match in re.finditer(tokens, SOURCE[body:]):
        token = match.group()
        if token == "{":
            depth += 1
        elif token == "}":
            depth -= 1
            if not depth:
                return SOURCE[start:body + match.end()]
    raise ValueError(f"Unterminated source block: {marker}")


cpp = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include "protocol.h"
#include "gnss_snapshot.h"
#include "lora_schedule.h"
#include "tracking_policy.h"
#include "command_freshness.h"
#include "packet_diagnostics.h"
using std::isfinite;
template<class T> T constrain(T value, T low, T high) {
  return std::min(std::max(value, low), high);
}
uint32_t clockMs = 0;
uint32_t millis() { return clockMs; }
uint16_t nodeId = 0xE91C, gpsClientId = 0xE91C;
uint16_t txSeq = 0, telemetrySeq = 0, diagnosticSeq = 0;
uint32_t gpsBacklogDrops = 0, clientTxErrors = 0, dataSkippedSlots = 0;
uint16_t cachedBatteryMv = 4150;
int16_t cachedTempC10 = 243;
uint8_t cachedHumidityPct = 68;
gnss_snapshot::Collector gnssCollector;
lora_schedule::AckWindow expectedAck;
'''
for marker in ["struct DecodedData {", "struct DecodedTelemetry {"]:
    cpp += block(marker) + ";\n"
for name in ["SEND_INTERVAL_MS", "ACK_EVERY_N", "GPS_FIX_MAX_AGE_MS", "RSSI_WINDOW",
             "ACK_START_MAX_AGE_MS", "TELEMETRY_SLOT_GUARD_MS"]:
    match = re.search(r"constexpr[^;\n]*\b" + name + r"\b[^;]*;", SOURCE)
    if not match:
        raise ValueError(f"Missing packet timing constant {name}")
    cpp += match.group() + "\n"
cpp += r'''
DecodedData lastData{};
DecodedTelemetry lastTelemetry{};
DiagnosticPayload lastClientDiagnostic{};
bool havePkt = false, haveTelemetry = false, haveClientDiagnostic = false;
uint32_t lastRxMs = 0, lastTelemetryRxMs = 0, lastClientDiagnosticMs = 0;
float lastRssi = 0, lastSnr = 0, receivedPacketRssi = 0, receivedPacketSnr = 0;
float rssiRing[RSSI_WINDOW]{}, snrRing[RSSI_WINDOW]{};
size_t rssiRingIdx = 0, rssiRingCount = 0;
uint32_t rxDataCount = 0, rxTelemetryCount = 0, rxDiagnosticCount = 0;
uint32_t rxDropCount = 0, rxWinDrop = 0, rxWinTelem = 0, rxWinData = 0;
uint32_t rejectedLength = 0, rejectedFormat = 0, rejectedBinding = 0, rejectedGpsSequence = 0;
uint32_t sequenceMissing = 0, rxWinMissing = 0, sequenceResyncs = 0;
uint32_t lastDataIntervalMs = 0, maxDataIntervalMs = 0, lastSourceEpochIntervalMs = 0;
uint32_t sourceEpochUpdates = 0, lastEstimatedSourceMs = 0;
uint32_t invalidFixPackets = 0, invalidVelocityPackets = 0, pktsThisWindow = 0, pktWindowStartMs = 0;
uint32_t dataAirtimeMs = 165, ackAirtimeMs = 145, ackSkippedCount = 0;
float cachedPktRate = 0, rxWinRssiMin = 0, rxWinRssiMax = 0, rxWinSnrMin = 0, rxWinSnrMax = 0;
double rxWinRssiSum = 0, rxWinSnrSum = 0;
bool haveSourceEstimate = false, rxWinHaveSeq = false, serverRxReady = true;
uint16_t rxWinFirstSeq = 0, rxWinLastSeq = 0;
int clientHumBaselinePct = -1;
uint8_t ackPacketBuffer[ACK_PACKET_LEN];
command_freshness::RadioSequence gpsSequence;
packet_diagnostics::Ring<64> packetEvents;
struct AckTransmitterStub {
  unsigned starts = 0;
  PacketHeader header{};
  AckPayload payload{};
  int start(const uint8_t *buf, size_t length, uint32_t, uint32_t) {
    assert(protocol::decodeAck(buf, length, header, payload));
    ++starts; return 1;
  }
} ackTransmitter;
void handleAckResult(int result) { assert(result == 1); }
'''
# The enclosing ROLE_CLIENT/ROLE_SERVER directives are outside these function
# bodies, so the real paired endpoints can coexist in a single host executable.
for marker in [
    "static bool isClientAllowed(",
    "static bool parseDataPacket(",
    "static bool parseTelemetryPacket(",
    "static size_t buildAckPacket(",
    "static bool parseAckPacket(",
    "static bool gpsFixFresh(",
    "static size_t buildDataPacket(",
    "static size_t buildTelemetryPacket(",
    "static size_t buildDiagnosticPacket(",
    "static void recordPacketEvent(",
    "static bool acceptRadioPacket(",
]:
    cpp += block(marker) + "\n"

cpp += r'''
std::string sentence(const std::string &body) {
  unsigned crc = 0;
  for (unsigned char ch : body) crc ^= ch;
  char suffix[8]; std::snprintf(suffix, sizeof(suffix), "*%02X\r\n", crc);
  return "$" + body + suffix;
}
std::string rmc(const char *utc = "120000.000", const char *speedKnots = "9.72",
                const char *course = "90.0", const char *valid = "A") {
  return std::string("GNRMC,") + utc + "," + valid +
      ",2407.40734,N,12107.40734,E," + speedKnots + "," + course + ",130926,,,A";
}
std::string gga(const char *utc = "120000.000", const char *sats = "09",
                const char *hdop = "1.50", const char *quality = "1") {
  return std::string("GNGGA,") + utc + ",2407.40734,N,12107.40734,E," +
      quality + "," + sats + "," + hdop + ",10.0,M,0.0,M,,";
}
void feedWire(const std::string &wire, uint32_t now) {
  clockMs = now;
  for (char ch : wire) gnssCollector.feed(ch, now);
}
void feed(const std::string &body, uint32_t now) { feedWire(sentence(body), now); }
void reset() {
  clockMs = 1000; txSeq = telemetrySeq = diagnosticSeq = 0;
  gpsBacklogDrops = clientTxErrors = dataSkippedSlots = 0;
  nodeId = gpsClientId = 0xE91C;
  cachedBatteryMv = 4150; cachedTempC10 = 243; cachedHumidityPct = 68;
  gnssCollector = gnss_snapshot::Collector{};
  expectedAck = lora_schedule::AckWindow{};
}
void pair(const char *speed = "9.72", const char *course = "90.0",
          const char *sats = "09", const char *hdop = "1.50") {
  feed(rmc("120000.000", speed, course), 1000);
  feed(gga("120000.000", sats, hdop), 1100);
  clockMs = 1200;
}
DecodedData roundTripData() {
  uint8_t wire[DATA_PACKET_LEN + 1]; std::memset(wire, 0xA5, sizeof(wire));
  const size_t n = buildDataPacket(wire);
  assert(n == 17 && wire[17] == 0xA5);
  assert(wire[0] == 0x53 && wire[1] == 0x41);
  DecodedData d{}; assert(parseDataPacket(wire, n, d));
  return d;
}
DecodedTelemetry roundTripTelemetry() {
  uint8_t wire[TELEMETRY_PACKET_LEN + 1]; std::memset(wire, 0xA5, sizeof(wire));
  const size_t n = buildTelemetryPacket(wire);
  assert(n == 11 && wire[11] == 0xA5);
  assert(wire[1] == 0x44);
  DecodedTelemetry d{}; assert(parseTelemetryPacket(wire, n, d));
  return d;
}
void test_actual_gnss_to_wire_to_server() {
  reset(); pair();
  const auto d = roundTripData();
  assert(d.srcId == 0xE91C && d.seq == 0 && d.fix && d.velocityValid);
  assert(std::abs(d.lat - 24.123456) < 1e-10);
  assert(std::abs(d.lon - 121.123456) < 1e-10);
  assert(d.speedCmS == 500 && d.courseDeg10 == 900);
  assert(d.satelliteClass == 3 && d.satellites == 8 && d.hdop10 == 15);
  assert(d.age10ms >= 20 && d.age10ms < 50);
  const auto t = roundTripTelemetry();
  assert(t.srcId == d.srcId && t.satellites == 9);
  assert(t.batteryMv == 4150 && t.tempC == 24 && t.humidityPct == 68);
  assert(d.satellites != t.satellites);  // class lower bound is not an exact count
  assert(txSeq == 1 && telemetrySeq == 1);
  std::cout << "PASS actual NMEA -> DATA17 -> Server E6/speed/flags and TEL11 exact diagnostics\n";
}
void test_half_second_transmission_does_not_refresh_gnss_age() {
  reset(); pair();
  assert(SEND_INTERVAL_MS == 500 && ACK_EVERY_N == 8);
  const auto first = roundTripData();
  clockMs += SEND_INTERVAL_MS;
  const auto second = roundTripData();
  assert(second.seq == first.seq + 1);
  assert(second.age10ms == first.age10ms + 50);
  assert(second.lat == first.lat && second.lon == first.lon && second.fix);
  // Repeated NMEA with the same epoch must not refresh the source timestamp.
  feed(rmc(), 1800); feed(gga(), 1900); clockMs = 2200;
  const auto duplicate = roundTripData();
  assert(duplicate.age10ms == first.age10ms + 100);
  assert(gnssCollector.stats().snapshots == 1);
  assert(gnssCollector.stats().duplicateEpochs == 2);
  // At 1.8 s plus its UART allowance, a position is already unusable even
  // though the source age itself is still representable and below 2 seconds.
  clockMs = 2800;
  const auto conservative = roundTripData();
  assert(!conservative.fix && !conservative.velocityValid);
  assert(conservative.age10ms < 200 && conservative.hdop10 == 255);
  clockMs = 4000;
  const auto stale = roundTripData();
  assert(!stale.fix && !stale.velocityValid && stale.age10ms == 255);
  assert(stale.speedCmS == UINT16_MAX && stale.satellites == 255);
  assert(roundTripTelemetry().satellites == 255);
  std::cout << "PASS 500 ms RF produces new sequence, not invented GPS epochs; age and uncertainty expire data\n";
}
void test_speed_threshold_unknown_overflow_and_direction() {
  reset(); pair("0.56");  // 0.288 m/s rounds to code 3, but is below raw 0.3 gate
  auto d = roundTripData();
  assert(d.fix && d.speedCmS == 30 && !d.velocityValid);
  reset(); pair("0.60");
  d = roundTripData(); assert(d.fix && d.speedCmS == 30 && d.velocityValid);
  reset(); pair("0.00");
  d = roundTripData(); assert(d.fix && d.speedCmS == 0 && !d.velocityValid);
  reset(); pair("50.00");  // 25.722 m/s exceeds uint8 speed domain
  d = roundTripData(); assert(d.fix && d.speedCmS == UINT16_MAX && !d.velocityValid);
  reset(); pair("");
  d = roundTripData(); assert(d.fix && d.speedCmS == UINT16_MAX && !d.velocityValid);
  reset(); pair("9.72", "");
  d = roundTripData(); assert(d.fix && d.courseDeg10 == 4095 && !d.velocityValid);
  reset(); pair("9.72", "360.0");
  d = roundTripData(); assert(d.fix && d.courseDeg10 == 4095 && !d.velocityValid);
  reset(); pair("9.72", "359.99");
  d = roundTripData(); assert(d.fix && d.courseDeg10 == 0 && d.velocityValid);
  std::cout << "PASS actual raw-speed threshold, overflow/unknown fallback and course normalization\n";
}
void test_hdop_rounds_up_and_epoch_mismatch_does_not_reuse_quality_or_velocity() {
  reset(); pair("9.72", "90", "07", "3.00");
  auto d = roundTripData();
  assert(d.satelliteClass == 2 && d.satellites == 6 && d.hdop10 == 30);
  assert(tracking_policy::usableGps(d.fix, d.satellites, d.hdop10 / 10.0f, 0));
  reset(); pair("9.72", "90", "07", "3.01");
  d = roundTripData(); assert(d.fix && d.hdop10 == 31);
  assert(!tracking_policy::usableGps(d.fix, d.satellites, d.hdop10 / 10.0f, 0));
  reset(); pair("9.72", "90", "09", "1.51");
  d = roundTripData(); assert(d.hdop10 == 16);
  reset(); pair("9.72", "90", "09", "");
  d = roundTripData(); assert(d.fix && d.hdop10 == 255);
  reset(); pair();
  feed(gga("120001.000"), 2100); clockMs = 2200;
  d = roundTripData();
  assert(d.fix && d.satelliteClass == 3 && d.hdop10 == 15);
  assert(!d.velocityValid && d.speedCmS == UINT16_MAX && d.courseDeg10 == 4095);
  const auto ggaOnlyAge = d.age10ms;
  feed(rmc("120002.000"), 3100); clockMs = 3200;
  d = roundTripData();
  // Until the matching GGA arrives the collector may retain the previous
  // position/quality snapshot, but it MUST retain that snapshot's older age
  // and absent velocity rather than combine it with the new RMC vector.
  assert(d.fix && !d.velocityValid && d.hdop10 == 15);
  assert(d.age10ms == ggaOnlyAge + 100);
  feed(gga("120002.000"), 3300); d = roundTripData();
  assert(d.fix && d.velocityValid && d.satelliteClass == 3);
  assert(d.age10ms < ggaOnlyAge + 100);
  reset(); feed(rmc("120002.000"), 3100); clockMs = 3200;
  d = roundTripData();
  assert(d.fix && d.velocityValid && d.satelliteClass == 0 && d.hdop10 == 255);
  assert(d.satellites == 255 && roundTripTelemetry().satellites == 255);
  feed(gga("120001.000"), 3300);  // older GGA cannot attach to the newer RMC
  d = roundTripData(); assert(d.satelliteClass == 0 && d.hdop10 == 255);
  std::cout << "PASS HDOP ceiling and missing/mismatched RMC/GGA keep quality and velocity separate\n";
}
void test_no_fix_checksum_backlog_and_coordinate_range() {
  reset(); auto d = roundTripData();
  assert(!d.fix && !d.velocityValid && d.age10ms == 255 && d.satellites == 255);
  pair(); const auto old = roundTripData();
  auto bad = sentence(rmc("120001.000"));
  bad[bad.size() - 4] = bad[bad.size() - 4] == '0' ? '1' : '0';
  feedWire(bad, 1700);
  d = roundTripData();
  assert(gnssCollector.stats().checksumErrors == 1);
  assert(d.age10ms == old.age10ms + 50);  // rejected checksum cannot create a fresh epoch
  gnssCollector.invalidate(clockMs); d = roundTripData();
  assert(!d.fix && !d.velocityValid && d.age10ms == 255);
  feed(rmc(), 1800); feed(gga(), 1900); d = roundTripData();
  assert(!d.fix && d.age10ms == 255);  // invalidated same epoch stays rejected
  feed(rmc("120001.000"), 2100); feed(gga("120001.000"), 2200);
  d = roundTripData(); assert(d.fix && d.velocityValid);
  reset(); feed(rmc("120000.000", "9.72", "90", "V"), 1000); feed(gga(), 1100);
  d = roundTripData(); assert(!d.fix && !d.velocityValid);
  reset(); feed(rmc(), 1000); feed(gga("120000.000", "09", "1.5", "0"), 1100);
  d = roundTripData(); assert(!d.fix && !d.velocityValid);
  reset();
  feed("GNRMC,120000.000,A,0000.00000,N,00000.00000,E,9.72,90,130926,,,A", 1000);
  feed("GNGGA,120000.000,0000.00000,N,00000.00000,E,1,09,1.5,10,M,0,M,,", 1100);
  d = roundTripData();
  assert(!d.fix && !d.velocityValid && d.lat == 24 && d.lon == 121);
  cachedBatteryMv = 0; cachedTempC10 = INT16_MIN; cachedHumidityPct = 255;
  const auto t = roundTripTelemetry();
  assert(t.batteryMv == 0 && t.tempC == INT8_MIN && t.humidityPct == 255);
  std::cout << "PASS checksum rejection, explicit no-fix, backlog invalidation and out-of-region canonical encoding\n";
}
void test_independent_data_sequence_survives_telemetry_and_wrap() {
  reset(); pair(); txSeq = 7; telemetrySeq = 123;
  auto d = roundTripData(); assert(d.seq == 7 && txSeq == 8);
  for (int i = 0; i < 5; ++i) roundTripTelemetry();
  assert(txSeq == 8 && telemetrySeq == 128);
  d = roundTripData(); assert(d.seq == 8 && d.seq % ACK_EVERY_N == 0);
  txSeq = 65535;
  assert(roundTripData().seq == 65535);
  roundTripTelemetry(); assert(roundTripData().seq == 0);
  assert(txSeq == 1 && telemetrySeq == 129);
  std::cout << "PASS actual DATA/TEL builders own separate counters and retain scheduled ACKs across wrap\n";
}
void test_actual_diagnostic_counter_saturation_flags_and_separate_sequence() {
  reset(); uint8_t wire[DIAGNOSTIC_PACKET_LEN + 1]; std::memset(wire, 0xA5, sizeof(wire));
  PacketHeader h{}; DiagnosticPayload d{};
  assert(buildDiagnosticPacket(wire) == 17 && wire[17] == 0xA5);
  assert(protocol::decodeDiagnostic(wire, 17, h, d));
  assert(h.msgType == MSG_DIAGNOSTIC && h.clientId == nodeId && h.seq == 0);
  assert(d.status == 0 && d.epochIntervalMs == 0);
  pair(); feed(rmc("120000.500"), 1500); feed(gga("120000.500"), 1600); clockMs = 1700;
  auto corrupt = sentence(rmc()); corrupt[corrupt.size() - 4] ^= 1;
  feedWire(corrupt, clockMs);
  gpsBacklogDrops = 65536; clientTxErrors = UINT32_MAX; dataSkippedSlots = 65534;
  const uint16_t beforeData = txSeq, beforeTelemetry = telemetrySeq;
  assert(buildDiagnosticPacket(wire) == 17);
  assert(protocol::decodeDiagnostic(wire, 17, h, d));
  assert(h.seq == 1 && diagnosticSeq == 2 && txSeq == beforeData && telemetrySeq == beforeTelemetry);
  assert(d.epochIntervalMs == 500 && d.backlogDrops == 65535);
  assert(d.nmeaErrors == 1 && d.txErrors == 65535 && d.skippedSlots == 65534);
  assert(d.status == 0x3F);
  diagnosticSeq = 65535; buildDiagnosticPacket(wire);
  assert(protocol::decodeDiagnostic(wire, 17, h, d) && h.seq == 65535 && diagnosticSeq == 0);
  std::cout << "PASS actual DIAG17 builder reports GNSS/counters, saturates totals and owns a separate sequence\n";
}
void test_bound_server_and_all_actual_parsers_reject_bad_lengths() {
  reset(); pair();
  uint8_t data[32]{}; const size_t n = buildDataPacket(data);
  DecodedData d{};
  gpsClientId = 0; assert(!parseDataPacket(data, n, d));
  gpsClientId = 0x1234; assert(!parseDataPacket(data, n, d));
  gpsClientId = nodeId; assert(parseDataPacket(data, n, d));
  for (size_t size = 0; size <= sizeof(data); ++size) if (size != n)
    assert(!parseDataPacket(data, size, d));
  uint8_t tel[32]{}; const size_t tn = buildTelemetryPacket(tel);
  DecodedTelemetry t{};
  for (size_t size = 0; size <= sizeof(tel); ++size) if (size != tn)
    assert(!parseTelemetryPacket(tel, size, t));
  assert(!parseTelemetryPacket(data, n, t)); assert(!parseDataPacket(tel, tn, d));
  gpsClientId = 0x1234; assert(!parseTelemetryPacket(tel, tn, t)); gpsClientId = nodeId;
  data[1] = 0x31; assert(!parseDataPacket(data, n, d));
  data[1] = 3; assert(!parseDataPacket(data, 32, d));
  data[1] = 0x4F; assert(!parseDataPacket(data, n, d));
  uint8_t ack[32]{}; buildAckPacket(ack, nodeId, 8, -90.5f, -13.25f);
  AckPayload a{}; expectedAck.expect(8, clockMs, 500);
  for (size_t size = 0; size <= sizeof(ack); ++size) if (size != ACK_PACKET_LEN)
    assert(!parseAckPacket(ack, size, a));
  assert(parseAckPacket(ack, ACK_PACKET_LEN, a));  // malformed lengths didn't consume the window
  std::cout << "PASS actual server binding and all parsers reject wrong type, v3, truncated or overlong RF packets\n";
}
void test_ack_client_id_sequence_window_replay_and_snr() {
  reset(); uint8_t ack[ACK_PACKET_LEN]; AckPayload a{};
  expectedAck.expect(8, clockMs, 500);
  assert(buildAckPacket(ack, 0x1234, 8, -98.7f, -13.25f) == 11);
  assert(!parseAckPacket(ack, sizeof(ack), a));
  buildAckPacket(ack, nodeId, 9, -98.7f, -13.25f);
  assert(!parseAckPacket(ack, sizeof(ack), a));
  buildAckPacket(ack, nodeId, 8, -98.7f, -13.25f);
  clockMs += 499; assert(parseAckPacket(ack, sizeof(ack), a));
  assert(a.ackSeq == 8 && a.rssiDbm10 == -987 && a.snrQuarterDb == -53);
  assert(!parseAckPacket(ack, sizeof(ack), a));
  expectedAck.expect(8, 2000, 500); clockMs = 2500;
  assert(!parseAckPacket(ack, sizeof(ack), a));  // exact deadline is expired
  expectedAck.clear(); assert(!parseAckPacket(ack, sizeof(ack), a));
  expectedAck.expect(8, UINT32_MAX - 249, 500); clockMs = 249;
  assert(parseAckPacket(ack, sizeof(ack), a));
  buildAckPacket(ack, nodeId, 16, -4000.f, 99.f);
  expectedAck.expect(16, clockMs, 500); assert(parseAckPacket(ack, sizeof(ack), a));
  assert(a.rssiDbm10 == INT16_MIN && a.snrQuarterDb == INT8_MAX);
  buildAckPacket(ack, nodeId, 24, 4000.f, -99.f);
  expectedAck.expect(24, clockMs, 500); assert(parseAckPacket(ack, sizeof(ack), a));
  assert(a.rssiDbm10 == INT16_MAX && a.snrQuarterDb == INT8_MIN);
  std::cout << "PASS actual ACK own-ID/expected-sequence checks, expiry/replay/wrap and signed quarter-dB SNR\n";
}
void test_actual_receive_dispatch_isolates_position_rf_and_records_each_event() {
  reset(); pair(); txSeq = 1;
  uint8_t data[32]{}; const size_t n = buildDataPacket(data);
  receivedPacketRssi = -92.5f; receivedPacketSnr = 6.25f;
  assert(!acceptRadioPacket(data, n, clockMs));  // seq1 accepted, no ACK scheduled
  assert(havePkt && rxDataCount == 1 && lastData.seq == 1);
  assert(lastRssi == -92.5f && lastSnr == 6.25f && lastRxMs == 1200);
  assert(rssiRingCount == 1 && packetEvents.size() == 1);
  const DecodedData original = lastData;
  const uint32_t originalEstimate = lastEstimatedSourceMs;
  auto unchanged = [&]() {
    assert(havePkt && rxDataCount == 1 && lastRxMs == 1200 && lastData.seq == original.seq);
    assert(lastData.lat == original.lat && lastData.lon == original.lon && lastData.fix == original.fix);
    assert(lastData.speedCmS == original.speedCmS && lastData.courseDeg10 == original.courseDeg10);
    assert(lastData.age10ms == original.age10ms && lastData.hdop10 == original.hdop10);
    assert(lastRssi == -92.5f && lastSnr == 6.25f);
    assert(rssiRingCount == 1 && rssiRingIdx == 1 && pktsThisWindow == 1);
    assert(rssiRing[0] == -92.5f && snrRing[0] == 6.25f);
    assert(lastEstimatedSourceMs == originalEstimate && sourceEpochUpdates == 1);
    assert(lastDataIntervalMs == 0 && maxDataIntervalMs == 0 && ackTransmitter.starts == 0);
  };
  auto receiveOther = [&](const uint8_t *wire, size_t length, uint32_t when,
                          float rssi, float snr, packet_diagnostics::Kind kind) {
    clockMs = when; receivedPacketRssi = rssi; receivedPacketSnr = snr;
    assert(!acceptRadioPacket(wire, length, when)); unchanged();
    const auto &event = packetEvents.at(packetEvents.size() - 1);
    assert(event.kind == kind && event.ms == when && event.length == length);
    assert(event.rssiDbm10 == std::lround(rssi * 10));
    assert(event.snrQuarterDb == std::lround(snr * 4));
    assert(event.rawLength == std::min(length, sizeof(event.raw)));
    assert(std::memcmp(event.raw, wire, event.rawLength) == 0);
  };
  using packet_diagnostics::Kind;
  uint8_t other[32]{}; std::memcpy(other, data, n);
  protocol::putU16(other + 2, 0x1234);
  receiveOther(other, n, 1300, -30.0f, 12.0f, Kind::Binding);
  assert(rejectedBinding == 1 && packetEvents.at(1).clientId == 0x1234);
  std::memcpy(other, data, n); protocol::putI24(other + 6, 1000000);
  receiveOther(other, n, 1400, -55.0f, -4.5f, Kind::Sequence);
  assert(rejectedGpsSequence == 1 && packetEvents.at(2).seq == original.seq);
  uint8_t telemetry[TELEMETRY_PACKET_LEN]; const size_t tn = buildTelemetryPacket(telemetry);
  receiveOther(telemetry, tn, 1500, -78.0f, 3.5f, Kind::Telemetry);
  assert(haveTelemetry && rxTelemetryCount == 1 && lastTelemetryRxMs == 1500);
  assert(lastTelemetry.satellites == 9 && lastTelemetry.batteryMv == cachedBatteryMv);
  uint8_t diagnostic[DIAGNOSTIC_PACKET_LEN]; const size_t dn = buildDiagnosticPacket(diagnostic);
  receiveOther(diagnostic, dn, 1550, -45.0f, 9.0f, Kind::Diagnostic);
  assert(haveClientDiagnostic && rxDiagnosticCount == 1 && lastClientDiagnosticMs == 1550);
  std::memcpy(other, data, n); other[1] = 0x31;
  receiveOther(other, n, 1600, -40.0f, 8.0f, Kind::Format);
  std::memcpy(other, data, n); other[n] = 0xAA;
  receiveOther(other, n + 1, 1650, -65.0f, -2.0f, Kind::Length);
  assert(rejectedFormat == 1 && rejectedLength == 1 && rxDropCount == 3);
  // Only the next accepted DATA can replace client pose, radio quality or age.
  clockMs = 1700; const size_t nextLength = buildDataPacket(data);
  clockMs = 1750; receivedPacketRssi = -101.25f; receivedPacketSnr = -8.5f;
  assert(!acceptRadioPacket(data, nextLength, clockMs));
  assert(rxDataCount == 2 && lastData.seq == 2 && lastRxMs == 1750);
  assert(lastRssi == -101.25f && lastSnr == -8.5f && rssiRingCount == 2);
  assert(lastDataIntervalMs == 550 && maxDataIntervalMs == 550);
  // An ACK uses the newly accepted DATA measurement, never an earlier TEL or
  // rejected strong packet. The stub only captures the actual encoded ACK.
  clockMs = 2000; txSeq = 8; buildDataPacket(data);
  receivedPacketRssi = -80.0f; receivedPacketSnr = 5.0f;
  assert(acceptRadioPacket(data, DATA_PACKET_LEN, clockMs));
  assert(ackTransmitter.starts == 1 && !serverRxReady);
  assert(ackTransmitter.header.clientId == nodeId && ackTransmitter.payload.ackSeq == 8);
  assert(ackTransmitter.payload.rssiDbm10 == -800 && ackTransmitter.payload.snrQuarterDb == 20);
  std::cout << "PASS actual receive dispatcher isolates DATA pose/RSSI/time from rejects/TEL/DIAG while events retain per-packet RF\n";
}
int main() {
  test_actual_gnss_to_wire_to_server();
  test_half_second_transmission_does_not_refresh_gnss_age();
  test_speed_threshold_unknown_overflow_and_direction();
  test_hdop_rounds_up_and_epoch_mismatch_does_not_reuse_quality_or_velocity();
  test_no_fix_checksum_backlog_and_coordinate_range();
  test_independent_data_sequence_survives_telemetry_and_wrap();
  test_actual_diagnostic_counter_saturation_flags_and_separate_sequence();
  test_bound_server_and_all_actual_parsers_reject_bad_lengths();
  test_ack_client_id_sequence_window_replay_and_snr();
  test_actual_receive_dispatch_isolates_position_rf_and_records_each_event();
}
'''

with tempfile.TemporaryDirectory(prefix="shore-packet-backend-") as work:
    work = Path(work)
    unit = work / "packet_backend.cpp"
    binary = work / "packet_backend"
    unit.write_text(cpp)
    result = subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
        "-I", str(ROOT / "include"), str(unit), "-o", str(binary),
    ], capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(result.stderr)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    print(result.stdout, end="")
    if result.returncode:
        raise SystemExit(result.stderr or f"Packet integration exited {result.returncode}")
