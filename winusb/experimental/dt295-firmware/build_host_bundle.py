# SPDX-License-Identifier: GPL-2.0-only
"""Bundle source-built PIX-DT295 firmware and its frontend initialization trace."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from aux_probe import image
from boot_contract import inspect_build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('auxiliary', type=Path)
    parser.add_argument('bulk', type=Path)
    parser.add_argument('trace', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    inspect_build(args.bulk)
    manifest = json.loads((args.bulk / 'manifest.json').read_text(encoding='utf-8'))
    if 'bulk_pump.c' not in manifest.get('sources', {}):
        parser.error('build.py --bulk is required')
    images = [image(args.auxiliary), *((args.bulk / name).read_bytes()
              for name in ('resident.bin', 'bootstrap.bin', 'boot-hook.bin'))]
    rows = bytearray()
    satellite = False
    for line in args.trace.read_text(encoding='utf-8').splitlines():
        row = json.loads(line)
        op = row['op']
        if op == -2:
            continue  # DriverHost owns the demodulator reset GPIO.
        if op == 2:
            satellite = True
        if op == -1:
            delay = row['delay_ms']
            if not 0 <= delay <= 255:
                parser.error('initialization delay must fit in one byte')
            data = bytes([delay])
        elif op in (0, 2, 128):
            data = bytes.fromhex(row['payload'])
            if not 1 <= len(data) <= 8 or (op == 128 and len(data) != 4):
                parser.error('invalid initialization transfer size')
        else:
            parser.error(f'unknown initialization operation {op}')
        if satellite and op in (0, 128):
            parser.error('terrestrial operation after satellite initialization')
        rows.extend(bytes([op & 255, len(data)]) + data)
    if not satellite:
        parser.error('the trace must include satellite initialization')
    sections = [*images, bytes(rows)]
    bundle = b'DTP1' + struct.pack('>5I', *(len(section) for section in sections)) + b''.join(sections)
    with args.output.open('xb') as output:
        output.write(bundle)
    print(f'SourceFirmwareSha256={hashlib.sha256(bundle).hexdigest()}')


if __name__ == '__main__':
    main()
