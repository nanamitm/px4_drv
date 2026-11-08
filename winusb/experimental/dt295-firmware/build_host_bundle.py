# SPDX-License-Identifier: GPL-2.0-only
"""Bundle source-built PIX-DT295 firmware and its frontend initialization trace.

DTP1 includes the initialization trace. DTP2 (--runtime-init) omits it; DriverHost then
builds the same trace at runtime from the bundled mAgicVISION 0.1.0 firmware, so the
distributed bundle carries only code built from this directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from aux_probe import image
from boot_contract import inspect_build


def trace_section(parser, path):
    rows = bytearray()
    satellite = False
    for line in path.read_text(encoding='utf-8').splitlines():
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
    return bytes(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('auxiliary', type=Path)
    parser.add_argument('bulk', type=Path)
    parser.add_argument('trace', type=Path, nargs='?', help='initialization JSONL (DTP1 only)')
    parser.add_argument('output', type=Path)
    parser.add_argument('--runtime-init', action='store_true',
                        help='write DTP2 without the trace; DriverHost builds it at runtime')
    parser.add_argument('--provenance', type=Path,
                        help='also write the build manifests and bundle digest as JSON')
    args = parser.parse_args()
    if args.runtime_init == (args.trace is not None):
        parser.error('give a trace for DTP1, or --runtime-init without a trace for DTP2')
    inspect_build(args.bulk)
    manifest = json.loads((args.bulk / 'manifest.json').read_text(encoding='utf-8'))
    if 'bulk_pump.c' not in manifest.get('sources', {}):
        parser.error('build.py --bulk is required')
    sections = [image(args.auxiliary), *((args.bulk / name).read_bytes()
                for name in ('resident.bin', 'bootstrap.bin', 'boot-hook.bin'))]
    if args.runtime_init:
        bundle = b'DTP2' + struct.pack('>4I', *(len(section) for section in sections)) + b''.join(sections)
    else:
        sections.append(trace_section(parser, args.trace))
        bundle = b'DTP1' + struct.pack('>5I', *(len(section) for section in sections)) + b''.join(sections)
    digest = hashlib.sha256(bundle).hexdigest()
    with args.output.open('xb') as output:
        output.write(bundle)
    if args.provenance:
        record = dict(format=bundle[:4].decode(), bundle=dict(file=args.output.name, bytes=len(bundle), sha256=digest),
                      auxiliary=json.loads((args.auxiliary / 'manifest.json').read_text(encoding='utf-8')),
                      bulk=manifest)
        with args.provenance.open('x', encoding='utf-8', newline='\n') as output:
            output.write(json.dumps(record, indent=2) + '\n')
    print(f'SourceFirmwareSha256={digest}')


if __name__ == '__main__':
    main()
