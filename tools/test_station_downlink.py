#!/usr/bin/env python3
"""Host lifecycle test for the actual Station downlink scheduler.

Extracts the state/radio portion of station_extensions.h and runs it against a
fake RadioLib radio.  No Arduino board or HTTP server is involved.
"""
from pathlib import Path
import re
import subprocess
import tempfile

source = Path("include/station_extensions.h").read_text()
start = source.index("namespace station_extensions {")
end = source.index("static String positionJson()", start)
actual = source[start:end]

cpp = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "protocol.h"
#include "client_control.h"
#include "station_client_link.h"
#include "phone_position.h"
#include "loop_metrics.h"

#define RADIOLIB_ERR_NONE 0
#define RADIOLIB_ERR_TX_TIMEOUT -6
#define RADIOLIB_SX126X_IRQ_TX_DONE 1
uint32_t nowMs = 0;
uint32_t millis() { return nowMs; }
uint16_t nodeId = 99;
volatile bool stationRadioIrq = false;
bool stationRxReady = true;
uint32_t nextStationRxRetryMs = 0;
float receivedPacketRssi = -71.5f, receivedPacketSnr = 7.25f;
static unsigned txFrontendCalls = 0;
static bool txFrontend = false;
void setStationRadioTransmit(bool tx) { ++txFrontendCalls; txFrontend = tx; }
bool isClientAllowed(uint16_t id) { return id == 5; }

struct FakeRadio {
  int16_t startResult = RADIOLIB_ERR_NONE, finishResult = RADIOLIB_ERR_NONE;
  uint32_t irqFlags = RADIOLIB_SX126X_IRQ_TX_DONE;
  unsigned standbyCalls = 0, startCalls = 0, finishCalls = 0, rxCalls = 0;
  uint8_t sent[CLIENT_CONTROL_PACKET_LEN]{};
  size_t sentLength = 0;
  int16_t standby() { ++standbyCalls; return RADIOLIB_ERR_NONE; }
  int16_t startTransmit(const uint8_t *b, size_t n) {
    ++startCalls; sentLength = n;
    for (size_t i = 0; i < n && i < sizeof(sent); ++i) sent[i] = b[i];
    return startResult;
  }
  uint32_t getIrqFlags() { return irqFlags; }
  int16_t finishTransmit() { ++finishCalls; return finishResult; }
  int16_t startReceive() { ++rxCalls; return RADIOLIB_ERR_NONE; }
} radio;

'''+actual+"\n} // namespace station_extensions\n"+r'''

static void reset() {
  station_extensions::link = station_client_link::Link{};
  station_extensions::txActive = station_extensions::rendezvous = false;
  station_extensions::nextSendMs = station_extensions::rendezvousMs = 0;
  stationRadioIrq = false; stationRxReady = true; nextStationRxRetryMs = 0;
  radio = FakeRadio{}; txFrontendCalls = 0; txFrontend = false; nowMs = 0;
}
static void state(uint32_t boot, uint16_t station = 0, uint16_t command = 0,
                  client_control::State value = client_control::State::Ready) {
  uint8_t b[CLIENT_STATE_PACKET_LEN]{};
  const PacketHeader h{5, 17, MSG_CLIENT_STATE};
  const client_control::Status s{boot, value, station, command, nowMs, 3900, 3, 0, false};
  assert(client_control::encodeStatus(b, sizeof(b), h, s) == sizeof(b));
  assert(station_extensions::accept(b, sizeof(b), nowMs));
}
static void requestStartAndRendezvous() {
  state(77);
  assert(station_extensions::link.request(client_control::Action::Start, nowMs, nodeId));
  nowMs = 10; state(77);  // recent STATE opens exactly one rendezvous window
}
int main() {
  reset();
  state(77);
  assert(station_extensions::link.request(client_control::Action::Start, nowMs, nodeId));
  nowMs = 1000; station_extensions::serviceRadio();
  assert(radio.startCalls == 0 && !station_extensions::txActive);  // no STATE window

  reset(); requestStartAndRendezvous();
  nowMs = 29; station_extensions::serviceRadio();
  assert(radio.startCalls == 0);
  nowMs = 30; station_extensions::serviceRadio();
  assert(radio.startCalls == 1 && radio.standbyCalls == 1 && station_extensions::txActive);
  assert(txFrontend && !stationRxReady && radio.sentLength == CLIENT_CONTROL_PACKET_LEN);
  PacketHeader h{}; client_control::Command c{};
  assert(client_control::decodeCommand(radio.sent, radio.sentLength, h, c));
  assert(c.boot == 77 && c.action == client_control::Action::Start && c.station == nodeId);
  stationRadioIrq = true; nowMs = 31; station_extensions::serviceRadio();
  assert(!station_extensions::txActive && !txFrontend && stationRxReady && radio.finishCalls == 1 && radio.rxCalls == 1);

  reset(); requestStartAndRendezvous(); nowMs = 131; station_extensions::serviceRadio();
  assert(radio.startCalls == 0 && !station_extensions::rendezvous);  // stale HTTP/window work is skipped

  reset(); requestStartAndRendezvous(); radio.startResult = -42; nowMs = 30;
  station_extensions::serviceRadio();
  assert(radio.startCalls == 1 && !station_extensions::txActive && !txFrontend && stationRxReady);
  assert(station_extensions::link.lastError == -42 && radio.rxCalls == 1);

  reset(); requestStartAndRendezvous(); nowMs = 30; station_extensions::serviceRadio();
  assert(station_extensions::txActive);
  nowMs = 1030; station_extensions::serviceRadio();
  assert(!station_extensions::txActive && !txFrontend && stationRxReady);
  assert(station_extensions::link.lastError == RADIOLIB_ERR_TX_TIMEOUT && radio.finishCalls == 1);
  // The service itself never advances time or waits; all guard decisions use nowMs.
  assert(nowMs == 1030);
  puts("PASS actual STATE rendezvous, 20ms guard, 120ms expiry, TX failure/timeout, and RX recovery");
}
'''

with tempfile.TemporaryDirectory(prefix="shore-station-downlink-") as directory:
    path = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    path.write_text(cpp)
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-I", "include", str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
