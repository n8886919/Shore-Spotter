#!/usr/bin/env python3
"""Check 64-bit native ELF LOAD alignment and uncompressed APK ZIP offsets."""
import struct
import sys
import zipfile

apk = sys.argv[1]
errors = []
checked = 0
with open(apk, "rb") as raw, zipfile.ZipFile(apk) as archive:
    for info in archive.infolist():
        if not info.filename.startswith(("lib/arm64-v8a/", "lib/x86_64/")) or not info.filename.endswith(".so"):
            continue
        data = archive.read(info)
        if data[:5] != b"\x7fELF\x02":
            errors.append(f"{info.filename}: not ELF64")
            continue
        endian = "<" if data[5] == 1 else ">"
        offset = struct.unpack_from(endian + "Q", data, 32)[0]
        size, count = struct.unpack_from(endian + "HH", data, 54)
        alignments = []
        for i in range(count):
            start = offset + i * size
            if struct.unpack_from(endian + "I", data, start)[0] == 1:
                alignments.append(struct.unpack_from(endian + "Q", data, start + 48)[0])
        if not alignments or any(a < 16384 or a % 16384 for a in alignments):
            errors.append(f"{info.filename}: ELF LOAD alignment {alignments}")
        if info.compress_type == zipfile.ZIP_STORED:
            raw.seek(info.header_offset + 26)
            name_size, extra_size = struct.unpack("<HH", raw.read(4))
            data_offset = info.header_offset + 30 + name_size + extra_size
            if data_offset % 16384:
                errors.append(f"{info.filename}: APK offset {data_offset} not aligned")
        checked += 1
        print(f"{info.filename}: ELF LOAD {alignments}")
if errors:
    print("\n".join(errors), file=sys.stderr)
    sys.exit(1)
print(f"PASS: {checked} 64-bit native libraries; 16 KB ELF/APK alignment")
