# SPDX-License-Identifier: GPL-2.0-only
"""Reject incomplete firmware distributions without modifying the real dist."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

WINUSB = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(WINUSB / "tools"))
from check_siano_package import check

class PackageTests(unittest.TestCase):
    def setUp(self):
        self.area = tempfile.TemporaryDirectory(dir=WINUSB / "build")
        self.addCleanup(self.area.cleanup)
        self.dist = Path(self.area.name)
        for bits in (32, 64):
            name = f"BonDriver_PX-S1UD_{bits}bit"
            shutil.copytree(WINUSB / "dist" / name, self.dist / name)
        shutil.copytree(WINUSB / "dist/Driver", self.dist / "Driver")
        self.folder = self.dist / "BonDriver_PX-S1UD_64bit"

    def test_complete(self):
        check(self.dist)

    def test_missing_firmware(self):
        (self.folder / "firmware/isdbt_rio.inp").unlink()
        with self.assertRaises(FileNotFoundError):
            check(self.dist)

    def test_tampered_firmware(self):
        (self.folder / "firmware/isdbt_rio.inp").write_bytes(b"wrong firmware")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            check(self.dist)

    def test_license_required(self):
        (self.folder / "licenses/siano/LICENCE.siano").write_text("incomplete")
        with self.assertRaisesRegex(ValueError, "license mismatch"):
            check(self.dist)

    def test_wrong_device(self):
        p = self.dist / "Driver/PX-S1UD.inf"
        p.write_bytes(p.read_bytes().replace(b"PID_0080", b"PID_0085"))
        with self.assertRaisesRegex(ValueError, "hardware ID"):
            check(self.dist)

    def test_requires_framed_stream(self):
        p = self.folder / "BonDriver_PX-S1UD.ini"
        p.write_bytes(p.read_bytes().replace(b"FramedStream=true", b"FramedStream=false"))
        with self.assertRaisesRegex(ValueError, "framed TS"):
            check(self.dist)

    def test_other_host_requires_firmware(self):
        other = self.dist / "BonDriver_Other_64bit"
        shutil.copytree(self.folder, other)
        (other / "firmware/isdbt_rio.inp").unlink()
        with self.assertRaises(FileNotFoundError):
            check(self.dist)

if __name__ == "__main__":
    # Windows PowerShell 5.1 の Stop 設定では stderr の進捗も NativeCommandError になる。
    unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout))
