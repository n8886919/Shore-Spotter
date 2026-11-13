#!/usr/bin/env python3
"""Read or export dated records from their pinned Git commit; no checkout/reset."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def read_blob(record, item):
    data = subprocess.check_output(
        ['git', 'show', record['commit'] + ':' + item['path']], cwd=ROOT)
    if hashlib.sha256(data).hexdigest() != item['sha256']:
        raise ValueError('SHA-256 mismatch: ' + item['path'])
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('record', nargs='?', help='record ID, filename or original path')
    parser.add_argument('--verify', action='store_true', help='verify records and their images')
    parser.add_argument('--export', type=Path, metavar='DIR', help='export one record and images, preserving relative paths')
    args = parser.parse_args()
    records = json.loads((ROOT / 'docs/records.json').read_text())['records']
    if args.record:
        selected = [r for r in records if args.record in (r['id'], r['path'], Path(r['path']).name)]
        if not selected:
            parser.error('unknown record; run without arguments to list IDs')
    else:
        if args.export:
            parser.error('--export requires a record ID')
        selected = records
    if args.verify:
        seen = set()
        for record in selected:
            for item in [record] + record.get('assets', []):
                key = (record['commit'], item['path'])
                if key not in seen:
                    read_blob(record, item)
                    seen.add(key)
        print(f'PASS: {len(selected)} records, {len(seen)} Git blobs verified')
    elif args.export:
        record = selected[0]
        exports = []
        for item in [record] + record.get('assets', []):
            path = PurePosixPath(item['path'])
            if path.is_absolute() or '..' in path.parts:
                raise ValueError('invalid archived path: ' + str(path))
            target = args.export.resolve() / path
            data = read_blob(record, item)
            if target.exists() and target.read_bytes() != data:
                raise ValueError('refusing to overwrite a different file: ' + str(target))
            exports.append((target, data))
        for target, data in exports:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        print(exports[0][0])
    elif args.record:
        sys.stdout.buffer.write(read_blob(selected[0], selected[0]))
    else:
        for record in sorted(records, key=lambda r: (r['date'], r['id']), reverse=True):
            print(f"{record['date']}  {record['id']}  {record['title']}")
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
