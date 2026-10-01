#!/usr/bin/env python3
"""Exercise the actual diagnostic USB exporter and C++ frame codec without hardware."""
import contextlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock
import zlib

from read_diagnostic_log import (Device, IncompleteCapture, SNAPSHOT, decode_frame,
                                 decode_payload, export_capture, main)

ROOT = Path(__file__).resolve().parents[1]


def fixtures(directory):
    snapshot = SNAPSHOT.pack(1, 31 | 64, 12, 2, 36001234, 30, 20, 20260923,
                             24.1234567, 121.7654321, 1.25, 120.5, 0.8,
                             4090, 0, 250000, 0, 55, 0, 2, 33, 40)
    snapshot_initializer = ','.join(str(byte) for byte in snapshot)
    source = Path(directory) / 'fixture.cpp'
    binary = Path(directory) / 'fixture'
    source.write_text(r'''
#include <iostream>
#include "diagnostic_store_codec.h"
using namespace diagnostic_store::codec;
void output(const uint8_t *frame) { std::cout.write(reinterpret_cast<const char *>(frame), kFrameBytes); }
int main() {
  uint8_t frame[kFrameBytes]; makeHeader(frame); output(frame);
  beginFrame(frame, 42, 0, 1000);
  const char boot[] = "{\"event\":\"boot\",\"reset_reason\":3}";
  const char nmea[] = "$GNRMC,100001.234,A,2407.40740,N,12145.92593,E*00\r\n";
  const uint8_t snapshot[] = {SNAPSHOT_INITIALIZER};
  if (!append(frame, 2, boot, sizeof(boot)-1, 1000) ||
      !append(frame, 1, nmea, sizeof(nmea)-1, 1020) ||
      !append(frame, 3, snapshot, sizeof(snapshot), 1030)) return 1;
  seal(frame); output(frame);
  beginFrame(frame, 42, 1, 2000);
  const char partial[] = "$GNGGA,partial";
  if (!append(frame, 1, partial, sizeof(partial)-1, 2000)) return 2;
  seal(frame); output(frame);
  beginFrame(frame, 43, 0, 1000);
  const char phase[] = "{\"phase\":0,\"reason\":\"new_boot\"}";
  if (!append(frame, 4, phase, sizeof(phase)-1, 1000)) return 3;
  seal(frame); output(frame);
}
'''.replace('SNAPSHOT_INITIALIZER', snapshot_initializer))
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'include'), str(source), '-o', str(binary)], check=True)
    raw = subprocess.check_output([str(binary)])
    assert len(raw) == 4 * 512
    return [raw[offset:offset + 512] for offset in range(0, len(raw), 512)]


def frame_line(index, raw, crc=None):
    crc = zlib.crc32(raw) if crc is None else crc
    return f'@DIAG FRAME {index} {crc:08x} {raw.hex()}\n'.encode()


class FakeSerial:
    def __init__(self, frames, transform=None, status=None):
        self.frames = frames
        self.transform = transform
        self.status = dict(used_frames=len(frames), capacity_frames=3072,
                           dropped=0, corrupt_frames=0, state='recording', error=0, usb_errors=0)
        self.status.update(status or {})
        self.data = b''
        self.commands = []
        self.reads = 0

    def write(self, data):
        command = data.decode('ascii').strip()
        self.commands.append(command)
        if command == 'DIAG STATUS':
            self.data += b'ordinary boot log\ntruncated log@DIAG STATUS ' + json.dumps(self.status).encode() + b'\n'
        elif command.startswith('DIAG READ '):
            _, _, start, count = command.split()
            start, count = int(start), int(count)
            self.reads += 1
            lines = [frame_line(index, self.frames[index]) for index in range(start, start + count)]
            if self.transform:
                lines = self.transform(start, count, self.reads, lines)
            self.data += f'@DIAG BEGIN {start} {count}\n'.encode() + b''.join(lines) + f'@DIAG END {start} {count}\n'.encode()
        elif command == 'DIAG ERASE CONFIRM':
            self.data += b'@DIAG ERASE OK\n'
        else:
            raise AssertionError(command)
        return len(data)

    def flush(self):
        pass

    def read_until(self, delimiter):
        at = self.data.find(delimiter)
        end = len(self.data) if at < 0 else at + len(delimiter)
        end = min(end, 17)  # Partial serial reads, including partial protocol lines.
        result, self.data = self.data[:end], self.data[end:]
        return result


def device(frames, **kwargs):
    result = Device.__new__(Device)
    result.serial = FakeSerial(frames, **kwargs)
    result.response_timeout = 0.02
    result.stats = {'usb_crc_errors': 0, 'frame_crc_errors': 0, 'protocol_errors': 0,
                    'retried_frames': 0, 'retry_requests': 0}
    return result


def reseal(raw, offset, value):
    value_raw = bytearray(raw)
    struct.pack_into('<I', value_raw, offset, value)
    struct.pack_into('<I', value_raw, 508, zlib.crc32(value_raw[:508]))
    return bytes(value_raw)


class DiagnosticUsbTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture_dir = tempfile.TemporaryDirectory()
        cls.frames = fixtures(cls.fixture_dir.name)

    @classmethod
    def tearDownClass(cls):
        cls.fixture_dir.cleanup()

    def setUp(self):
        self.output_dir = tempfile.TemporaryDirectory()
        self.prefix = Path(self.output_dir.name) / 'trip'

    def tearDown(self):
        self.output_dir.cleanup()

    def read_json(self, extension):
        return json.loads(Path(str(self.prefix) + extension).read_text())

    def test_cpp_codec_round_trip_and_export(self):
        obj = device(self.frames)
        result = export_capture(obj, self.prefix, batch_size=2)
        self.assertTrue(result['complete'])
        self.assertEqual(Path(str(self.prefix) + '.bin').read_bytes(), b''.join(self.frames))
        self.assertEqual(result['verified_frames'], 4)
        rows = [json.loads(line) for line in Path(str(self.prefix) + '.ndjson').read_text().splitlines()]
        self.assertEqual(len(rows), 6)
        snapshot = next(row for row in rows if row['kind'] == 'snapshot')
        self.assertEqual(snapshot['gnss_utc'], '2026-09-23T10:00:01.234Z')
        self.assertEqual(snapshot['utc_source'], 'gnss_rmc')
        self.assertEqual(snapshot['phase_name'], 'rf_on_sd_off')
        self.assertEqual(snapshot['boot_id'], 42)
        self.assertEqual(snapshot['boot_ms'], 1030)
        self.assertNotEqual(snapshot['gnss_utc'], snapshot['host_received_utc'])
        raw = next(row for row in rows if row['kind'] == 'raw_uart')
        self.assertTrue(bytes.fromhex(raw['raw_hex']).endswith(b'\r\n'))
        self.assertEqual(rows[-1]['boot_id'], 43)
        self.assertEqual(rows[-1]['frame_sequence'], 0)
        for suffix in ('.bin', '.ndjson', '.manifest.json'):
            self.assertEqual(os.stat(str(self.prefix) + suffix).st_mode & 0o777, 0o600)
        with self.assertRaises(FileExistsError):
            export_capture(obj, self.prefix)

    def test_missing_and_crc_bad_frames_are_retried_by_index(self):
        def glitch(start, count, attempt, lines):
            if attempt == 1:
                return [lines[0], frame_line(2, self.frames[2], crc=0), lines[3]]
            return lines
        obj = device(self.frames, transform=glitch)
        result = export_capture(obj, self.prefix)
        self.assertTrue(result['complete'])
        self.assertEqual(result['statistics']['usb_crc_errors'], 1)
        self.assertEqual(result['statistics']['retried_frames'], 2)
        self.assertIn('DIAG READ 1 1', obj.serial.commands)
        self.assertIn('DIAG READ 2 1', obj.serial.commands)
        self.assertEqual(Path(str(self.prefix) + '.bin').read_bytes(), b''.join(self.frames))

    def test_malformed_and_duplicate_protocol_lines_cannot_hide_gap(self):
        def glitch(start, count, attempt, lines):
            return [lines[0], lines[0], b'@DIAG FRAME 1 not-hex\n', *lines[2:]] if attempt == 1 else lines
        result = export_capture(device(self.frames, transform=glitch), self.prefix)
        self.assertTrue(result['complete'])
        self.assertEqual(result['statistics']['retried_frames'], 1)
        self.assertGreaterEqual(result['statistics']['protocol_errors'], 2)

    def test_unrecoverable_usb_failure_is_explicit_and_retains_partial(self):
        def glitch(start, count, attempt, lines):
            return [line for line in lines if not line.startswith(b'@DIAG FRAME 2 ')]
        obj = device(self.frames, transform=glitch)
        with self.assertRaisesRegex(IncompleteCapture, 'Frame 2 unavailable'):
            export_capture(obj, self.prefix, retries=2)
        self.assertFalse(Path(str(self.prefix) + '.bin').exists())
        self.assertEqual(Path(str(self.prefix) + '.bin.partial').read_bytes(), b''.join(self.frames[:2]))
        manifest = self.read_json('.manifest.json.partial')
        self.assertFalse(manifest['complete'])
        self.assertEqual(manifest['verified_frames'], 2)
        self.assertEqual(manifest['statistics']['retry_requests'], 2)

    def test_crc_bad_storage_frame_is_preserved_and_reported(self):
        frames = list(self.frames)
        frames[2] = frames[2][:30] + bytes([frames[2][30] ^ 1]) + frames[2][31:]
        with self.assertRaises(IncompleteCapture):
            export_capture(device(frames), self.prefix)
        self.assertEqual(Path(str(self.prefix) + '.bin').read_bytes(), b''.join(frames))
        manifest = self.read_json('.manifest.json')
        self.assertEqual(manifest['exported_frames'], 4)
        self.assertEqual(manifest['verified_frames'], 3)
        self.assertEqual(manifest['statistics']['frame_crc_errors'], 1)
        self.assertFalse(manifest['complete'])

    def test_previous_boot_drop_counter_survives_later_clean_boot(self):
        frames = list(self.frames)
        frames[2] = reseal(frames[2], 24, 7)
        with self.assertRaises(IncompleteCapture):
            export_capture(device(frames), self.prefix)
        manifest = self.read_json('.manifest.json')
        self.assertEqual(manifest['boot_dropped'], {'42': 7, '43': 0})
        self.assertTrue(any('7 records dropped' in problem for problem in manifest['problems']))

    def test_sequence_gap_is_reported_without_discarding_raw(self):
        frames = list(self.frames)
        frames[2] = reseal(frames[2], 16, 3)
        with self.assertRaises(IncompleteCapture):
            export_capture(device(frames), self.prefix)
        manifest = self.read_json('.manifest.json')
        self.assertEqual(len(manifest['sequence_gaps']), 1)
        self.assertEqual(manifest['exported_frames'], 4)

    def test_full_and_source_error_are_not_success(self):
        with self.assertRaises(IncompleteCapture):
            export_capture(device(self.frames, status={'state': 'full', 'dropped': 2}), self.prefix)
        manifest = self.read_json('.manifest.json')
        self.assertFalse(manifest['complete'])
        self.assertTrue(any('full' in problem for problem in manifest['problems']))
        self.assertEqual(manifest['exported_frames'], 4)

    def test_recording_growth_is_distinct_from_missing_frames(self):
        obj = device(self.frames)
        original_status = obj.status
        calls = 0
        def status():
            nonlocal calls
            calls += 1
            result = original_status()
            if calls == 2:
                result['used_frames'] += 3
            return result
        obj.status = status
        manifest = export_capture(obj, self.prefix)
        self.assertTrue(manifest['complete'])
        self.assertEqual(manifest['requested_frames'], 4)
        self.assertEqual(manifest['new_frames_during_export'], 3)

    def test_unknown_snapshot_keeps_raw_and_fails_visibly(self):
        raw = bytearray(self.frames[1])
        at = 28
        while struct.unpack_from('<H', raw, at)[0] != 3:
            at += 8 + struct.unpack_from('<H', raw, at + 2)[0]
        raw[at + 8] = 2  # Future/unsupported schema, with a valid on-flash CRC.
        struct.pack_into('<I', raw, 508, zlib.crc32(raw[:508]))
        frames = [self.frames[0], bytes(raw), *self.frames[2:]]
        with self.assertRaises(IncompleteCapture):
            export_capture(device(frames), self.prefix)
        self.assertEqual(Path(str(self.prefix) + '.bin').read_bytes(), b''.join(frames))
        self.assertEqual(self.read_json('.manifest.json')['decode_errors'], 1)

    def test_empty_uninitialized_capture_is_not_success(self):
        with self.assertRaisesRegex(RuntimeError, 'No diagnostic ownership header'):
            export_capture(device([]), self.prefix)

    def test_invalid_payload_utc_and_binary_auxiliary_events(self):
        payload = bytearray(SNAPSHOT.pack(1, 16, 0, 255, 86400000, 0, 0, 20260923,
                                          0, 0, 0, 0, 99, 4000, 0, 1, 0, 0, 0, 0, 0, 0))
        with self.assertRaisesRegex(RuntimeError, 'out-of-range'):
            decode_payload(3, payload)
        payload[1] = 0
        snapshot = decode_payload(3, payload)
        self.assertIsNone(snapshot['gnss_utc'])
        self.assertEqual(snapshot['phase_name'], 'station_continuous_rx_sd')
        payload[3] = 0
        self.assertEqual(decode_payload(3, payload)['phase_name'], 'warmup_rf_off_sd_off')
        payload[3] = 6
        payload[1] = 64
        aborted = decode_payload(3, payload)
        self.assertEqual(aborted['phase_name'], 'aborted')
        self.assertTrue(aborted['rf_target_enabled'])
        self.assertFalse(aborted['sd_target_enabled'])
        tx = decode_payload(7, struct.pack('<Bh', 3, -7) + b'\x01\xff')
        self.assertEqual((tx['event'], tx['status'], tx['wire_hex']), ('failed', -7, '01ff'))
        chunk = decode_payload(8, b'{"part":')
        self.assertEqual(chunk['kind'], 'sd_status_chunk')
        self.assertEqual(chunk['raw_hex'], b'{"part":'.hex())
        self.assertFalse(decode_payload(6, b'\xff')['utf8_valid'])

    def test_erase_requires_explicit_confirmation(self):
        obj = device(self.frames)
        with self.assertRaises(ValueError):
            obj.erase()
        self.assertEqual(obj.serial.commands, [])
        self.assertTrue(obj.erase(confirm=True)['erased'])
        self.assertEqual(obj.serial.commands, ['DIAG ERASE CONFIRM'])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            main(['--port', 'must-not-open', 'erase'])
        self.assertEqual(raised.exception.code, 2)

    def test_safe_serial_open_never_updates_modem_lines_and_clears_hupcl(self):
        class SerialBase:
            resets = 0
            def __init__(self, **kwargs):
                self.kwargs = kwargs
            def _update_dtr_state(self):
                SerialBase.resets += 1
            def _update_rts_state(self):
                SerialBase.resets += 1
            @property
            def dtr(self):
                return False
            @dtr.setter
            def dtr(self, value):
                self._update_dtr_state()
            @property
            def rts(self):
                return False
            @rts.setter
            def rts(self, value):
                self._update_rts_state()
            def open(self):
                self._update_dtr_state()
                self._update_rts_state()
            def close(self):
                pass
            def fileno(self):
                return 99
        serial = types.SimpleNamespace(Serial=SerialBase)
        termios = types.SimpleNamespace(HUPCL=1024, TCSANOW=0,
                                       tcgetattr=lambda fd: [0, 0, 1024, 0], tcsetattr=mock.Mock())
        with mock.patch.dict(sys.modules, {'serial': serial, 'termios': termios}):
            obj = Device('fake')
        self.assertEqual(SerialBase.resets, 0)
        self.assertEqual(termios.tcsetattr.call_args.args[2][2] & 1024, 0)
        self.assertIsNone(obj.serial.kwargs['port'])
        self.assertEqual(obj.serial.kwargs['baudrate'], 115200)


if __name__ == '__main__':
    unittest.main(verbosity=2)
