# SPDX-License-Identifier: GPL-2.0-only
"""Package the main-CPU-only DT295 PIF1 build for DriverHost."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from boot_contract import inspect_build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    segments = inspect_build(args.build)
    manifest = json.loads((args.build / 'manifest.json').read_text(encoding='utf-8'))
    if 'direct_pump.c' not in manifest['sources'] or segments['binary']['address'] != '0x000e1060':
        parser.error('build.py --direct-usb is required')
    sections = [(args.build / name).read_bytes()
                for name in ('resident.bin', 'bootstrap.bin', 'boot-hook.bin')]
    bundle = b'DTP3' + struct.pack('>3I', *(len(section) for section in sections)) + b''.join(sections)
    with args.output.open('xb') as output:
        output.write(bundle)
    print(f'SourceFirmwareSha256={hashlib.sha256(bundle).hexdigest()}')


if __name__ == '__main__':
    main()
