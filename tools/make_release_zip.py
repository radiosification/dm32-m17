#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Package the DM32 FM/M17 application in the Open25 v2 ZIP format."""
import hashlib
import json
from pathlib import Path
import zipfile

from release_version import read_version

ROOT = Path(__file__).resolve().parents[1]
APPLICATION = ROOT / 'firmware/build/dm32-m17/rfoundry_dm32.bin'
MAX_APPLICATION_BYTES = 0xE8000 - 0xC000


def main():
    release = read_version()
    stamp = APPLICATION.with_name('version')
    if not stamp.is_file() or stamp.read_text().split()[1:2] != [release]:
        raise SystemExit('Firmware version does not match VERSION. Run make release-zip.')
    application = APPLICATION.read_bytes()
    if not 64 <= len(application) <= MAX_APPLICATION_BYTES:
        raise SystemExit('Application does not fit between the bootloader and reserved codec slot.')
    if len(set(application[:64])) == 1 or b'BFUV32-V2' in application[:256]:
        raise SystemExit('Expected a DM32 application image from make firmware.')

    manifest = {
        'format': 'open25-dm32-firmware',
        'version': 2,
        'target': 'dm32-048',
        'release': release,
        'applicationSha256': hashlib.sha256(application).hexdigest(),
        'codecTablesEnd': 0x18003E28,
    }
    output = ROOT / 'artifacts' / f'dm32-m17-{release}.zip'
    output.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(output, 'w') as archive:
        archive.writestr(zipfile.ZipInfo('firmware.json'), json.dumps(manifest, indent=2) + '\n')
        archive.writestr(zipfile.ZipInfo('application.bin'), application)
    print(output.relative_to(ROOT))
    print('ZIP SHA-256:', hashlib.sha256(output.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
