#!/usr/bin/env python3
"""Export diagnostic flash after a trip; opening USB never resets or erases it.

``read --output /private/path/trip`` writes trip.bin (exact flash frames),
trip.ndjson (decoded records), and trip.manifest.json (coverage/integrity).
Host receive times are export times, never substitutes for GNSS observation time.
"""
import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import struct
import sys
import time
import zlib

FRAME_SIZE = 512
SNAPSHOT = struct.Struct('<BBBBIIIIddfffHHIIIIIII')
PHASES = ('warmup_rf_off_sd_off', 'rf_off_sd_off', 'rf_on_sd_off', 'rf_on_sd_on',
          'rf_on_sd_off_repeat', 'done', 'aborted')
FRAME_LINE = re.compile(r'^@DIAG FRAME (\d+) ([0-9a-fA-F]{8}) ([0-9a-fA-F]{1024})$')


class ProtocolError(RuntimeError):
    pass


class IncompleteCapture(RuntimeError):
    """Artifacts are retained, but a complete trustworthy capture is unavailable."""


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec='milliseconds').replace('+00:00', 'Z')


def _finite(value):
    return value if math.isfinite(value) else None


def decode_payload(kind, payload):
    if kind in (1, 6):
        return {'kind': 'raw_uart' if kind == 1 else 'discarded_backlog',
                'raw_hex': payload.hex(),
                'text': payload.decode('utf-8', errors='replace'),
                'utf8_valid': _is_utf8(payload),
                'note': 'Exact UART chunk; may contain partial or multiple NMEA sentences'}
    if kind == 7:
        if len(payload) < 3:
            raise ProtocolError('Truncated TX event payload')
        event, status = struct.unpack_from('<Bh', payload)
        names = ('none', 'started', 'sent', 'failed', 'timeout', 'cancelled')
        return {'kind': 'tx_event', 'event_id': event,
                'event': names[event] if event < len(names) else 'unknown',
                'status': status, 'wire_hex': payload[3:].hex()}
    if kind == 8:
        return {'kind': 'sd_status_chunk', 'raw_hex': payload.hex(),
                'text': payload.decode('utf-8', errors='replace'),
                'utf8_valid': _is_utf8(payload),
                'note': 'Ordered JSON stream chunk; concatenate through newline within this boot/ms'}
    if kind in (2, 4, 5):
        value = json.loads(payload.decode('utf-8'))
        if not isinstance(value, dict):
            raise ProtocolError('Diagnostic JSON payload is not an object')
        return {'kind': {2: 'boot', 4: 'phase', 5: 'gap_status'}[kind], 'data': value}
    if kind != 3:
        return {'kind': 'unknown', 'raw_hex': payload.hex()}
    if len(payload) != SNAPSHOT.size:
        raise ProtocolError(f'Snapshot payload has {len(payload)} bytes; expected {SNAPSHOT.size}')
    values = SNAPSHOT.unpack(payload)
    (version, flags, sats, phase, epoch, source_age, arrival_age, date,
     lat, lon, speed, course, hdop, battery, reserved, heap, backlog,
     tx, tx_errors, splits, loop_gap, utc_age) = values
    if version != 1 or reserved != 0:
        raise ProtocolError(f'Unsupported snapshot schema {version} or reserved field {reserved}')
    utc = None
    if flags & 16:
        if not 0 <= epoch < 86400000 or utc_age >= 2000:
            raise ProtocolError('Snapshot marks out-of-range/stale GNSS UTC valid')
        try:
            parsed = datetime.strptime(str(date), '%Y%m%d').replace(tzinfo=timezone.utc)
            from datetime import timedelta
            utc = (parsed + timedelta(milliseconds=epoch)).isoformat(timespec='milliseconds').replace('+00:00', 'Z')
        except ValueError as error:
            raise ProtocolError(f'Snapshot marks invalid GNSS date valid: {date}') from error
    return {'kind': 'snapshot', 'schema_version': version, 'flags': flags,
            'sampled': bool(flags & 1), 'fix': bool(flags & 2),
            'rmc_present': bool(flags & 4), 'gga_present': bool(flags & 8),
            'utc_valid': bool(flags & 16), 'recovering': bool(flags & 32),
            'rf_target_enabled': bool(flags & 64), 'sd_target_enabled': bool(flags & 128),
            'satellites': sats, 'phase': phase,
            'phase_name': PHASES[phase] if phase < len(PHASES) else
                          'client_trip_continuous_rf_sd' if phase == 7 else
                          'station_continuous_rx_sd' if phase == 255 else 'unknown',
            'epoch_ms': epoch, 'source_age_ms': source_age, 'arrival_age_ms': arrival_age,
            'utc_date_yyyymmdd': date, 'gnss_utc': utc,
            'utc_source': 'gnss_rmc' if utc is not None else None, 'utc_age_ms': utc_age,
            'lat': _finite(lat), 'lon': _finite(lon), 'speed_mps': _finite(speed),
            'course_deg': _finite(course), 'hdop': _finite(hdop), 'battery_mv': battery,
            'heap_free': heap, 'backlog_drops': backlog, 'tx_count': tx,
            'tx_errors': tx_errors, 'raw_chunk_splits': splits, 'loop_gap_max_ms': loop_gap}


def _is_utf8(payload):
    try:
        payload.decode('utf-8')
        return True
    except UnicodeDecodeError:
        return False


def decode_frame(raw, index):
    # The frame codec is kept explicit and versioned; never guess at corrupt bytes.
    if len(raw) != FRAME_SIZE or zlib.crc32(raw[:508]) != struct.unpack_from('<I', raw, 508)[0]:
        raise ProtocolError(f'Frame {index}: invalid on-flash CRC/length')
    if index == 0:
        address, size, frame_bytes, version, reserved = struct.unpack_from('<IIIII', raw, 8)
        if raw[:8] != b'SSDHDR01' or (address, size, frame_bytes, version, reserved) != (0x670000, 0x180000, 512, 1, 0):
            raise ProtocolError('Unsupported diagnostic flash ownership header')
        return [{'kind': 'store_header', 'frame_index': 0, 'schema_version': version,
                 'partition_address': address, 'partition_bytes': size, 'frame_bytes': frame_bytes}]
    if raw[:8] != b'SSDFRM01':
        raise ProtocolError(f'Frame {index}: invalid record magic')
    used, count, boot, sequence, first_ms, dropped = struct.unpack_from('<HHIIII', raw, 8)
    if not 0 < used <= 480 or not count:
        raise ProtocolError(f'Frame {index}: invalid frame header')
    records, at, end = [], 28, 28 + used
    while at < end:
        if at + 8 > end:
            raise ProtocolError(f'Frame {index}: truncated record header')
        kind, length, ms = struct.unpack_from('<HHI', raw, at)
        at += 8
        if at + length > end:
            raise ProtocolError(f'Frame {index}: truncated record payload')
        payload = raw[at:at + length]
        try:
            record = decode_payload(kind, payload)
        except (ValueError, UnicodeDecodeError, ProtocolError) as error:
            # A decode failure must retain the original bytes for investigation.
            # Re-reading cannot fix an unsupported/bad payload inside a valid CRC.
            record = {'kind': 'decode_error', 'raw_hex': payload.hex(), 'decode_error': str(error)}
        record.update(frame_index=index, frame_sequence=sequence, frame_first_ms=first_ms,
                      frame_dropped=dropped, record_index=len(records), kind_id=kind,
                      boot_id=boot, boot_ms=ms)
        records.append(record)
        at += length
    if len(records) != count:
        raise ProtocolError(f'Frame {index}: record count mismatch')
    return records


class Device:
    def __init__(self, port, response_timeout=8):
        import serial
        serial_class = serial.Serial
        if os.name == 'posix':
            # USB/JTAG modem-line transitions can reset the ESP32. Merely reading
            # a log must neither reset it nor enter the ROM download loader.
            class NoResetSerial(serial.Serial):
                def _update_dtr_state(self):
                    pass

                def _update_rts_state(self):
                    pass
            serial_class = NoResetSerial
        self.serial = serial_class(port=None, baudrate=115200, timeout=0.25, write_timeout=3)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = port
        self.serial.open()
        try:
            if os.name == 'posix':
                import termios
                attrs = termios.tcgetattr(self.serial.fileno())
                attrs[2] &= ~termios.HUPCL
                termios.tcsetattr(self.serial.fileno(), termios.TCSANOW, attrs)
        except BaseException:
            self.serial.close()
            raise
        self.response_timeout = response_timeout
        self.stats = {'usb_crc_errors': 0, 'frame_crc_errors': 0,
                      'protocol_errors': 0, 'retried_frames': 0, 'retry_requests': 0}

    def close(self):
        self.serial.close()

    def send(self, command):
        encoded = (command + '\n').encode('ascii')
        sent = self.serial.write(encoded)
        if sent != len(encoded):
            raise ProtocolError(f'USB command short write ({sent}/{len(encoded)})')
        self.serial.flush()

    def line(self, timeout=None):
        deadline = time.monotonic() + (self.response_timeout if timeout is None else timeout)
        pending = bytearray()
        while time.monotonic() < deadline:
            chunk = self.serial.read_until(b'\n')
            if not chunk:
                continue
            pending.extend(chunk)
            if len(pending) > 8192:
                raise ProtocolError('USB line exceeds bounded diagnostic protocol limit')
            if not pending.endswith(b'\n'):
                continue
            raw = bytes(pending)
            pending.clear()
            at = raw.find(b'@DIAG ')
            if at < 0:
                continue
            try:
                result = raw[at:].decode('ascii').strip()
            except UnicodeDecodeError as error:
                raise ProtocolError('Non-ASCII diagnostic protocol line') from error
            if result.startswith('@DIAG ERROR '):
                raise ProtocolError(result)
            return result
        raise TimeoutError('Diagnostic USB response timed out')

    def status(self):
        self.send('DIAG STATUS')
        line = self.line()
        if not line.startswith('@DIAG STATUS '):
            raise ProtocolError('Unexpected diagnostic status response: ' + line)
        value = json.loads(line[len('@DIAG STATUS '):])
        if not isinstance(value, dict) or not isinstance(value.get('used_frames'), int):
            raise ProtocolError('Diagnostic status lacks integer used_frames')
        if value['used_frames'] < 0:
            raise ProtocolError('Diagnostic status has negative used_frames')
        return value

    def erase(self, *, confirm=False):
        if not confirm:
            raise ValueError('Erasure requires explicit --confirm-erase')
        self.send('DIAG ERASE CONFIRM')
        line = self.line(timeout=120)
        if line != '@DIAG ERASE OK':
            raise ProtocolError('Unexpected diagnostic erase response: ' + line)
        return {'erased': True}

    def read_batch(self, start, count):
        self.send(f'DIAG READ {start} {count}')
        deadline = time.monotonic() + max(self.response_timeout, count * 0.5)
        expected_begin = f'@DIAG BEGIN {start} {count}'
        # A timed-out USB batch may leave its tail in the serial buffer. Consume
        # bounded stale protocol lines until the requested indexed reply begins.
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('Missing diagnostic read header')
            if self.line(timeout=remaining) == expected_begin:
                break
            self.stats['protocol_errors'] += 1
        records = {}
        received_lines = 0
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('Diagnostic frame batch timed out')
            line = self.line(timeout=remaining)
            if line.startswith('@DIAG END '):
                parts = line.split()
                if len(parts) != 4 or parts[2] != str(start):
                    self.stats['protocol_errors'] += 1
                    continue
                if parts[3] != str(count):
                    self.stats['protocol_errors'] += 1
                return records
            received_lines += 1
            if received_lines > count * 2 + 16:
                raise ProtocolError('Too many unexpected diagnostic frame lines')
            match = FRAME_LINE.fullmatch(line)
            if not match:
                self.stats['protocol_errors'] += 1
                continue
            index, crc_hex, raw_hex = match.groups()
            index = int(index)
            if not start <= index < start + count or index in records:
                self.stats['protocol_errors'] += 1
                continue
            raw = bytes.fromhex(raw_hex)
            if zlib.crc32(raw) != int(crc_hex, 16):
                self.stats['usb_crc_errors'] += 1
                continue
            try:
                decoded = decode_frame(raw, index)
            except ProtocolError as error:
                self.stats['frame_crc_errors'] += 1
                # USB CRC passed: these are the bytes actually stored in flash.
                # Preserve corrupt storage frames instead of losing the evidence
                # through endless retries or silently dropping their records.
                decoded = [{'kind': 'corrupt_frame', 'frame_index': index,
                            'decode_error': str(error), 'raw_hex': raw.hex()}]
            records[index] = (raw, decoded, utc_now())


def _source_problems(status, when):
    problems = []
    for name in ('dropped', 'corrupt_frames', 'errors', 'error'):
        value = status.get(name)
        if value not in (None, False, 0, '', '0', 'none', 'ok'):
            problems.append(f'{when}: {name}={value}')
    if status.get('state') == 'full':
        problems.append(f'{when}: diagnostic flash full; recording stopped at capacity')
    return problems


def export_capture(device, output, *, retries=3, batch_size=32):
    if not 1 <= batch_size <= 256 or retries < 0:
        raise ValueError('batch_size must be 1..256 and retries must be nonnegative')
    output = Path(output).expanduser().resolve()
    paths = {name: Path(str(output) + suffix) for name, suffix in
             (('binary', '.bin'), ('ndjson', '.ndjson'), ('manifest', '.manifest.json'))}
    partials = {name: Path(str(path) + '.partial') for name, path in paths.items()}
    for path in (*paths.values(), *partials.values()):
        if path.exists():
            raise FileExistsError(path)
    started = utc_now()
    before = device.status()
    count = before['used_frames']
    if count == 0:
        raise ProtocolError('No diagnostic ownership header/frames available; inspect DIAG STATUS before exporting')
    capacity = before.get('capacity_frames')
    if capacity is not None and (not isinstance(capacity, int) or count > capacity):
        raise ProtocolError('Diagnostic status frame count exceeds capacity')
    manifest = {'format': 'shore-spotter-diagnostic-export-v1', 'started_utc': started,
                'host_time_meaning': 'USB export receive time; not GNSS acquisition time',
                'frame_bytes': FRAME_SIZE, 'requested_frames': count, 'verified_frames': 0,
                'exported_frames': 0,
                'status_before': before, 'status_after': None, 'complete': False,
                'statistics': device.stats, 'problems': [],
                'files': {name: str(path) for name, path in paths.items()}}
    error = None
    total_crc = 0
    last_boot = last_sequence = None
    source_gaps = []
    decode_errors = 0
    boot_dropped = {}
    binary = decoded_file = None
    try:
        binary = open(partials['binary'], 'xb')
        decoded_file = open(partials['ndjson'], 'x', encoding='utf-8')
        for path in (partials['binary'], partials['ndjson']):
            os.chmod(path, 0o600)
        for start in range(0, count, batch_size):
            size = min(batch_size, count - start)
            try:
                records = device.read_batch(start, size)
            except (ProtocolError, TimeoutError) as issue:
                device.stats['protocol_errors'] += 1
                records = {}
                manifest['problems'].append(f'Batch {start} retry after {issue}')
            for index in range(start, start + size):
                if index not in records:
                    device.stats['retried_frames'] += 1
                    for attempt in range(retries):
                        device.stats['retry_requests'] += 1
                        try:
                            recovered = device.read_batch(index, 1)
                        except (ProtocolError, TimeoutError) as issue:
                            device.stats['protocol_errors'] += 1
                            manifest['problems'].append(f'Frame {index} retry {attempt + 1}: {issue}')
                            continue
                        if index in recovered:
                            records.update(recovered)
                            break
                if index not in records:
                    raise IncompleteCapture(f'Frame {index} unavailable after {retries} retries; retained .partial files')
                raw, decoded, received = records[index]
                binary.write(raw)
                total_crc = zlib.crc32(raw, total_crc)
                first = decoded[0]
                if 'boot_id' in first:
                    boot, seq = first['boot_id'], first['frame_sequence']
                    if boot == last_boot and seq != (last_sequence + 1) & 0xffffffff:
                        source_gaps.append(f'Frame {index}: boot {boot} sequence {last_sequence} -> {seq}')
                    elif boot != last_boot and seq != 0:
                        source_gaps.append(f'Frame {index}: boot {boot} begins at sequence {seq}')
                    last_boot, last_sequence = boot, seq
                    boot_dropped[str(boot)] = max(boot_dropped.get(str(boot), 0), first['frame_dropped'])
                for record in decoded:
                    decode_errors += int('decode_error' in record)
                    record['host_received_utc'] = received
                    decoded_file.write(json.dumps(record, ensure_ascii=False, allow_nan=False) + '\n')
                manifest['exported_frames'] += 1
                manifest['verified_frames'] += int(first.get('kind') != 'corrupt_frame')
        after = device.status()
        manifest['status_after'] = after
        manifest['new_frames_during_export'] = after['used_frames'] - count
        if after['used_frames'] < count:
            raise IncompleteCapture('Diagnostic frame count shrank during export; capture may have been erased')
        source_problems = _source_problems(before, 'before') + _source_problems(after, 'after') + source_gaps
        if decode_errors:
            source_problems.append(f'{decode_errors} records could not be decoded; raw bytes retained')
        for boot, dropped in boot_dropped.items():
            if dropped:
                source_problems.append(f'Boot {boot}: at least {dropped} records dropped before flash write')
        manifest['problems'].extend(source_problems)
        manifest['complete'] = not source_problems
        if source_problems:
            error = IncompleteCapture('Flash reported capture loss/error; inspect retained manifest')
    except BaseException as issue:
        error = issue
        manifest['problems'].append(str(issue))
    finally:
        manifest['finished_utc'] = utc_now()
        manifest['exported_bytes'] = manifest['exported_frames'] * FRAME_SIZE
        manifest['verified_bytes'] = manifest['verified_frames'] * FRAME_SIZE
        manifest['binary_crc32'] = f'{total_crc:08x}'
        manifest['sequence_gaps'] = source_gaps
        manifest['decode_errors'] = decode_errors
        manifest['boot_dropped'] = boot_dropped
        for stream in (binary, decoded_file):
            if stream is not None:
                stream.flush()
                os.fsync(stream.fileno())
                stream.close()
        with open(partials['manifest'], 'x', encoding='utf-8') as stream:
            os.chmod(partials['manifest'], 0o600)
            json.dump(manifest, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
    # A verified transfer can still expose source recording loss. Keep its fully
    # exported files and fail visibly; incomplete transfers retain .partial.
    if manifest['exported_frames'] == count and manifest['status_after'] is not None:
        for name in paths:
            os.link(partials[name], paths[name])
            partials[name].unlink()
    if error is not None:
        raise error
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True, help='Confirmed Client/Station USB port')
    parser.add_argument('--timeout', type=float, default=8, help='USB response timeout in seconds')
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('status')
    read = sub.add_parser('read', help='Export a fixed starting frame count without stopping capture')
    read.add_argument('--output', required=True, help='New output prefix, without .bin/.ndjson extension')
    read.add_argument('--retries', type=int, default=3, help='Additional single-frame retries on USB errors')
    read.add_argument('--batch-size', type=int, default=32)
    erase = sub.add_parser('erase', help='ERASE diagnostic flash; never runs automatically')
    erase.add_argument('--confirm-erase', action='store_true', required=True)
    args = parser.parse_args(argv)
    device = None
    try:
        device = Device(args.port, response_timeout=args.timeout)
        if args.command == 'status':
            result = device.status()
        elif args.command == 'erase':
            result = device.erase(confirm=args.confirm_erase)
        else:
            result = export_capture(device, args.output, retries=args.retries, batch_size=args.batch_size)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1
    finally:
        if device is not None:
            device.close()


if __name__ == '__main__':
    raise SystemExit(main())
