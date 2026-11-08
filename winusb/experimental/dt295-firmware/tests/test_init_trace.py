#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from extract_init_trace import trace

# 実機で検証した初期化列のハッシュ。値そのものはリポジトリへ収録しない。
VALIDATED_TRACE_SHA256 = '4cfc10fee80f7788b4a8bb8f9370ce2bac8e6cb2bd62a8a61bfe90aa9a5c8098'


class InitTraceTests(unittest.TestCase):
    def test_trace_matches_validated_sequence(self):
        rows = trace(FIRMWARE)
        text = ''.join(json.dumps(row) + '\n' for row in rows)
        self.assertEqual(hashlib.sha256(text.encode()).hexdigest(), VALIDATED_TRACE_SHA256)
        self.assertEqual(len(rows), 143)

    def test_satellite_records_follow_terrestrial_records(self):
        ops = [row['op'] for row in trace(FIRMWARE) if row['op'] != -1]
        first = ops.index(2)
        self.assertTrue(all(op in (0, 128) for op in ops[:first]))
        self.assertTrue(all(op == 2 for op in ops[first:]))

    def test_other_firmware_is_rejected(self):
        for data in (FIRMWARE[:-1], FIRMWARE[:40] + bytes([FIRMWARE[40] ^ 1]) + FIRMWARE[41:], b''):
            with self.assertRaises(ValueError):
                trace(data)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--firmware', type=Path, required=True, help='pkg/DriverHost_PX4/firmware/magicvision-dt295.bin')
    args, remaining = ap.parse_known_args()
    FIRMWARE = args.firmware.read_bytes()
    unittest.main(argv=[sys.argv[0]] + remaining)
