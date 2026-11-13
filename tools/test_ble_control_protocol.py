#!/usr/bin/env python3
"""Compile and exercise the shared BLE wire codec on the host."""
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]

SOURCE = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "ble_control_protocol.h"
using namespace ble_control;

int main() {
  assert(strcmp(kServiceUuid, "a87e0001-74e8-4da8-9b2a-46b710924010") == 0);
  assert(strcmp(kCommandUuid, "a87e0002-74e8-4da8-9b2a-46b710924010") == 0);
  assert(strcmp(kStatusUuid, "a87e0003-74e8-4da8-9b2a-46b710924010") == 0);
  assert(validChannel(0) && validChannel(1) && !validChannel(2));
  assert(frequencyMhz(0) == 923.2f && frequencyMhz(1) == 923.8f);
  assert(frequencyMhz(2) == 0.0f && strcmp(channelName(1), "B") == 0);

  Command c{Op::Test, 1, 0x1234, 0x5678, 0x12345678, 0x9abc, 0x87654321};
  uint8_t cb[kCommandSize] = {};
  assert(encodeCommand(cb, sizeof(cb) - 1, c) == 0);
  assert(encodeCommand(cb, sizeof(cb), c) == kCommandSize);
  const uint8_t expected[] = {0x53,1,4,1,0x34,0x12,0x78,0x56,
    0x78,0x56,0x34,0x12,0xbc,0x9a,0,0,0x21,0x43,0x65,0x87};
  assert(memcmp(cb, expected, sizeof(expected)) == 0);
  Command cd{};
  assert(decodeCommand(cb, sizeof(cb), cd));
  assert(cd.op == c.op && cd.channel == c.channel && cd.client == c.client &&
         cd.station == c.station && cd.boot == c.boot && cd.seq == c.seq &&
         cd.stationBoot == c.stationBoot);
  assert(!decodeCommand(cb, sizeof(cb)-1, cd));
  uint8_t extended[kCommandSize + 1] = {};
  memcpy(extended, cb, sizeof(cb));
  assert(!decodeCommand(extended, sizeof(extended), cd));
  cb[0] = 0; assert(!decodeCommand(cb, sizeof(cb), cd)); cb[0] = 0x53;
  cb[1] = 2; assert(!decodeCommand(cb, sizeof(cb), cd)); cb[1] = 1;
  cb[2] = 5; assert(!decodeCommand(cb, sizeof(cb), cd)); cb[2] = 4;
  cb[3] = 2; assert(!decodeCommand(cb, sizeof(cb), cd)); cb[3] = 1;
  cb[4] = cb[5] = 0; assert(!decodeCommand(cb, sizeof(cb), cd));
  cb[4] = 0x34; cb[5] = 0x12;
  memset(cb + 8, 0, 4); assert(!decodeCommand(cb, sizeof(cb), cd));
  cb[8] = 0x78; cb[9] = 0x56; cb[10] = 0x34; cb[11] = 0x12;
  memset(cb + 16, 0, 4); assert(!decodeCommand(cb, sizeof(cb), cd));
  cb[16] = 0x21; cb[17] = 0x43; cb[18] = 0x65; cb[19] = 0x87;
  Command bad = c;
  bad.op = Op(5); assert(encodeCommand(cb, sizeof(cb), bad) == 0);
  bad = c; bad.channel = 2; assert(encodeCommand(cb, sizeof(cb), bad) == 0);
  bad = c; bad.client = 0; assert(encodeCommand(cb, sizeof(cb), bad) == 0);
  bad = c; bad.station = 0xffff; assert(encodeCommand(cb, sizeof(cb), bad) == 0);
  bad = c; bad.boot = 0; assert(encodeCommand(cb, sizeof(cb), bad) == 0);
  bad = c; bad.stationBoot = 0; assert(encodeCommand(cb, sizeof(cb), bad) == 0);
  cb[14] = 1; assert(!decodeCommand(cb, sizeof(cb), cd)); cb[14] = 0;

  Status s{client_control::State::Test, 0, 0x1234, 0x5678, 0x12345678,
    0x9abc, 0xdef0, 0x12345678, 0xabcd, 7, Result::Rejected, -1234,
    kSf, 0x11223344, 0x87654321, 0x4567};
  uint8_t sb[kStatusSize] = {};
  assert(encodeStatus(sb, sizeof(sb)-1, s) == 0);
  assert(encodeStatus(sb, sizeof(sb), s) == kStatusSize);
  assert(sb[0] == 0x53 && sb[1] == 1 && sb[22] == 7 && sb[23] == 2);
  assert(sb[24] == 0x2e && sb[25] == 0xfb && sb[27] == 0 && sb[38] == 0 && sb[39] == 0);
  Status sd{};
  assert(decodeStatus(sb, sizeof(sb), sd));
  assert(sd.state == s.state && sd.channel == s.channel && sd.client == s.client &&
         sd.owner == s.owner && sd.boot == s.boot && sd.command == s.command &&
         sd.batteryMv == s.batteryMv && sd.txPackets == s.txPackets &&
         sd.rxErrors == s.rxErrors && sd.flags == s.flags && sd.result == s.result &&
         sd.error == s.error && sd.sf == s.sf && sd.uptime == s.uptime &&
         sd.stationBoot == s.stationBoot && sd.replySeq == s.replySeq);
  assert(!decodeStatus(sb, sizeof(sb)-1, sd));
  uint8_t sextra[kStatusSize + 1] = {};
  memcpy(sextra, sb, sizeof(sb)); assert(!decodeStatus(sextra, sizeof(sextra), sd));
  sb[0] = 0; assert(!decodeStatus(sb, sizeof(sb), sd)); sb[0] = 0x53;
  sb[1] = 2; assert(!decodeStatus(sb, sizeof(sb), sd)); sb[1] = 1;
  sb[2] = 4; assert(!decodeStatus(sb, sizeof(sb), sd)); sb[2] = 3;
  sb[23] = 3; assert(!decodeStatus(sb, sizeof(sb), sd)); sb[23] = 2;
  Status unclaimed = s; unclaimed.owner = 0; unclaimed.stationBoot = 0;
  assert(encodeStatus(sb, sizeof(sb), unclaimed) == kStatusSize);
  Status invalid = s; invalid.state = client_control::State::Storage;
  assert(encodeStatus(sb, sizeof(sb), invalid) == 0);
  invalid = s; invalid.channel = 2; assert(encodeStatus(sb, sizeof(sb), invalid) == 0);
  invalid = s; invalid.client = 0; assert(encodeStatus(sb, sizeof(sb), invalid) == 0);
  invalid = s; invalid.owner = 0xffff; assert(encodeStatus(sb, sizeof(sb), invalid) == 0);
  invalid = s; invalid.flags = 0x10; assert(encodeStatus(sb, sizeof(sb), invalid) == 0);
  invalid = s; invalid.result = Result(3); assert(encodeStatus(sb, sizeof(sb), invalid) == 0);
  invalid = s; invalid.sf = 10; assert(encodeStatus(sb, sizeof(sb), invalid) == 0);
  invalid = s; invalid.owner = 0; invalid.stationBoot = 0;
  assert(encodeStatus(sb, sizeof(sb), invalid) == kStatusSize);
  invalid = s; invalid.stationBoot = 0;
  assert(encodeStatus(sb, sizeof(sb), invalid) == 0);
  sb[27] = 1; assert(!decodeStatus(sb, sizeof(sb), sd)); sb[27] = 0;
  sb[38] = 1; assert(!decodeStatus(sb, sizeof(sb), sd)); sb[38] = 0;
  sb[22] = 0x80; assert(!decodeStatus(sb, sizeof(sb), sd));
  return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="ble-control-codec-") as temp:
        source = pathlib.Path(temp) / "test.cpp"
        binary = pathlib.Path(temp) / "test"
        source.write_text(SOURCE)
        subprocess.run(
            ["g++", "-std=c++14", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"), str(source), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
    print("BLE control wire codec: PASS")


if __name__ == "__main__":
    main()
