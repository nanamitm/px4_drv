#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import argparse
from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from extract_reference_aux import decompress, extract


class ReferenceTests(unittest.TestCase):
    def test_literal_and_overlapping_backreference(self):
        self.assertEqual(decompress(b'\x02abc\x90\x02',7),b'abcabca')

    def test_bad_compressed_input_is_rejected(self):
        for data,size in ((b'\x80',3),(b'\x80\0',3),(b'\x02ab',3),(b'\0a',0),(b'\0a',2)):
            with self.assertRaises(ValueError):decompress(data,size)

    def test_pinned_reference_size_checksum_and_entry(self):
        data,meta=extract(REFERENCE)
        self.assertEqual(len(data),8168)
        self.assertEqual(meta['sha256'],'7bf697cc4ef4867f6dfe89a2c3c9103bd9a158b950e584e566e2e965131d9f76')
        self.assertEqual(data[:2],b'\x83\xef')

    def test_other_reference_is_rejected(self):
        with self.assertRaises(ValueError):extract(REFERENCE[:-1])


if __name__=='__main__':
    ap=argparse.ArgumentParser()
    ap.add_argument('--reference',type=Path,required=True)
    args,remaining=ap.parse_known_args()
    REFERENCE=args.reference.read_bytes()
    unittest.main(argv=[sys.argv[0]]+remaining)
