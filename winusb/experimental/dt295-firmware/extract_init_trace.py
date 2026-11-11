#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate the PIX-DT295 frontend initialization trace from the bundled mAgicVISION firmware.

The register values are not stored in this repository. They are read on the user's
machine from the mAgicVISION 0.1.0 container that the distribution already ships under
its upstream redistribution permission. The tool never accesses a device.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

BUNDLE_SHA256 = 'd9f549b29d286c2ff1e7daf4237e2205bb8a5a2affbb98362bce137b8b42a33f'
RESIDENT_SHA256 = '17148723122fe132d3f5438e9e3192eefa0b734aba6403348bbd9ba796bdd941'
RESIDENT_VA = 0x80380000
# 0.1.0の常駐コードが復調器・RF初期化に使うコマンド列。134レコード、992 bytes。
SCRIPT_VA, SCRIPT_BYTES, SCRIPT_RECORDS = 0x803ACB8C, 992, 134
# 地デジ初期化の末尾で、表の先頭にある3レジスターを同じ値で書き直す。
# pix-userlandの地デジ初期化手順と同じ順序で、実機検証済みの初期化列と一致させる。
TERRESTRIAL_REWRITE = (0x25, 0x23, 0x03)
# DriverHostが復調器リセット後に置く20ms・5msの待機と同じ値。検証済みの初期化列と一致させるため残す。
LEADING_DELAYS_MS = (20, 5)


def records(bundle):
    if hashlib.sha256(bundle).hexdigest() != BUNDLE_SHA256:
        raise ValueError('unsupported mAgicVISION firmware; 0.1.0 (SHA-256 d9f549b2...) is required')
    offset, size = struct.unpack_from('>II', bundle, 40)
    resident = bundle[offset:offset + size]
    if hashlib.sha256(resident).hexdigest() != RESIDENT_SHA256:
        raise ValueError('resident image SHA-256 mismatch')
    start = SCRIPT_VA - RESIDENT_VA
    script = resident[start:start + SCRIPT_BYTES]
    result, cursor = [], 0
    while cursor < len(script):
        if len(script) - cursor < 4:
            raise ValueError('truncated record header')
        op, length = script[cursor], script[cursor + 1]
        delay = struct.unpack_from('>H', script, cursor + 2)[0]
        payload = script[cursor + 4:cursor + 4 + length]
        if op not in (0, 2, 128) or not 1 <= length <= 8 or len(payload) != length or delay > 255:
            raise ValueError(f'unexpected record at script offset {cursor}')
        result.append((op, payload, delay))
        cursor += 4 + length
    if len(result) != SCRIPT_RECORDS:
        raise ValueError('unexpected record count')
    return result


def trace(bundle):
    rows = [{'op': -1, 'delay_ms': ms} for ms in LEADING_DELAYS_MS]
    source = records(bundle)
    first_satellite = next(i for i, (op, _, _) in enumerate(source) if op == 2)
    if any(op != 2 for op, _, _ in source[first_satellite:]):
        raise ValueError('terrestrial record after satellite initialization')
    values = {}
    for op, payload, _ in source[:first_satellite]:
        if op == 0 and len(payload) == 2:
            values.setdefault(payload[0], payload[1])
    if any(reg not in values for reg in TERRESTRIAL_REWRITE):
        raise ValueError('terrestrial register missing from initialization table')
    for index, (op, payload, delay) in enumerate(source):
        if index == first_satellite:
            rows.extend({'op': 0, 'payload': bytes((reg, values[reg])).hex(' ')} for reg in TERRESTRIAL_REWRITE)
        rows.append({'op': op, 'payload': payload.hex(' ')})
        if delay:
            rows.append({'op': -1, 'delay_ms': delay})
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware', type=Path, help='magicvision-dt295.bin (mAgicVISION 0.1.0)')
    parser.add_argument('output', type=Path, help='new JSONL file; an existing file is not overwritten')
    args = parser.parse_args()
    try:
        rows = trace(args.firmware.read_bytes())
    except ValueError as error:
        parser.error(str(error))
    text = ''.join(json.dumps(row) + '\n' for row in rows)
    with args.output.open('x', encoding='utf-8', newline='\n') as output:
        output.write(text)
    print(f'rows={len(rows)} sha256={hashlib.sha256(text.encode()).hexdigest()}', file=sys.stderr)


if __name__ == '__main__':
    main()
