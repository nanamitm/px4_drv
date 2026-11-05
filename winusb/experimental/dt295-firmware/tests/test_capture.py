#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate saved-capture diagnostics with a CRC-bearing PAT and injected faults."""
import sys
from pathlib import Path
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from check_capture import inspect

# 実機のPAT section。映像・音声payloadは含まず、TSID/番組表とCRCだけを保存する。
PAT = bytes.fromhex('00b01d7c11c100000000e010f808e1f0f809e3f0f80ae4f0f988ffc8064bce24')


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'fixture.ts'
        self.packets = [bytes([0x47, 0x40, 0, 0x10 | i, 0]) + PAT + b'\xff' * (183 - len(PAT))
                        for i in range(4)]
        self.raw = b'\x00' * 97 + b''.join(b'\x00' * 4 + p for p in self.packets)

    def check(self, data=None, expected=31761, discarded=97):
        self.path.write_bytes(b''.join(self.packets) if data is None else data)
        Path(str(self.path) + '.prefix.raw').write_bytes(self.raw)
        return inspect(self.path, expected, discarded)

    def test_initial_alignment_separate_from_stable_interval(self):
        result = self.check()
        self.assertTrue(result['stable_interval_ok'])
        self.assertFalse(result['strict_zero_discard_ok'])
        self.assertEqual(result['PAT_valid'], 4)
        self.assertEqual(result['startup_alignment_bytes'], 97)
        self.assertEqual(result['midstream_discarded_bytes'], 0)

    def test_aligned_start_meets_strict_requirement(self):
        self.raw = self.raw[97:]
        self.assertTrue(self.check(discarded=0)['strict_zero_discard_ok'])

    def test_reject_corrupted_ts(self):
        data = bytearray(b''.join(self.packets))
        for offset, mask, metric in [(0, 1, 'sync_errors'), (1, 0x80, 'TEI'),
                                     (188 + 3, 1, 'cc_anomalies'), (5 + len(PAT) - 1, 1, 'PAT_bad_crc')]:
            with self.subTest(metric=metric):
                broken = data.copy()
                broken[offset] ^= mask
                result = self.check(broken)
                self.assertGreater(result[metric], 0)
                self.assertFalse(result['stable_interval_ok'])

    def test_reject_wrong_channel_truncation_and_discard_mismatch(self):
        self.assertFalse(self.check(expected=31760)['stable_interval_ok'])
        self.assertFalse(self.check(b''.join(self.packets) + b'\x00')['stable_interval_ok'])
        self.assertFalse(self.check(discarded=98)['stable_interval_ok'])
        self.assertFalse(self.check(discarded=96)['stable_interval_ok'])


if __name__ == '__main__':
    unittest.main()
