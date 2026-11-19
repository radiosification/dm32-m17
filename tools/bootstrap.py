#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch the pinned Codec2-mod and libm17 revisions."""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true',
                        help='verify dependencies without fetching or changing them')
    args = parser.parse_args()
    vendor = ROOT / 'vendor'
    if not args.check:
        vendor.mkdir(exist_ok=True)
    for name, dep in json.loads((ROOT / 'sources.json').read_text())['dependencies'].items():
        path = vendor / name
        if not path.exists():
            if args.check:
                raise SystemExit(f'{name} is missing. Run make bootstrap.')
            subprocess.run(['git', 'clone', dep['url'], str(path)], check=True)
        if Path(git(path, 'rev-parse', '--show-toplevel')).resolve() != path.resolve():
            raise SystemExit(f'vendor/{name} must be its own Git checkout.')
        if git(path, 'status', '--porcelain'):
            raise SystemExit(f'vendor/{name} has local changes. Use a clean checkout for a release.')
        if git(path, 'rev-parse', 'HEAD') != dep['commit']:
            if args.check:
                raise SystemExit(f'{name} is at the wrong revision. Run make bootstrap.')
            subprocess.run(['git', '-C', str(path), 'checkout', '--detach', dep['commit']], check=True)
        print(name, dep['commit'])


if __name__ == '__main__':
    main()
