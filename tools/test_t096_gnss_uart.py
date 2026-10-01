#!/usr/bin/env python3
"""Compile the actual T096 GNSS power function against a small host mock."""

from __future__ import annotations

import os
import pathlib
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


def main() -> None:
    actual = extract_function(SOURCE.read_text(encoding="utf-8"), "void setGnssEnabled(bool enabled)")
    harness = f"""
#include <cstdint>
#include <stdexcept>

namespace t096_pins {{ constexpr int kGnssEnable = 6; }}
constexpr int OUTPUT = 1, LOW = 0, HIGH = 1;
int pinModeCalls = 0, digitalWriteCalls = 0, lastPin = -1, lastValue = -1;
void pinMode(int pin, int) {{ ++pinModeCalls; lastPin = pin; }}
void digitalWrite(int pin, int value) {{ ++digitalWriteCalls; lastPin = pin; lastValue = value; }}
uint32_t millis() {{ return 1234; }}

struct FakeCollector {{ int resetCalls = 0; uint32_t resetAt = 0; void reset(uint32_t now) {{ ++resetCalls; resetAt = now; }} }} gnss;
struct FakeCadence {{ int resetCalls = 0; uint32_t resetAt = 0; void reset(uint32_t now) {{ ++resetCalls; resetAt = now; }} }} cadence;
struct FakeUart {{
  bool begun = false; int beginCalls = 0, endCalls = 0; uint32_t baud = 0;
  explicit operator bool() const {{ return begun; }}
  void begin(uint32_t value) {{ begun = true; ++beginCalls; baud = value; }}
  void end() {{ if (!begun) throw std::runtime_error("end before begin"); begun = false; ++endCalls; }}
}} Serial2;

{actual}

int main() {{
  setGnssEnabled(false);  // startup disable must not call end()
  if (Serial2.endCalls || gnss.resetCalls || cadence.resetCalls || lastValue != HIGH) return 1;
  setGnssEnabled(true);
  if (!Serial2.begun || Serial2.beginCalls != 1 || Serial2.baud != 115200 || gnss.resetCalls != 1 || cadence.resetCalls != 1 || lastValue != LOW) return 2;
  setGnssEnabled(false);
  if (Serial2.begun || Serial2.endCalls != 1 || lastValue != HIGH) return 3;
  setGnssEnabled(false);  // repeated disable remains safe
  if (Serial2.endCalls != 1) return 4;
  setGnssEnabled(true);
  setGnssEnabled(false);
  if (Serial2.beginCalls != 2 || Serial2.endCalls != 2 || gnss.resetCalls != 2 || cadence.resetCalls != 2) return 5;
  return 0;
}}
"""
    with tempfile.TemporaryDirectory() as directory:
        cpp = pathlib.Path(directory) / "t096_gnss_uart.cpp"
        binary = pathlib.Path(directory) / "t096_gnss_uart"
        cpp.write_text(harness, encoding="utf-8")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", str(cpp), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("PASS: actual setGnssEnabled startup/enable/disable/repeat lifecycle")


if __name__ == "__main__":
    main()
