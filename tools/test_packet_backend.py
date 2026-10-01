"""Host integration of the actual Client/Station v5 packet path, without hardware.

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
#include <vector>
#include "loop_metrics.h"
#include "protocol.h"
#include "gnss_snapshot.h"
#include "gnss_diagnostics.h"
#include "gnss_rate.h"
#include "station_position.h"
#include "lora_schedule.h"
#include "client_cadence.h"
#include "tracking_policy.h"
#include "command_freshness.h"
#include "packet_diagnostics.h"
#include "packet_rate.h"
#include "client_control.h"
using std::isfinite;
#define F(x) x
struct LogStub {template<class T> void print(T) {} template<class T> void println(T) {}} Log;
template<class T> T constrain(T value, T low, T high) {
  return std::min(std::max(value, low), high);
}
uint32_t clockMs = 0;
uint32_t millis() { return clockMs; }
uint16_t nodeId = 0xE91C, gpsClientId = 0xE91C;
uint16_t txSeq = 0, telemetrySeq = 0, diagnosticSeq = 0;
uint16_t gnssDiagnosticSeq = 0; uint8_t gnssDiagnosticPage = 0;
gnss_diagnostics::Report outgoingGnssDiagnostic;
uint32_t gpsBacklogDrops = 0, clientTxErrors = 0, dataSkippedSlots = 0;
uint16_t cachedBatteryMv = 4150;
int16_t cachedTempC10 = 243;
uint8_t cachedHumidityPct = 68;
gnss_snapshot::Collector gnssCollector;
gnss_rate::Monitor gpsRate;
station_position::Average stationAverage;
'''
for marker in ["struct DecodedData {", "struct DecodedTelemetry {"]:
    cpp += block(marker) + ";\n"
for name in ["SEND_INTERVAL_MS", "GPS_FIX_MAX_AGE_MS", "RSSI_WINDOW",
             "TELEMETRY_SLOT_GUARD_MS", "TELEMETRY_INTERVAL_MS", "LINK_TIMEOUT_MS"]:
    match = re.search(r"constexpr[^;\n]*\b" + name + r"\b[^;]*;", SOURCE)
    if not match:
        raise ValueError(f"Missing packet timing constant {name}")
    cpp += match.group() + "\n"
cpp += r'''
DecodedData lastData{};
DecodedTelemetry lastTelemetry{};
DiagnosticPayload lastClientDiagnostic{};
gnss_diagnostics::Latest clientGnssDiagnostic;
uint32_t rxGnssDiagnosticCount = 0;
bool havePkt = false, haveTelemetry = false, haveClientDiagnostic = false;
uint32_t lastRxMs = 0, lastTelemetryRxMs = 0, lastClientDiagnosticMs = 0;
float lastRssi = 0, lastSnr = 0, receivedPacketRssi = 0, receivedPacketSnr = 0;
float rssiRing[RSSI_WINDOW]{}, snrRing[RSSI_WINDOW]{};
size_t rssiRingIdx = 0, rssiRingCount = 0;
uint32_t rxDataCount = 0, rxTelemetryCount = 0, rxDiagnosticCount = 0;
packet_rate::Window10s loraDataRate;
uint32_t rxDropCount = 0, rxWinDrop = 0, rxWinTelem = 0, rxWinData = 0;
uint32_t rejectedLength = 0, rejectedFormat = 0, rejectedBinding = 0, rejectedGpsSequence = 0;
uint32_t sequenceMissing = 0, rxWinMissing = 0, sequenceResyncs = 0;
uint32_t lastDataIntervalMs = 0, maxDataIntervalMs = 0, lastSourceEpochIntervalMs = 0;
uint32_t sourceEpochUpdates = 0, lastEstimatedSourceMs = 0;
uint32_t invalidFixPackets = 0, invalidVelocityPackets = 0, pktsThisWindow = 0, pktWindowStartMs = 0;
uint32_t dataAirtimeMs = 330;
float cachedPktRate = 0, rxWinRssiMin = 0, rxWinRssiMax = 0, rxWinSnrMin = 0, rxWinSnrMax = 0;
double rxWinRssiSum = 0, rxWinSnrSum = 0;
bool haveSourceEstimate = false, rxWinHaveSeq = false, stationRxReady = true;
uint16_t rxWinFirstSeq = 0, rxWinLastSeq = 0;
int clientHumBaselinePct = -1;
command_freshness::RadioSequence gpsSequence;
packet_diagnostics::Ring<64> packetEvents;
// New control/probe path is exercised with the actual radio lifecycle in
// test_station_downlink.py; this fixture retains legacy DATA/TELEMETRY scope.
namespace station_extensions {
  struct {struct {uint32_t boot=0;}status;bool have=false;}link;
  bool haveRf = false; uint32_t lastRfMs = 0;
  bool accept(const uint8_t *raw,size_t n,uint32_t) {
    PacketHeader h{};client_control::Status status{};
    if(!client_control::decodeStatus(raw,n,h,status))return false;
    link.status.boot=status.boot;link.have=true;return true;
  }
}
namespace sd_log {
  unsigned calls = 0;
  void packet(const packet_diagnostics::Event &e, const uint8_t *raw, size_t length) {
    assert(e.id == packetEvents.total());
    assert(!raw || e.rawLength == std::min(length, sizeof(e.raw)));
    ++calls;
  }
}
'''
# The enclosing ROLE_CLIENT/ROLE_STATION directives are outside these function
# bodies, so the real paired endpoints can coexist in a single host executable.
for marker in [
    "static bool isClientAllowed(",
    "static bool parseDataPacket(",
    "static bool parseTelemetryPacket(",
    "static bool gpsFixFresh(",
    "static bool clientFixUsable(",
    "static size_t buildDataPacket(",
    "static size_t buildTelemetryPacket(",
    "static size_t buildDiagnosticPacket(",
    "static size_t buildGnssDiagnosticPacket(",
    "static void recordPacketEvent(",
    "static void acceptRadioPacket(",
    "static uint32_t boundRfAgeMs(",
    "static uint32_t clientSampleAgeMs(",
    "static bool clientFixFresh(",
]:
    cpp += block(marker) + "\n"

# Extract the actual RF display timeout from buildTrackJson; the test below
# verifies its boundary separately from the DATA and tracking freshness gates.
rf_timeout = re.search(r'js\s*\+=\s*rfAge\s*<\s*(\d+)\s*\?', block("static String buildTrackJson("))
if not rf_timeout:
    raise ValueError("Missing RF-alive timeout in buildTrackJson")
cpp += f"constexpr uint32_t kUiRfAliveMs = {rf_timeout.group(1)};\n"

cpp += r'''
uint32_t nextTelemetryMs = 0, nextDiagnosticMs = 0, nextGnssDiagnosticMs = 0;
uint32_t lastSendMs = 0, diagnosticAirtimeMs = 330, gnssDiagnosticAirtimeMs = 494, telemetryAirtimeMs = 289;
bool clientRadioReady = true, haveDataSent = false, clientSendingData = false;
uint32_t diagnosticTxCount = 0, nextClientTxMs = 0, nextClientExtraMs = 0;
bool clientDataDeferred = false;
client_cadence::Scheduler clientCadence;
uint8_t clientTxBuffer[MAX_PACKET_LEN];
uint8_t clientLogTxLength=0; uint32_t clientLogTxStarted=0;
void serviceClientSd(const gnss_snapshot::Snapshot&) {}
struct ClientTxStub {
  struct Tx { uint32_t at, finish; uint8_t type; uint16_t seq; uint8_t page; std::vector<uint8_t> wire; };
  std::vector<Tx> sent;
  bool running = false;
  uint32_t end = 0;
  bool active() const { return running; }
  int start(const uint8_t *buf, size_t n, uint32_t now, uint32_t) {
    assert(!running); PacketHeader h{}; assert(protocol::decodeHeader(buf, n, h));
    end = now + (h.msgType == MSG_GNSS_DIAGNOSTIC ? gnssDiagnosticAirtimeMs : h.msgType == MSG_TELEMETRY ? telemetryAirtimeMs : dataAirtimeMs); running = true;
    sent.push_back({now,end,h.msgType,h.seq,buf[6],std::vector<uint8_t>(buf,buf+n)}); return 1;
  }
} clientTransmitter;
void handleClientTxResult(int event) {
  if (event == 2) { nextClientTxMs=millis()+TELEMETRY_SLOT_GUARD_MS; clientRadioReady = true; if (clientSendingData) haveDataSent = true; }
}
void serviceClientRadio() {
  if (clientTransmitter.running && millis() >= clientTransmitter.end) {
    clientTransmitter.running = false; handleClientTxResult(2);
  }
}
void serviceGps() {} // Scheduler fixture; NMEA/real service gaps are tested separately.
'''
cpp += block("static void sendClientDiagnostic(") + "\n"
cpp += block("static void serviceClientTransmit(") + "\n"

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
  clockMs = 1000; txSeq = telemetrySeq = diagnosticSeq = 0; station_extensions::link={};
  gnssDiagnosticSeq = 0; gnssDiagnosticPage = 0; rxGnssDiagnosticCount = 0;
  clientGnssDiagnostic = gnss_diagnostics::Latest{};
  havePkt = haveTelemetry = haveClientDiagnostic = haveSourceEstimate = rxWinHaveSeq = false;
  lastData = DecodedData{}; lastTelemetry = DecodedTelemetry{};
  lastRxMs = lastTelemetryRxMs = lastClientDiagnosticMs = 0;
  rxDataCount = rxTelemetryCount = rxDiagnosticCount = rxDropCount = 0;
  loraDataRate = packet_rate::Window10s{};
  rxWinDrop = rxWinTelem = rxWinData = rejectedLength = rejectedFormat = rejectedBinding = rejectedGpsSequence = 0;
  sequenceMissing = rxWinMissing = sequenceResyncs = 0;
  lastDataIntervalMs = maxDataIntervalMs = lastSourceEpochIntervalMs = sourceEpochUpdates = lastEstimatedSourceMs = 0;
  invalidFixPackets = invalidVelocityPackets = pktsThisWindow = pktWindowStartMs = 0;
  rssiRingIdx = rssiRingCount = 0; lastRssi = lastSnr = 0;
  gpsSequence = command_freshness::RadioSequence{}; packetEvents = packet_diagnostics::Ring<64>{};
  stationRxReady = true; diagnosticTxCount = 0; clientDataDeferred = false;
  clientCadence.reset(clockMs); clientTransmitter = ClientTxStub{};
  nextClientTxMs=nextClientExtraMs=clockMs;gpsRate={};
  clientRadioReady=true; haveDataSent=false; clientSendingData=false;
  nextTelemetryMs=nextDiagnosticMs=nextGnssDiagnosticMs=clockMs;

  clientHumBaselinePct = -1;
  gpsBacklogDrops = clientTxErrors = dataSkippedSlots = 0;
  nodeId = gpsClientId = 0xE91C;
  cachedBatteryMv = 4150; cachedTempC10 = 243; cachedHumidityPct = 68;
  gnssCollector = gnss_snapshot::Collector{};
}
void pair(const char *speed = "9.72", const char *course = "90.0",
          const char *sats = "09", const char *hdop = "1.50") {
  feed(rmc("120000.000", speed, course), 1000);
  feed(gga("120000.000", sats, hdop), 1100);
  clockMs = 1200;
}
DecodedData roundTripData() {
 uint8_t wire[DATA_PACKET_LEN+1];memset(wire,0xa5,sizeof(wire));
 auto n=buildDataPacket(wire);assert(n==18&&wire[18]==0xa5&&wire[1]==0x51);
 DecodedData d{};assert(parseDataPacket(wire,n,d));return d;
}
void test_codec_dispatch_and_weak_quality() {
 reset();pair();auto d=roundTripData();
 assert(d.fix&&d.velocityValid&&d.speedCmS==500&&d.courseDeg10==900);
 assert(std::abs(d.lat-24.1234557)<1e-10&&std::abs(d.lon-121.1234557)<1e-10);
 assert(d.satellites==8&&d.hdop10==15);
 uint8_t buf[MAX_PACKET_LEN];auto n=buildDataPacket(buf);acceptRadioPacket(buf,n,1300);
 assert(rxDataCount==1&&lastData.seq==d.seq+1);auto seq=lastData.seq;
 acceptRadioPacket(buf,n,1400);assert(rejectedGpsSequence==1&&lastRxMs==1300);
 buf[1]=0x41;acceptRadioPacket(buf,n,1450);assert(rejectedFormat==1&&lastRxMs==1300);
 nodeId=0xabcd;n=buildDataPacket(buf);acceptRadioPacket(buf,n,1500);assert(rejectedBinding==1&&lastData.seq==seq);
 nodeId=0xe91c;n=buildTelemetryPacket(buf);assert(n==11);acceptRadioPacket(buf,n,1600);
 assert(rxTelemetryCount==1&&lastRxMs==1300&&lastTelemetry.satellites==9);
 gpsBacklogDrops=80000;clientTxErrors=90000;n=buildDiagnosticPacket(buf);assert(n==17);
 acceptRadioPacket(buf,n,1700);assert(lastClientDiagnostic.backlogDrops==65535&&lastClientDiagnostic.txErrors==65535&&lastRxMs==1300);
 n=buildGnssDiagnosticPacket(buf);assert(n==36);acceptRadioPacket(buf,n,1800);
 assert(clientGnssDiagnostic.received()&&rxGnssDiagnosticCount==1&&lastRxMs==1300);
 assert(std::abs(loraDataRate.fps(1800)-0.1f)<0.0001f); // Only accepted DATA, not rejects/TEL/DIAG.
 assert(packetEvents.at(packetEvents.size()-1).rawLength==36);
 txSeq=0;n=buildDataPacket(buf);acceptRadioPacket(buf,n,4300);assert(lastData.seq==0&&sequenceResyncs==1);
 reset();pair("9.72","90.0","05","8.0");d=roundTripData();
 assert(d.fix&&d.velocityValid&&d.hdop10==80&&d.satelliteClass==1);
 reset();pair("0.10","90.0");d=roundTripData();assert(d.fix&&!d.velocityValid&&d.speedCmS==10);
 reset();pair("99.0","90.0");d=roundTripData();assert(d.fix&&!d.velocityValid&&d.speedCmS==UINT16_MAX);
 std::cout<<"PASS real v5 DATA18/TEL11/DIAG17/GNSS36, E7 precision, weak-quality raw vector, dispatch, rejection, sequence recovery\n";
}
void test_latest_and_observed_age() {
 reset();pair();gnss_snapshot::Snapshot s;
 clockMs=1700;feed(rmc("120001.000"),1700);assert(gnssCollector.sample(clockMs,s));
 assert(s.epochMsOfDay==43201000&&!s.haveGga&&std::isnan(s.hdop));
 feed(gga("120001.000"),1720);clockMs=1800;
 auto d=roundTripData();assert(d.fix);
 // Shift UART arrival 600 ms relative to the old mapping. Accept advancing fixes.
 reset();pair();
 for(int i=1;i<=5;++i){char utc[20];snprintf(utc,sizeof(utc),"12000%d.000",i);
  feed(rmc(utc),1000+i*1000+600);feed(gga(utc),1020+i*1000+600);
  gnssCollector.sample(clockMs,s);assert(s.sourceAgeMs>=600&&s.arrivalAgeMs==20&&clientFixUsable(s));}
 const auto arrival=clockMs-20;feed(rmc("120005.000"),clockMs+500);feed(gga("120005.000"),clockMs+20);
 clockMs=arrival+2000;assert(!roundTripData().fix); // repeats cannot renew local age
 gnssCollector.invalidate(clockMs);assert(!roundTripData().fix);
 std::cout<<"PASS new epoch only, missing counterpart stays unknown, shifted diagnostic age does not gate valid data, repeats/backlog do not revive position\n";
}
void test_bound_rf_freshness_is_separate_from_data() {
 static_assert(kUiRfAliveMs==90000&&LINK_TIMEOUT_MS==5000&&tracking_policy::kGpsFreshMs==2000,
               "RF status must not loosen DATA display or GPS tracking freshness");
 uint8_t wire[MAX_PACKET_LEN];
 // Each accepted low-rate packet independently establishes RF life before the
 // very first DATA fix. Binding/format rejects cannot renew that evidence.
 for(unsigned type=0;type<3;++type) {
  reset();assert(boundRfAgeMs()==UINT32_MAX&&!clientFixFresh());
  auto build=[&](){return type==0?buildTelemetryPacket(wire):type==1?buildDiagnosticPacket(wire):buildGnssDiagnosticPacket(wire);};
  clockMs=1000;auto n=build();acceptRadioPacket(wire,n,clockMs);
  assert(boundRfAgeMs()==0&&!havePkt&&lastRxMs==0&&!clientFixFresh());
  clockMs=4000;nodeId=0xabcd;n=build();acceptRadioPacket(wire,n,clockMs);
  assert(rejectedBinding==1&&boundRfAgeMs()==3000&&!havePkt);
  nodeId=0xe91c;n=build();wire[1]=0x41;clockMs=5000;acceptRadioPacket(wire,n,clockMs);
  assert(rejectedFormat==1&&boundRfAgeMs()==4000&&!havePkt);
  clockMs=1000+kUiRfAliveMs-1;assert(boundRfAgeMs()==89999&&boundRfAgeMs()<kUiRfAliveMs);
  clockMs=1000+kUiRfAliveMs;assert(boundRfAgeMs()==90000&&!(boundRfAgeMs()<kUiRfAliveMs));
 }
 // Newest accepted packet wins across all four message types, while a fresh
 // TEL does not revive the old DATA position or the original two-second gate.
 reset();pair();auto n=buildDataPacket(wire);clockMs=1300;acceptRadioPacket(wire,n,clockMs);
 assert(clientFixFresh()&&boundRfAgeMs()==0);
 clockMs=3299;assert(clientFixFresh()&&clientSampleAgeMs()==1999);
 clockMs=3300;n=buildTelemetryPacket(wire);acceptRadioPacket(wire,n,clockMs);
 assert(!clientFixFresh()&&clientSampleAgeMs()==2000&&boundRfAgeMs()==0);
 clockMs=5000;n=buildDiagnosticPacket(wire);acceptRadioPacket(wire,n,clockMs);
 clockMs=6000;n=buildGnssDiagnosticPacket(wire);acceptRadioPacket(wire,n,clockMs);
 clockMs=6300;assert(clientSampleAgeMs()==LINK_TIMEOUT_MS&&boundRfAgeMs()==300&&!clientFixFresh());
 clockMs=6301;assert(clientSampleAgeMs()>LINK_TIMEOUT_MS&&boundRfAgeMs()==301);
 // A duplicate DATA is a rejected sequence, not fresh RF evidence.
 reset();pair();n=buildDataPacket(wire);clockMs=1300;acceptRadioPacket(wire,n,clockMs);
 clockMs=1400;acceptRadioPacket(wire,n,clockMs);
 assert(rejectedGpsSequence==1&&boundRfAgeMs()==100);
 // All timestamps use unsigned elapsed time, including the GNSS report cache.
 reset();const uint32_t base=0xfffffff0U;
 clockMs=base;n=buildTelemetryPacket(wire);acceptRadioPacket(wire,n,clockMs);
 clockMs=base+8;n=buildDiagnosticPacket(wire);acceptRadioPacket(wire,n,clockMs);
 clockMs=base+32;n=buildGnssDiagnosticPacket(wire);acceptRadioPacket(wire,n,clockMs);
 clockMs=base+40;assert(boundRfAgeMs()==8);
 clockMs=base+32+kUiRfAliveMs;assert(boundRfAgeMs()==kUiRfAliveMs);
 reset();pair();n=buildDataPacket(wire);clockMs=base;acceptRadioPacket(wire,n,clockMs);
 clockMs=base+1999;assert(clientFixFresh()&&boundRfAgeMs()==1999);
 clockMs=base+2000;assert(!clientFixFresh()&&boundRfAgeMs()==2000);
 std::cout<<"PASS accepted DATA/TEL/DIAG/GNSS RF freshness, no-fix startup, bound/format/sequence rejection, 90s expiry, unchanged 5s DATA/2s GPS gates and millis wrap\n";
}
void test_scheduler_two_hz_latest_only_and_diagnostic_guard() {
 reset();uint32_t nextFeed=1000;size_t data=0,extras=0;uint32_t lastDataAt=0,maxGap=0;
 for(clockMs=1000;clockMs<136000;++clockMs){
  if(clockMs>=nextFeed){const uint32_t t=clockMs-1000;char utc[24];snprintf(utc,sizeof(utc),"12%02u%02u.%03u",t/60000,(t/1000)%60,t%1000);
   feed(rmc(utc),clockMs);feed(gga(utc),clockMs);nextFeed+=500;}
  serviceClientTransmit();
 }
 for(size_t i=0;i<clientTransmitter.sent.size();++i){const auto &tx=clientTransmitter.sent[i];
  if(i)assert(tx.at>=clientTransmitter.sent[i-1].finish+80);
  if(tx.type==MSG_DATA){DecodedData d;assert(parseDataPacket(tx.wire.data(),tx.wire.size(),d)&&d.fix);
   if(lastDataAt) maxGap=std::max(maxGap,tx.at-lastDataAt);
   lastDataAt=tx.at;++data;
  }else{++extras;if(tx.type==MSG_GNSS_DIAGNOSTIC){assert(tx.wire.size()==36&&tx.finish-tx.at==494);}}
 }
 assert(data>250&&data<=270&&extras==9&&maxGap<1200);
 std::cout<<"PASS 135s source2Hz simulation DATA="<<data<<" diagnostics="<<extras<<" max DATA interval="<<maxGap<<" ms, all TX guards >=80ms\n";
 reset();nextTelemetryMs=nextDiagnosticMs=nextGnssDiagnosticMs=100000;clientRadioReady=false;
 feed(rmc("120000.000"),1000);feed(gga("120000.000"),1000);serviceClientTransmit();
 feed(rmc("120000.500"),1500);feed(gga("120000.500"),1500);serviceClientTransmit();
 feed(rmc("120001.000"),2000);feed(gga("120001.000"),2000);clientRadioReady=true;serviceClientTransmit();
 assert(clientTransmitter.sent.size()==1&&dataSkippedSlots==1&&clientTransmitter.sent[0].seq==0);
 auto &tx=clientTransmitter.sent[0];DecodedData d;assert(parseDataPacket(tx.wire.data(),tx.wire.size(),d)&&d.fix);
 clockMs=2500;gnssCollector.invalidate(clockMs);serviceClientTransmit();
 clockMs=3000;serviceClientTransmit();clockMs=3500;serviceClientTransmit();
 assert(clientTransmitter.sent.size()==2);assert(parseDataPacket(clientTransmitter.sent[1].wire.data(),18,d)&&!d.fix);
 reset();for(clockMs=1000;clockMs<136000;++clockMs)serviceClientTransmit();
 assert(clientTransmitter.sent.size()==9);for(auto &t:clientTransmitter.sent)assert(t.type!=MSG_DATA);
 std::cout<<"PASS latest-only while radio busy, one invalid transition, no invalid heartbeat, independent low-rate diagnostics\n";
}
void test_ten_second_packet_rate() {
 packet_rate::Window10s rate;
 assert(rate.fps(0)==0);
 for(uint32_t t=0;t<10000;t+=500)rate.record(t);
 assert(rate.fps(9999)==2.0f);
 assert(std::abs(rate.fps(10000)-1.9f)<0.0001f);
 assert(rate.fps(19500)==0.0f);
 rate.record(20000);assert(std::abs(rate.fps(20000)-0.1f)<0.0001f);
 rate={};
 for(uint32_t n=0;n<400;n++)rate.record(0xfffff000u+n*500);
 assert(rate.fps(0xfffff000u+399*500)==2.0f);
 assert(rate.fps(0xfffff000u+419*500)==0.0f);
 std::cout<<"PASS exact rolling 10s DATA FPS: startup, boundary expiry, silence, recovery, sustained traffic and millis wrap\n";
}
void test_t096_boot_discards_old_live_source() {
 reset();pair();uint8_t wire[MAX_PACKET_LEN];auto n=buildDataPacket(wire);acceptRadioPacket(wire,n,1300);
 assert(havePkt&&rxDataCount==1);
 n=buildTelemetryPacket(wire);acceptRadioPacket(wire,n,1400);
 n=buildDiagnosticPacket(wire);acceptRadioPacket(wire,n,1500);
 n=buildGnssDiagnosticPacket(wire);acceptRadioPacket(wire,n,1600);
 assert(haveTelemetry&&haveClientDiagnostic&&clientGnssDiagnostic.received());
 client_control::Status state{10,client_control::State::Ready,22,1,0,0,0,0,false};
 PacketHeader h{nodeId,1,MSG_CLIENT_STATE};
 n=client_control::encodeStatus(wire,sizeof(wire),h,state);acceptRadioPacket(wire,n,1700);
 assert(havePkt); // first ever STATE provides no evidence that prior DATA belongs to another boot
 state.boot=11;n=client_control::encodeStatus(wire,sizeof(wire),h,state);acceptRadioPacket(wire,n,1800);
 assert(!havePkt&&!haveTelemetry&&!haveClientDiagnostic&&!clientGnssDiagnostic.received());
 assert(loraDataRate.fps(1800)==0&&rxDataCount==1&&rxTelemetryCount==1);
 txSeq=0;n=buildDataPacket(wire);acceptRadioPacket(wire,n,1900);
 assert(havePkt&&lastData.seq==0&&rxDataCount==2&&sequenceMissing==0);
 std::cout<<"PASS T096 new boot clears stale live source and accepts new DATA baseline without resetting receiver counters\n";
}
int main(){test_t096_boot_discards_old_live_source();test_ten_second_packet_rate();test_codec_dispatch_and_weak_quality();test_latest_and_observed_age();test_bound_rf_freshness_is_separate_from_data();test_scheduler_two_hz_latest_only_and_diagnostic_guard();}

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
