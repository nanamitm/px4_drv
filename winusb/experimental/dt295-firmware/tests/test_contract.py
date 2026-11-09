#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import boot_contract as c


class ContractTests(unittest.TestCase):
    def test_known_reference_and_nonexecutable_plan(self):
        result=c.plan(BUILD,SNAPSHOT)
        self.assertFalse(result['executable'])
        self.assertEqual(result['segments']['hook']['bytes'],8)
        self.assertEqual(result['diagnostic_exchange']['out']['value'],'0x0c7e')

    def test_unknown_or_prepatched_boot_rejected(self):
        for snapshot in (SNAPSHOT[:-1],SNAPSHOT+b'\0',bytes(len(SNAPSHOT))):
            with self.assertRaises(ValueError):c.validate_boot(snapshot)
        snapshot=bytearray(SNAPSHOT);snapshot[0x16fc:0x1704]=c.SOURCE_HOOK
        with self.assertRaises(ValueError):c.validate_boot(snapshot)

    def test_artifact_corruption_rejected(self):
        for filename in ('resident.bin','bootstrap.bin','boot-hook.bin'):
            with tempfile.TemporaryDirectory() as temp:
                for source in BUILD.iterdir():
                    if source.is_file():shutil.copyfile(source,Path(temp)/source.name)
                target=Path(temp)/filename
                target.write_bytes(target.read_bytes()+b'\0')
                with self.assertRaises(ValueError):c.inspect_build(temp)

    def test_manifest_cannot_claim_hardware_ready(self):
        with tempfile.TemporaryDirectory() as temp:
            for source in BUILD.iterdir():
                if source.is_file():shutil.copyfile(source,Path(temp)/source.name)
            target=Path(temp)/'manifest.json'
            manifest=json.loads(target.read_text());manifest['deployable']=True
            target.write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):c.inspect_build(temp)


if __name__=='__main__':
    ap=argparse.ArgumentParser()
    ap.add_argument('--build',type=Path,required=True)
    ap.add_argument('--boot-snapshot',type=Path,required=True)
    args,remaining=ap.parse_known_args()
    BUILD=args.build
    SNAPSHOT=args.boot_snapshot.read_bytes()
    unittest.main(argv=[sys.argv[0]]+remaining)
