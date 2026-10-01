#!/usr/bin/env python3
"""Compile T096's actual async radio functions against a clocked FakeRadio."""

from __future__ import annotations

import pathlib
import re
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "t096_client.cpp"


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature + " {")
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated {signature}")


def source_dispatch(loop: str) -> str:
    match = re.search(r"serviceTransmit\s*\(\s*millis\s*\(\s*\)\s*\)\s*;\s*serviceReceive\s*\(\s*millis\s*\(\s*\)\s*\)\s*;", loop)
    assert match, "loop must service TX and RX with fresh millis() values"
    return match.group(0)


def main() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    receive = extract_function(source, "void startReceiveWindow()")
    transmit = extract_function(source, "void serviceTransmit(uint32_t now)")
    command = extract_function(source, "void serviceReceive(uint32_t now)")
    dispatch = source_dispatch(extract_function(source, "void loop()"))
    assert "airStartedMs = millis()" in receive, "RX deadline must start after FEM RX switching"
    cached_dispatch = re.sub(r"millis\s*\(\s*\)", "now", dispatch)
    harness = f"""
#include <cstddef>
#include <cstdint>
using std::size_t;
struct PacketHeader;
constexpr int16_t RADIOLIB_ERR_NONE = 0, RADIOLIB_ERR_PACKET_TOO_LONG = -1;
constexpr uint32_t RADIOLIB_SX126X_IRQ_RX_DONE = 2, RADIOLIB_SX126X_IRQ_TX_DONE = 1, RADIOLIB_SX126X_IRQ_TIMEOUT = 0x200;
constexpr uint32_t RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED=4, RADIOLIB_SX126X_IRQ_HEADER_VALID=16;
constexpr uint32_t RADIOLIB_SX126X_RX_TIMEOUT_INF=0, RADIOLIB_IRQ_RX_DEFAULT_FLAGS=0, RADIOLIB_IRQ_RX_DEFAULT_MASK=0, RADIOLIB_IRQ_PREAMBLE_DETECTED=2;
constexpr size_t MAX_PACKET_LEN = 64;
constexpr uint32_t kTxTimeoutMs = 3000;
uint32_t clockMs = 0; uint32_t millis() {{ return clockMs; }} void delay(unsigned) {{ ++clockMs; }}
namespace client_control {{
constexpr uint32_t kReceiveWindowMs = 800, kReceiveListenMs = 85;
enum class State {{ Ready, Tracking, Test, Storage }}; struct Command {{}};
inline bool decodeCommand(const uint8_t*, size_t, ::PacketHeader&, Command&) {{ return true; }}
}}
namespace t096_control {{
enum class CommandResult {{ Ignored, Applied, Duplicate }};
inline bool elapsed(uint32_t now, uint32_t then, uint32_t period) {{ return uint32_t(now - then) >= period; }}
}}
struct PacketHeader {{}};
struct FakeControl {{
  bool applied = false;
  t096_control::CommandResult apply(const PacketHeader&, const client_control::Command&, uint32_t) {{ applied = true; return t096_control::CommandResult::Applied; }}
  client_control::State state() const {{ return client_control::State::Ready; }}
  void notedState(uint32_t) {{}} void cancelStorage(uint32_t) {{}}
}} control;
template<class... T> void usbLog(const char*, T...) {{}}
struct FakeRadio {{
  uint32_t flags = 0; bool receiveStarted = false;
  int16_t startReceive(uint32_t,uint32_t,uint32_t) {{ receiveStarted = true; flags=0; return RADIOLIB_ERR_NONE; }}
  uint32_t getIrqFlags() {{ return flags; }} size_t getPacketLength() {{ return 12; }}
  int16_t readData(uint8_t*, size_t) {{ return RADIOLIB_ERR_NONE; }}
  void finishReceive() {{}} void finishTransmit() {{}}
}} radio;
enum class AirState : uint8_t {{ Idle, Sending, Receiving }};
AirState air = AirState::Idle;
bool radioIrq = false, confirmationPending = false, outOpensReceiveWindow = false, outIsState = false, sleepAfterStateTx = false;
bool rxPreamble=false; uint32_t txErrors=0;
uint8_t out[64] = {{}}; size_t outLength = 0; uint16_t rxErrors = 0; uint32_t airStartedMs = 0, txPackets = 0;
void femRx() {{ delay(1); }} void femOff() {{}} void setGnssEnabled(bool) {{}} void armSystemOff(uint32_t) {{}}
bool usbVbusPresent() {{ return false; }} void enterSystemOff() {{}}

{receive}
{command}
{transmit}

static void actualDispatch() {{ {dispatch} }}
static void cachedDispatch() {{ const uint32_t now = millis(); {cached_dispatch} }}
static void resetTxDone() {{
  air = AirState::Sending; radioIrq = true; radio.flags = RADIOLIB_SX126X_IRQ_TX_DONE;
  outOpensReceiveWindow = true; outIsState = false; outLength = 1; confirmationPending = false;
  radio.receiveStarted = false; control.applied = false; rxErrors = 0;
}}
int main() {{
  clockMs = 100; resetTxDone(); actualDispatch();
  if (air != AirState::Receiving || !radio.receiveStarted || airStartedMs != 101) return 1;
  radio.flags = RADIOLIB_SX126X_IRQ_RX_DONE; radioIrq = true; actualDispatch();
  if (!control.applied || !confirmationPending) return 2;
  clockMs=300; startReceiveWindow(); air=AirState::Receiving; radioIrq=false;
  clockMs=airStartedMs+84; serviceReceive(millis()); if(air!=AirState::Receiving)return 5;
  ++clockMs; serviceReceive(millis()); if(air!=AirState::Idle)return 6;
  startReceiveWindow(); radioIrq=false; radio.flags=RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED;
  clockMs=airStartedMs+85; serviceReceive(millis()); if(air!=AirState::Receiving)return 7;
  clockMs=airStartedMs+500; serviceReceive(millis()); if(air!=AirState::Receiving)return 8;
  clockMs=airStartedMs+800; serviceReceive(millis()); if(air!=AirState::Idle)return 9;
  clockMs = 200; resetTxDone(); cachedDispatch();
  if (air != AirState::Idle || !radio.receiveStarted) return 3;
  air = AirState::Receiving; radioIrq = false; airStartedMs = 0xFFFFFFF0u; clockMs = 0x00000311u;
  serviceReceive(millis());
  if (air != AirState::Idle) return 4;
  return 0;
}}
"""
    with tempfile.TemporaryDirectory() as directory:
        cpp = pathlib.Path(directory) / "t096_actual_radio.cpp"
        binary = pathlib.Path(directory) / "t096_actual_radio"
        cpp.write_text(harness, encoding="utf-8")
        subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("PASS: actual T096 TX-done/FEM-RX/command/wrap lifecycle")


if __name__ == "__main__":
    main()
