#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read the release version used by the firmware and installer ZIP."""
from pathlib import Path
import re

VERSION_FILE = Path(__file__).resolve().parents[1] / 'VERSION'
VERSION_PATTERN = re.compile(r'v[1-9][0-9]*')


def read_version():
    try:
        version = VERSION_FILE.read_text(encoding='utf-8').strip()
    except OSError as error:
        raise SystemExit(f'Could not read VERSION: {error.strerror}') from error
    if not VERSION_PATTERN.fullmatch(version):
        raise SystemExit('VERSION must contain v followed by a positive integer, such as v1.')
    return version


if __name__ == '__main__':
    print(read_version())
