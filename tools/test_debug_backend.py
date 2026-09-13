"""Compile the actual debug JSON and byte-log GET handlers without hardware.

Run: python3 tools/test_debug_backend.py
The firmware builders and event ring are real. Arduino String, HTTP and clock
are narrow host substitutes; output is parsed as JSON and checked in Python.
"""
from pathlib import Path
import json
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get("SHORE_DEBUG_BACKEND_SOURCE", ROOT / "src/main.cpp")).read_text()


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
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include "firmware_version.h"
#include "protocol.h"
#include "packet_diagnostics.h"
#include "gnss_snapshot.h"
#include "tracking_policy.h"
#include "command_freshness.h"
#include "servo_profile.h"
#define F(x) x
#define ESP_ARDUINO_VERSION_MAJOR 3
struct String : std::string {
  using std::string::string;
  using std::string::operator=;
  using std::string::operator+=;
  String(const std::string &value) : std::string(value) {}
  template<class T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
  explicit String(T value) : std::string(std::to_string(value)) {}
  String(double value, unsigned char places) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(places) << value;
    assign(stream.str());
  }
};
uint32_t clockMs = 1000, controlBootId = 1234567;
uint32_t millis() { return clockMs; }
uint32_t micros() { return clockMs * 1000; }
const uint32_t randomValues[] = {101, 102, 201, 202, 0, 302, 401, 402};
size_t randomIndex = 0;
uint32_t esp_random() {
  assert(randomIndex < sizeof(randomValues) / sizeof(randomValues[0]));
  return randomValues[randomIndex++];
}
command_freshness::HttpGate commandGate;
bool servoPwmReady = false, attachSucceeds = false, centreSucceeds = false;
float servoAngleDeg = 90, servoTargetDeg = 90;
bool ledcAttach(int, uint32_t, uint8_t) { return attachSucceeds; }
bool setServoAngle(float angle) {
  if (!centreSucceeds) return false;
  servoAngleDeg = angle; return true;
}
struct ServoInitStub { void initializeUs(double, uint32_t) {} } servoMotion;
struct LogStub {
  template<class T> void print(T) {}
  template<class T> void println(T) {}
  void println() {}
} Log;
uint16_t gpsClientId = 0xE91C;
uint32_t dataAirtimeMs = 165, ackAirtimeMs = 145, telemetryAirtimeMs = 145, diagnosticAirtimeMs = 165;
uint32_t gpsBacklogDrops = 0;
gnss_snapshot::Collector gnssCollector;
DiagnosticPayload lastClientDiagnostic{};
bool haveClientDiagnostic = false;
uint32_t lastClientDiagnosticMs = 0;
packet_diagnostics::Ring<64> packetEvents;
uint32_t rxDataCount = 0, rxTelemetryCount = 0, rxDiagnosticCount = 0, rxErrorCount = 0;
uint32_t rejectedLength = 0, rejectedFormat = 0, rejectedBinding = 0, rejectedGpsSequence = 0;
uint32_t sequenceMissing = 0, sequenceResyncs = 0, invalidFixPackets = 0, invalidVelocityPackets = 0;
uint32_t ackTxCount = 0, ackErrorCount = 0, ackSkippedCount = 0, radioRecoverCount = 0;
uint32_t lastDataIntervalMs = 0, maxDataIntervalMs = 0, sourceEpochUpdates = 0, lastSourceEpochIntervalMs = 0;
struct Http {
  std::map<std::string, String> args, headers;
  int code = 0;
  String contentType, body;
  bool hasArg(const char *key) const { return args.count(key); }
  String arg(const char *key) const { const auto it = args.find(key); return it == args.end() ? String{} : it->second; }
  void sendHeader(const char *key, const String &value) { headers[key] = value; }
  void send(int status, const char *mime, const String &value) { code = status; contentType = mime; body = value; }
  template<class Builder> auto measureBuild(Builder builder) { return builder(); }
  void reset() { args.clear(); headers.clear(); code = 0; contentType.clear(); body.clear(); }
} httpServer;
'''
for name in [
    "RF_FREQUENCY", "RF_BW", "RF_SF", "RF_CR", "SEND_INTERVAL_MS", "ACK_EVERY_N",
    "GPS_BAUD", "GPS_BACKLOG_GUARD_MS", "GPS_FIX_MAX_AGE_MS", "LOG_BUF_BYTES",
    "SERVO_PIN", "SERVO_PWM_HZ", "SERVO_PWM_RES_BITS",
]:
    match = re.search(r"constexpr[^;\n]*\b" + name + r"\b[^;]*;", SOURCE)
    if not match:
        raise ValueError(f"Missing debug constant {name}")
    cpp += match.group() + "\n"
cpp += "char logBuf[LOG_BUF_BYTES]{}; size_t logHead = 0; bool logWrapped = false; uint32_t logTotal = 0;\n"
for marker in [
    "static void logPush(", "static bool gpsFixFresh(",
    "static String buildDebugJson(", "static String buildLogText(",
    "static bool initServo()",
]:
    cpp += block(marker) + "\n"
for path, name in [("/api/debug", "debugGet"), ("/api/log", "logGet")]:
    match = re.search(r'httpServer.on\("' + re.escape(path) + r'",\s*HTTP_GET,\s*\[\]\(\)\s*{', SOURCE)
    if not match:
        raise ValueError(f"Missing GET route {path}")
    handler = block(match.group())
    cpp += "void " + name + "()" + handler[handler.index("{"):] + "\n"

cpp += r'''
std::string sentence(const std::string &body) {
  unsigned checksum = 0;
  for (unsigned char c : body) checksum ^= c;
  char suffix[8]; std::snprintf(suffix, sizeof(suffix), "*%02X\r\n", checksum);
  return "$" + body + suffix;
}
void feed(const std::string &body, uint32_t now) {
  clockMs = now;
  for (char c : sentence(body)) gnssCollector.feed(c, now);
}
std::string hexBytes(const std::string &bytes) {
  const char *hex = "0123456789abcdef";
  std::string out;
  for (unsigned char ch : bytes) { out += hex[ch >> 4]; out += hex[ch & 15]; }
  return out;
}
using Arguments = std::map<std::string, String>;
Arguments cursorArgs(uint32_t since, uint32_t boot = 1234567) {
  return {{"boot_id", String(boot)}, {"since", String(since)}};
}
uint32_t emitDebug(const std::string &label, const Arguments &args = {}) {
  httpServer.reset(); httpServer.args = args; debugGet();
  assert(httpServer.code == 200 && httpServer.contentType == "application/json");
  assert(httpServer.headers["Cache-Control"] == "no-store");
  std::cout << "JSON\t" << label << '\t' << httpServer.body << '\n';
  // Follow the actual returned cursor, so a premature jump to total would skip
  // pages and fail the Python sequence checks below.
  const std::string marker = "\"next_id\":";
  const auto pos = httpServer.body.find(marker);
  assert(pos != std::string::npos);
  return std::strtoul(httpServer.body.c_str() + pos + marker.size(), nullptr, 10);
}
void emitDebugError(const std::string &label, const Arguments &args) {
  httpServer.reset(); httpServer.args = args; debugGet();
  assert(httpServer.code == 400 && httpServer.contentType == "application/json");
  assert(httpServer.headers["Cache-Control"] == "no-store");
  std::cout << "ERROR\t" << label << '\t' << httpServer.body << '\n';
}
void appendLog(const std::string &text) { for (unsigned char ch : text) logPush(ch); }
void resetLog() { logHead = 0; logWrapped = false; logTotal = 0; }
void emitLog(const char *label, uint32_t from) {
  httpServer.reset(); httpServer.args["from"] = String(from); logGet();
  assert(httpServer.code == 200 && httpServer.contentType == "text/plain; charset=utf-8");
  assert(httpServer.headers["Cache-Control"] == "no-store");
  std::cout << "LOG\t" << label << '\t' << httpServer.headers["X-Log-Boot"] << '\t'
      << httpServer.headers["X-Log-Next"] << '\t' << httpServer.headers["X-Log-Dropped"]
      << '\t' << hexBytes(httpServer.body) << '\n';
}
int main() {
  emitDebug("empty");
  feed("GNRMC,120000.000,A,2400.000,N,12100.000,E,9.72,90,130926,,,A", 1000);
  feed("GNGGA,120000.000,2400.000,N,12100.000,E,1,09,1.5,10,M,0,M,,", 1100);
  feed("GNRMC,120000.500,A,2400.000,N,12100.000,E,9.72,90,130926,,,A", 1500);
  feed("GNGGA,120000.500,2400.000,N,12100.000,E,1,09,1.5,10,M,0,M,,", 1600);
  clockMs = 1800;
  // Populate the remote diagnostic through its actual codec rather than
  // copying its counters into a separate JSON fixture.
  const PacketHeader dh{gpsClientId, 2, MSG_DIAGNOSTIC};
  const DiagnosticPayload diag{500, 65535, 3, 7, 9, 0x3F};
  uint8_t diagWire[DIAGNOSTIC_PACKET_LEN]; PacketHeader decodedHeader{};
  assert(protocol::encodeDiagnostic(diagWire, sizeof(diagWire), dh, diag) == 17);
  assert(protocol::decodeDiagnostic(diagWire, sizeof(diagWire), decodedHeader, lastClientDiagnostic));
  haveClientDiagnostic = true; lastClientDiagnosticMs = 1750; rxDiagnosticCount = 1;
  rxDataCount = 70; rxTelemetryCount = 2; rejectedLength = 3; ackTxCount = 8;
  lastDataIntervalMs = 500; maxDataIntervalMs = 1000; sourceEpochUpdates = 35;
  lastSourceEpochIntervalMs = 1000;
  for (uint16_t i = 0; i < 70; ++i) {
    packet_diagnostics::Event event;
    event.kind = packet_diagnostics::Kind::Data; event.ms = 100 + i * 10;
    event.clientId = gpsClientId; event.seq = i; event.length = DATA_PACKET_LEN;
    event.sourceAgeMs = 250; event.rssiDbm10 = -987; event.snrQuarterDb = -53;
    event.flags = 3;
    PositionPayload position{24000000, 121000000, 50, 900, 3, true, true, 15, 25};
    const PacketHeader header{gpsClientId, i, MSG_DATA};
    event.rawLength = protocol::encodeData(event.raw, sizeof(event.raw), header, position);
    assert(event.rawLength == 17);
    packetEvents.push(event);
  }
  uint32_t cursor = emitDebug("full_0");
  for (unsigned page = 1; page < 8; ++page)
    cursor = emitDebug("full_" + std::to_string(page), cursorArgs(cursor));
  emitDebug("caught_up", cursorArgs(cursor));
  // A radio failure has no decoded position age or measured RSSI/SNR.
  packet_diagnostics::Event missing;
  missing.kind = packet_diagnostics::Kind::RadioError;
  missing.ms = 1801; missing.length = 32; missing.code = -7;
  missing.sourceAgeMs = UINT16_MAX;
  missing.rssiDbm10 = INT16_MIN; missing.snrQuarterDb = INT16_MIN;
  packetEvents.push(missing);
  cursor = emitDebug("missing_event", cursorArgs(cursor));
  clockMs = lastClientDiagnosticMs + 90000;
  emitDebug("stale", cursorArgs(cursor));
  emitDebug("cold_overwritten");
  emitDebug("overrun", cursorArgs(6));
  emitDebug("oldest_predecessor", cursorArgs(7));
  emitDebug("future", cursorArgs(72));
  emitDebug("different_boot", cursorArgs(71, 7654321));
  auto one = cursorArgs(69); one["limit"] = "1";
  cursor = emitDebug("limit_one", one);
  one["since"] = String(cursor); emitDebug("limit_one_last", one);
  auto eight = cursorArgs(63); eight["limit"] = "8";
  emitDebug("limit_eight", eight);
  emitDebug("uint32_max", cursorArgs(UINT32_MAX, UINT32_MAX));

  // Exercise route validation, not a second implementation of its parser.
  emitDebugError("boot_without_since", {{"boot_id", "1234567"}});
  emitDebugError("since_without_boot", {{"since", "0"}});
  const char *invalidUint32[] = {"", "-1", "+1", " 1", "1 ", "1x", "1.0", "0x10", "4294967296"};
  for (unsigned i = 0; i < sizeof(invalidUint32) / sizeof(invalidUint32[0]); ++i) {
    auto badBoot = cursorArgs(0); badBoot["boot_id"] = invalidUint32[i];
    emitDebugError("bad_boot_" + std::to_string(i), badBoot);
    auto badSince = cursorArgs(0); badSince["since"] = invalidUint32[i];
    emitDebugError("bad_since_" + std::to_string(i), badSince);
    emitDebugError("bad_limit_text_" + std::to_string(i), {{"limit", invalidUint32[i]}});
  }
  for (const char *limit : {"0", "9", "4294967295"})
    emitDebugError(std::string("bad_limit_range_") + limit, {{"limit", limit}});

  // A boot mismatch restarts even when the new boot's ring is empty.
  const auto savedRing = packetEvents;
  packetEvents = {}; ++controlBootId;
  emitDebug("empty_new_boot", cursorArgs(71));
  emitDebug("empty_caught_up", cursorArgs(0, controlBootId));
  --controlBootId;
  // New events arriving between pages must appear after the unfinished page.
  for (unsigned i = 0; i < 10; ++i) packetEvents.push(missing);
  cursor = emitDebug("arrival_first");
  packetEvents.push(missing); packetEvents.push(missing);
  cursor = emitDebug("arrival_second", cursorArgs(cursor));
  emitDebug("arrival_caught_up", cursorArgs(cursor));
  packetEvents = savedRing;
  resetLog(); emitLog("empty", 0);
  const std::string utf8 = "位置A\n";
  appendLog(utf8); emitLog("utf8", 0); emitLog("caught_up", utf8.size());
  appendLog("next\n"); emitLog("delta", utf8.size());
  resetLog(); appendLog(std::string(LOG_BUF_BYTES + 29, 'x'));
  emitLog("overwritten", 0); emitLog("tail", LOG_BUF_BYTES + 4);
  emitLog("future_cursor", LOG_BUF_BYTES + 1000);
  ++controlBootId; resetLog(); appendLog("boot\n");
  emitLog("reboot", LOG_BUF_BYTES + 29);
  ++controlBootId; resetLog(); emitLog("empty_reboot", 12345);
  // Seed the ring immediately before 32-bit wrap without writing 4 GiB.
  // The head matches the absolute modulo count, exactly as real logPush does.
  std::memset(logBuf, 'p', sizeof(logBuf));
  logWrapped = true; logTotal = UINT32_MAX - 7; logHead = logTotal % LOG_BUF_BYTES;
  const uint32_t beforeWrap = logTotal;
  appendLog("abcdefgh"); emitLog("cursor_wrap_exact", beforeWrap);
  appendLog("ijklmnopqrst"); emitLog("cursor_wrap_crossing", beforeWrap);
  emitLog("cursor_wrap_from_zero", 0); emitLog("cursor_wrap_caught_up", logTotal);
  emitLog("cursor_wrap_overwritten", logTotal - LOG_BUF_BYTES - 1);
  // Hardware failure cannot prevent a distinct diagnostic boot identity.
  resetLog(); attachSucceeds = false; centreSucceeds = false;
  assert(!initServo() && !servoPwmReady && controlBootId == 101 && commandGate.epoch() == 102);
  emitLog("attach_failure_boot", 0);
  attachSucceeds = true;
  assert(!initServo() && !servoPwmReady && controlBootId == 201 && commandGate.epoch() == 202);
  emitLog("centre_failure_boot", 0);
  attachSucceeds = false;
  assert(!initServo() && controlBootId == 1 && commandGate.epoch() == 302);
  emitLog("zero_random_boot", 0);
  attachSucceeds = true; centreSucceeds = true;
  assert(initServo() && servoPwmReady && controlBootId == 401 && commandGate.epoch() == 402);
  emitLog("successful_init_boot", 0);
  // Metadata scalars from the real headers let the Python assertions detect
  // later hard-coded JSON constants drifting away from the wire definitions.
  std::cout << "META\t" << unsigned(PROTO_VERSION) << '\t' << DATA_PACKET_LEN << '\t'
      << ACK_PACKET_LEN << '\t' << TELEMETRY_PACKET_LEN << '\t' << DIAGNOSTIC_PACKET_LEN << '\t'
      << LOG_BUF_BYTES << '\t' << SHORE_SPOTTER_VERSION << '\n';
}
'''

with tempfile.TemporaryDirectory(prefix="shore-debug-backend-") as work:
    work = Path(work)
    unit, binary = work / "debug_backend.cpp", work / "debug_backend"
    unit.write_text(cpp)
    result = subprocess.run([
        "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O2",
        "-I", str(ROOT / "include"), str(unit), "-o", str(binary),
    ], capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(result.stderr)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(result.stderr or f"Debug integration exited {result.returncode}")

snapshots, logs, metadata = {}, {}, None
debug_errors, debug_bytes = {}, {}


def reject_nonfinite(token):
    raise ValueError(f"Non-JSON numeric constant: {token}")


for line in result.stdout.splitlines():
    kind, _, rest = line.partition("\t")
    if kind == "JSON":
        label, body = rest.split("\t", 1)
        snapshots[label] = json.loads(body, parse_constant=reject_nonfinite)
        debug_bytes[label] = len(body.encode("utf-8"))
    elif kind == "ERROR":
        label, body = rest.split("\t", 1)
        debug_errors[label] = json.loads(body, parse_constant=reject_nonfinite)
    elif kind == "LOG":
        label, boot, next_cursor, dropped, raw = rest.split("\t")
        logs[label] = dict(boot=int(boot), next=int(next_cursor), dropped=dropped == "1", body=bytes.fromhex(raw))
    elif kind == "META":
        metadata = rest.split("\t")
    else:
        raise AssertionError(f"Unexpected host output: {line}")
assert metadata is not None
version, data_bytes, ack_bytes, telemetry_bytes, diagnostic_bytes, capacity = map(int, metadata[:6])
firmware_version = metadata[6]
empty = snapshots["empty"]
assert empty["schema_version"] == 2 and empty["protocol_version"] == version == 4
assert empty["firmware_version"] == firmware_version and empty["build"]
assert empty["boot_id"] == 1234567 and empty["clock_ms"] == 1000
config = empty["config"]
assert (config["data_bytes"], config["ack_bytes"], config["telemetry_bytes"], config["diagnostic_bytes"]) == (
    data_bytes, ack_bytes, telemetry_bytes, diagnostic_bytes)
assert config["send_interval_ms"] == 500 and config["ack_every_n"] == 8
assert config["rf_frequency_mhz"] == 923.2 and config["bw_khz"] == 125
assert config["sf"] == 9 and config["cr"] == 5
assert config["bound_client_id"] == 0xE91C and config["gnss_age_uncertainty_ms"] == 200
assert empty["gps"]["scope"] == "server_local"
assert empty["gps"]["source_age_ms"] is None and empty["gps"]["epoch_ms_of_day"] is None
assert empty["gps"]["measurement_clock_synchronized"] is False
assert empty["gps"]["fix"] is False and empty["events"]["items"] == []
assert empty["client_diagnostic"]["received"] is False and empty["client_diagnostic"]["fresh"] is False
for field in ["rx_age_ms", "epoch_interval_ms", "backlog_drops", "nmea_errors", "tx_errors", "skipped_slots", "status_bits"]:
    assert empty["client_diagnostic"][field] is None
print("PASS actual debug JSON parses strictly, agrees with wire/version metadata and emits unknowns as null")

full = snapshots["full_0"]
assert full["gps"]["fix"] is True and full["gps"]["have_rmc"] is True and full["gps"]["have_gga"] is True
assert full["gps"]["epochs"] == 2 and full["gps"]["last_epoch_interval_ms"] == 500
assert full["gps"]["rmc"] == 2 and full["gps"]["gga"] == 2
assert full["gps"]["epoch_ms_of_day"] == 43200500 and 300 <= full["gps"]["source_age_ms"] < 500
assert full["client_diagnostic"]["received"] is True and full["client_diagnostic"]["fresh"] is True
assert full["client_diagnostic"]["rx_age_ms"] == 50
assert full["client_diagnostic"]["backlog_drops"] == 65535 and full["client_diagnostic"]["epoch_interval_ms"] == 500
assert full["client_diagnostic"]["counter_encoding"] == "uint16_saturating_since_client_boot"
assert full["counters"]["rx_data"] == 70 and full["counters"]["rx_diagnostic"] == 1
assert full["counters"]["rejected_length"] == 3 and full["counters"]["ack_sent"] == 8
assert full["counters"]["inferred_source_interval_ms"] == 1000
assert full["events"]["capacity"] == 64 and full["events"]["total"] == 70 and full["events"]["overwritten"] == 6
items = [item for page in range(8) for item in snapshots[f"full_{page}"]["events"]["items"]]
assert len(items) == 64 and [item["id"] for item in items] == list(range(7, 71))
for index, item in enumerate(items, start=6):
    assert item["kind"] == "data" and item["seq"] == index and item["client_id"] == 0xE91C
    assert item["source_age_ms"] == 250 and item["flags"] == 3
    assert item["rssi_dbm"] == -98.7 and item["snr_db"] == -13.25
    raw = bytes.fromhex(item["raw_hex"])
    assert item["length"] == len(raw) == 17 and raw[:4] == bytes([0x53, 0x41, 0x1C, 0xE9])
    assert int.from_bytes(raw[4:6], "little") == index
missing = snapshots["missing_event"]["events"]
assert missing["overwritten"] == 7 and missing["total"] == 71 and len(missing["items"]) == 1
last = missing["items"][-1]
assert last["kind"] == "radio_error" and last["code"] == -7 and last["raw_hex"] == ""
assert last["source_age_ms"] is None and last["rssi_dbm"] is None and last["snr_db"] is None
assert snapshots["stale"]["client_diagnostic"]["fresh"] is False
assert snapshots["stale"]["client_diagnostic"]["rx_age_ms"] == 90000
assert snapshots["stale"]["gps"]["fix"] is False
assert len(full["limitations"]) >= 4
print("PASS real GNSS/DIAG snapshots, 64 events recovered through pages, raw hex and unknown signal values")


def assert_page(label, ids, next_id, more=False, dropped=False, reset=False):
    events = snapshots[label]["events"]
    assert [event["id"] for event in events["items"]] == list(ids), label
    assert events["next_id"] == next_id, label
    assert events["more"] is more and events["dropped"] is dropped and events["reset"] is reset, label


assert_page("empty", [], 0, reset=True)
for page in range(8):
    start = 7 + page * 8
    assert_page(f"full_{page}", range(start, start + 8), start + 7,
                more=page < 7, reset=page == 0)
assert_page("caught_up", [], 70)
assert_page("missing_event", [71], 71)
assert_page("stale", [], 71)
assert_page("cold_overwritten", range(8, 16), 15, more=True, reset=True)
assert_page("overrun", range(8, 16), 15, more=True, dropped=True, reset=True)
assert_page("oldest_predecessor", range(8, 16), 15, more=True)
assert_page("future", range(8, 16), 15, more=True, dropped=True, reset=True)
assert_page("different_boot", range(8, 16), 15, more=True, reset=True)
assert_page("uint32_max", range(8, 16), 15, more=True, reset=True)
assert_page("empty_new_boot", [], 0, reset=True)
assert_page("empty_caught_up", [], 0)
assert snapshots["empty_new_boot"]["boot_id"] == 1234568
assert_page("arrival_first", range(1, 9), 8, more=True, reset=True)
assert_page("arrival_second", range(9, 13), 12)
assert_page("arrival_caught_up", [], 12)
print("PASS actual debug GET cursor pagination, arrivals, overwrite/future recovery and boot reset")

assert_page("limit_one", [70], 70, more=True)
assert_page("limit_one_last", [71], 71)
assert_page("limit_eight", range(64, 72), 71)
assert len(debug_errors) == 32
assert all(isinstance(body.get("error"), str) and body["error"] for body in debug_errors.values())
assert all(snapshot["schema_version"] == 2 and len(snapshot["events"]["items"]) <= 8
           for snapshot in snapshots.values())
max_debug_bytes = max(debug_bytes.values())
assert max_debug_bytes < 4500, debug_bytes
print(f"PASS debug GET limit 1..8, 32 malformed argument cases; {len(snapshots)} responses <=8 events, "
      f"largest ordinary fixture JSON {max_debug_bytes} bytes (<4500)")

utf8 = "位置A\n".encode()
assert logs["empty"] == dict(boot=1234567, next=0, dropped=False, body=b"")
assert logs["utf8"] == dict(boot=1234567, next=len(utf8), dropped=False, body=utf8)
assert logs["caught_up"]["body"] == b"" and logs["caught_up"]["next"] == len(utf8)
assert logs["delta"]["body"] == b"next\n" and logs["delta"]["next"] == len(utf8) + 5
assert logs["overwritten"]["body"] == b"x" * capacity and logs["overwritten"]["dropped"] is True
assert logs["overwritten"]["next"] == capacity + 29
assert logs["tail"]["body"] == b"x" * 25 and logs["tail"]["dropped"] is False
assert logs["future_cursor"]["dropped"] is True and logs["future_cursor"]["body"] == b"x" * capacity
assert logs["reboot"] == dict(boot=1234568, next=5, dropped=True, body=b"boot\n")
assert logs["empty_reboot"] == dict(boot=1234569, next=0, dropped=False, body=b"")
assert logs["cursor_wrap_exact"] == dict(boot=1234569, next=0, dropped=False, body=b"abcdefgh")
assert logs["cursor_wrap_crossing"] == dict(boot=1234569, next=12, dropped=False, body=b"abcdefghijklmnopqrst")
assert logs["cursor_wrap_from_zero"] == dict(boot=1234569, next=12, dropped=False, body=b"ijklmnopqrst")
assert logs["cursor_wrap_caught_up"] == dict(boot=1234569, next=12, dropped=False, body=b"")
assert logs["cursor_wrap_overwritten"] == dict(
    boot=1234569, next=12, dropped=True, body=b"p" * (capacity - 20) + b"abcdefghijklmnopqrst")
print("PASS actual log GET UTF-8 byte cursors, overflow/future recovery, UINT32 wrap and reboot identity")
for name, boot in [("attach_failure_boot", 101), ("centre_failure_boot", 201),
                   ("zero_random_boot", 1), ("successful_init_boot", 401)]:
    assert logs[name] == dict(boot=boot, next=0, dropped=False, body=b"")
print("PASS actual initServo creates nonzero boot identity and control epoch before either PWM failure path")
