#!/usr/bin/env python3
"""Run the existing host regressions; never opens hardware or uploads firmware."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare', action='store_true',
                        help='install library dependencies required by host tests first')
    parser.add_argument('--list', action='store_true', help='only list test commands')
    args = parser.parse_args()
    commands = [['pio', 'test', '-e', 'native']]
    commands += [[sys.executable if p.suffix == '.py' else 'node', str(p.relative_to(ROOT))]
                 for p in sorted((ROOT / 'tools').glob('test_*'))
                 if p.suffix in ('.py', '.js')]
    if args.list:
        for command in commands:
            print(' '.join(command))
        return 0
    missing = [tool for tool in ('pio', 'node', 'g++') if not shutil.which(tool)]
    if missing:
        print('Missing tools: ' + ', '.join(missing), file=sys.stderr)
        return 2
    env = dict(os.environ)
    # A station-specific profile must not leak into the other test environments.
    env.pop('SHORE_DEVICE', None)
    if args.prepare:
        command = ['pio', 'pkg', 'install', '-e', 'tbeam-client',
                   '-e', 'tbeam-client-trip', '-e', 't096-client']
        result = subprocess.run(command, cwd=ROOT, env=env)
        if result.returncode:
            return result.returncode
    dependencies = [
        '.pio/libdeps/tbeam-client/XPowersLib/src/XPowersAXP2101.tpp',
        '.pio/libdeps/tbeam-client-trip/XPowersLib/src/REG/AXP2101Constants.h',
        '.pio/libdeps/t096-client/Adafruit ST7735 and ST7789 Library/Adafruit_ST7735.cpp',
    ]
    if any(not (ROOT / path).is_file() for path in dependencies):
        print('Missing test libraries. Run: python3 tools/verify_project.py --prepare',
              file=sys.stderr)
        return 2
    failed = []
    for command in commands:
        print('\nRUN ' + ' '.join(command), flush=True)
        result = subprocess.run(command, cwd=ROOT, env=env)
        if result.returncode:
            failed.append(' '.join(command))
    print(f'\nHost suites: {len(commands) - len(failed)}/{len(commands)} passed')
    for command in failed:
        print('FAILED ' + command)
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
