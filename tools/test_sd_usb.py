#!/usr/bin/env python3
"""Exercise the actual USB reader without serial hardware."""
import json
from pathlib import Path
import tempfile
import zlib
from read_sd_log import Device

class Serial:
    def __init__(self, data):
        self.data = data
        self.sent = b''
    def read_until(self, delimiter):
        n = self.data.find(delimiter)
        n = len(self.data) if n < 0 else n + 1
        chunk, self.data = self.data[:n], self.data[n:]
        return chunk
    def read(self, n):
        n = min(n, 7)  # partial USB reads
        chunk, self.data = self.data[:n], self.data[n:]
        return chunk
    def write(self, data):
        self.sent += data
        return len(data)
    def flush(self):
        pass

def device(data):
    obj = Device.__new__(Device)
    obj.serial = Serial(data)
    return obj

with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'verified.ndjson'
    payload = b'{"kind":"session"}\n{"raw_hex":"000aff"}\n'
    header = f'@SD BEGIN {len(payload)} /logs/12345678-0000.ndjson\n'.encode()
    tail = f'@SD END {zlib.crc32(payload):08x}\n'.encode()
    obj = device(b'boot log\n' + header + payload + tail)
    result = obj.read('LAST', path)
    assert path.read_bytes() == payload and result['bytes'] == len(payload)
    assert not path.with_name(path.name + '.partial').exists()
    try:
        obj.read('LAST', path)
        assert False, 'Must not overwrite local data'
    except FileExistsError:
        pass
    bad = Path(tmp) / 'corrupt.ndjson'
    try:
        device(header + payload + b'@SD END 00000000\n').read('LAST', bad)
        assert False, 'Must reject CRC failure'
    except RuntimeError:
        pass
    assert not bad.exists() and bad.with_name(bad.name + '.partial').exists()
assert device(b'truncated log@SD STATUS {"state":"recording"}\n').status()['state'] == 'recording'
assert device(b'@SD LIST BEGIN\n@SD FILE 123 /logs/12345678-0000.ndjson\n@SD LIST END\n').list()[0]['bytes'] == 123
assert device(b'@SD FORMAT BEGIN bytes=8000000000\n@SD FORMAT OK FAT32\n').format()[-1] == '@SD FORMAT OK FAT32'
print('PASS USB reader: partial reads, byte count/CRC, corruption rejection, local overwrite protection, list/status/format framing')
