#!/usr/bin/env python3
"""Read Station/Client SD logs over USB with size/CRC validation; no HTTP/UI required."""
import argparse
import json
import os
from pathlib import Path
import time
import zlib


class Device:
    def __init__(self, port):
        import serial
        serial_class = serial.Serial
        if os.name == 'posix':
            # On Linux USB/JTAG, changing modem lines can reset into download
            # mode. A log reader must not reset a device that is writing SD.
            class NoResetSerial(serial.Serial):
                def _update_dtr_state(self):
                    pass
                def _update_rts_state(self):
                    pass
            serial_class = NoResetSerial
        self.serial = serial_class(port=None, baudrate=115200, timeout=0.5, write_timeout=3)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = port
        self.serial.open()
        if os.name == 'posix':
            import termios
            attrs = termios.tcgetattr(self.serial.fileno())
            attrs[2] &= ~termios.HUPCL
            termios.tcsetattr(self.serial.fileno(), termios.TCSANOW, attrs)

    def close(self):
        self.serial.close()

    def send(self, command):
        self.serial.write((command + '\n').encode('ascii'))
        self.serial.flush()

    def line(self, timeout=90):
        deadline = time.monotonic() + timeout
        pending = bytearray()
        while time.monotonic() < deadline:
            chunk = self.serial.read_until(b'\n')
            if not chunk:
                continue
            pending.extend(chunk)
            if not pending.endswith(b'\n'):
                if len(pending) > 65536:
                    del pending[:-1024]
                continue
            raw = bytes(pending)
            pending.clear()
            at = raw.find(b'@SD ')
            if at < 0:
                continue
            result = raw[at:].decode('utf-8').strip()
            if result.startswith('@SD ERROR '):
                raise RuntimeError(result)
            return result
        raise TimeoutError('Device USB response timed out')

    def status(self, action='STATUS'):
        self.send('SD ' + action)
        line = self.line()
        if not line.startswith('@SD STATUS '):
            raise RuntimeError('Unexpected status response: ' + line)
        return json.loads(line[len('@SD STATUS '):])

    def list(self):
        self.send('SD LIST')
        if self.line() != '@SD LIST BEGIN':
            raise RuntimeError('Missing SD list header')
        files = []
        while True:
            line = self.line()
            if line == '@SD LIST END':
                return files
            prefix, kind, size, path = line.split(' ', 3)
            if (prefix, kind) != ('@SD', 'FILE'):
                raise RuntimeError('Unexpected file entry: ' + line)
            files.append({'bytes': int(size), 'path': path})

    def read(self, path, output):
        # Refuse to overwrite local data. A failed transfer retains .partial.
        output = Path(output).expanduser().resolve()
        partial = output.with_name(output.name + '.partial')
        if output.exists():
            raise FileExistsError(output)
        with open(partial, 'xb') as stream:
            os.chmod(partial, 0o600)
            self.send('SD READ ' + path)
            header = self.line().split(' ', 3)
            if len(header) != 4 or header[:2] != ['@SD', 'BEGIN']:
                raise RuntimeError('Missing file header')
            size = remaining = int(header[2])
            if size < 0:
                raise RuntimeError('Negative file size')
            crc = 0
            deadline = time.monotonic() + 15
            while remaining:
                chunk = self.serial.read(min(4096, remaining))
                if not chunk:
                    if time.monotonic() > deadline:
                        raise TimeoutError('USB file transfer stalled')
                    continue
                deadline = time.monotonic() + 15
                remaining -= len(chunk)
                stream.write(chunk)
                crc = zlib.crc32(chunk, crc)
            tail = self.line()
            if tail != f'@SD END {crc:08x}':
                raise RuntimeError('USB CRC mismatch: ' + tail)
            stream.flush()
            os.fsync(stream.fileno())
        # link is atomic and fails if another process created the destination.
        os.link(partial, output)
        partial.unlink()
        return {'path': header[3], 'bytes': size, 'crc32': f'{crc:08x}', 'output': str(output)}

    def format(self):
        self.send('SD FORMAT FAT32 ERASE')
        progress = []
        while True:
            # A 128 GB card can take several minutes to initialize FAT32 over SPI.
            line = self.line(timeout=900)
            progress.append(line)
            if line == '@SD FORMAT OK FAT32':
                return progress


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='/dev/ttyACM0')
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('status', 'start', 'stop', 'list'):
        sub.add_parser(name)
    read = sub.add_parser('read', help='Stops recording, reads a file, verifies CRC; use start afterwards')
    read.add_argument('path', help='/logs/<name>.ndjson or LAST')
    read.add_argument('--output', required=True)
    fmt = sub.add_parser('format', help='ERASE the inserted card and create FAT32; never runs automatically')
    fmt.add_argument('--erase', action='store_true', required=True)
    args = parser.parse_args()
    device = Device(args.port)
    try:
        # Allow a reset/USB reconnect to settle before sending the first command.
        time.sleep(2)
        if args.command == 'list':
            result = device.list()
        elif args.command == 'read':
            result = device.read(args.path, args.output)
        elif args.command == 'format':
            result = device.format()
        else:
            result = device.status(args.command.upper())
        print(json.dumps(result, ensure_ascii=False, indent=2))
    finally:
        device.close()


if __name__ == '__main__':
    main()
