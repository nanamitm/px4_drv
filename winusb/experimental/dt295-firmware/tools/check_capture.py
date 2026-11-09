#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Inspect saved 188-byte TS and raw startup alignment; never opens hardware."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path


def crc(data):
    value = 0xffffffff
    for byte in data:
        value ^= byte << 24
        for _ in range(8):
            value = ((value << 1) ^ (0x04c11db7 if value & 0x80000000 else 0)) & 0xffffffff
    return value


def inspect(path, expected_tsid, discarded, ts_only=False):
    counts = Counter()
    previous = {}
    cc_pids = Counter()
    tsids, programs = set(), set()
    pending = bytearray()
    digest = hashlib.sha256()

    def sections():
        while len(pending) >= 3:
            if pending[0] == 0xff:
                pending.clear()
                break
            size = 3 + ((pending[1] & 15) << 8 | pending[2])
            if not 12 <= size <= 1024 or pending[0] != 0 or not pending[1] & 0x80:
                counts['PAT_malformed'] += 1
                pending.clear()
                break
            if len(pending) < size:
                break
            section = bytes(pending[:size])
            del pending[:size]
            if crc(section):
                counts['PAT_bad_crc'] += 1
                continue
            counts['PAT_valid'] += 1
            tsids.add(int.from_bytes(section[3:5], 'big'))
            for at in range(8, size - 4, 4):
                program = int.from_bytes(section[at:at + 2], 'big')
                if program:
                    programs.add(program)

    with path.open('rb') as stream:
        while block := stream.read(188 * 4096):
            digest.update(block)
            counts['bytes'] += len(block)
            if len(block) % 188:
                counts['trailing_bytes'] += len(block) % 188
            for offset in range(0, len(block) - 187, 188):
                p = block[offset:offset + 188]
                counts['packets'] += 1
                if p[0] != 0x47:
                    counts['sync_errors'] += 1
                    continue
                pid = (p[1] & 31) << 8 | p[2]
                afc = (p[3] >> 4) & 3
                if not afc or (afc & 2 and (p[4] > 183 or (afc & 1 and p[4] > 182))):
                    counts['malformed'] += 1
                    continue
                if p[3] & 0xc0:
                    counts['scrambled'] += 1
                if p[1] & 0x80:
                    counts['TEI'] += 1
                    previous.pop(pid, None)
                    if pid == 0:
                        pending.clear()
                    continue
                if afc & 2 and p[4] and p[5] & 0x80:
                    previous.pop(pid, None)
                if not afc & 1 or pid == 8191:
                    continue
                cc = p[3] & 15
                if pid in previous and cc != (previous[pid] + 1) & 15:
                    counts['cc_anomalies'] += 1
                    cc_pids[pid] += 1
                previous[pid] = cc
                if pid != 0:
                    continue
                start = 5 + p[4] if afc & 2 else 4
                payload = p[start:]
                if p[1] & 0x40:
                    if not payload or payload[0] >= len(payload):
                        counts['PAT_malformed'] += 1
                        pending.clear()
                        continue
                    pointer = payload[0]
                    if pending:
                        pending.extend(payload[1:1 + pointer])
                        sections()
                    pending.clear()
                    payload = payload[1 + pointer:]
                elif not pending:
                    continue
                pending.extend(payload)
                sections()

    raw = b'' if ts_only else Path(str(path) + '.prefix.raw').read_bytes()
    first = None
    for offset in range(max(0, len(raw) - 3 * 192 + 1)):
        if all(raw[offset + 4 + n * 192] == 0x47 and
               not raw[offset + 5 + n * 192] & 0x80 and
               raw[offset + 7 + n * 192] & 0x30 for n in range(3)):
            first = offset
            break
    prefix_sync_errors = None if first is None else sum(
        raw[i + 4] != 0x47 for i in range(first, len(raw) - 191, 192))
    midstream = None if first is None or discarded is None else discarded - first
    metrics = {key: counts[key] for key in ['bytes', 'packets', 'trailing_bytes', 'sync_errors',
        'TEI', 'malformed', 'cc_anomalies', 'scrambled', 'PAT_valid', 'PAT_bad_crc', 'PAT_malformed']}
    packets_ok = (counts['packets'] > 0 and counts['PAT_valid'] > 0 and tsids == {expected_tsid}
        and not any(counts[key] for key in ['trailing_bytes', 'sync_errors', 'TEI', 'malformed',
            'cc_anomalies', 'PAT_bad_crc', 'PAT_malformed']))
    stable = None if ts_only else packets_ok and midstream == 0 and prefix_sync_errors == 0
    return dict(file=path.name, **metrics, TSIDs=sorted(tsids), programs=sorted(programs),
        cc_pids={f'{pid:04x}': count for pid, count in cc_pids.items()}, sha256=digest.hexdigest(),
        startup_alignment_bytes=first, total_discarded_bytes=discarded,
        midstream_discarded_bytes=midstream, prefix_sync_errors=prefix_sync_errors,
        packet_stream_ok=packets_ok, stable_interval_ok=stable,
        strict_zero_discard_ok=None if ts_only else stable and discarded == 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('ts', type=Path)
    ap.add_argument('--expected-tsid', type=int, required=True)
    ap.add_argument('--discarded-bytes', type=int, help='exact capture log counter; required with raw prefix')
    ap.add_argument('--ts-only', action='store_true',help='inspect normalized BonDriver TS; no raw alignment/byte-discard claim')
    ap.add_argument('--strict-zero-discard', action='store_true')
    args = ap.parse_args()
    if (args.discarded_bytes is not None and args.discarded_bytes < 0) or not 0 <= args.expected_tsid <= 65535:
        ap.error('invalid TSID/discarded byte count')
    if (not args.ts_only and args.discarded_bytes is None) or (args.ts_only and args.strict_zero_discard):
        ap.error('raw-prefix validation needs --discarded-bytes; --ts-only cannot prove strict zero-discard')
    result = inspect(args.ts, args.expected_tsid, args.discarded_bytes,args.ts_only)
    print(json.dumps(result, indent=2))
    criterion='packet_stream_ok' if args.ts_only else ('strict_zero_discard_ok' if args.strict_zero_discard else 'stable_interval_ok')
    raise SystemExit(0 if result[criterion] else 1)


if __name__ == '__main__':
    main()
