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

    def test_missing_j83b_firmware(self):
        for bits in (32, 64):
            with self.subTest(bits=bits):
                p = self.dist / f"BonDriver_PX-S1UD_{bits}bit/firmware/s1ud_j83b.inp"
                original = p.read_bytes()
                p.unlink()
                with self.assertRaises(FileNotFoundError):
                    check(self.dist)
                p.write_bytes(original)

    def test_external_j83b_firmware_rejected(self):
        (self.folder / "firmware/s1ud_j83b.inp").write_bytes(b"external firmware")
        with self.assertRaisesRegex(ValueError, "J.83B firmware hash mismatch"):
            check(self.dist)

    def test_other_host_requires_j83b_firmware(self):
        other = self.dist / "BonDriver_Other_64bit"
        shutil.copytree(self.folder, other)
        (other / "firmware/s1ud_j83b.inp").unlink()
        with self.assertRaises(FileNotFoundError):
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

    def test_missing_helper(self):
        (self.folder / "msi2500/msi2500_frontend.exe").unlink()
        with self.assertRaises(FileNotFoundError):
            check(self.dist)

    def test_helper_license_required(self):
        (self.folder / "msi2500/COPYING.frontend").write_text("incomplete")
        with self.assertRaisesRegex(ValueError, "frontend license"):
            check(self.dist)

    def test_helper_sources_required(self):
        source = self.folder / "Sources/winusb/tools/msi2500_frontend/src/s1ud.rs"
        source.write_bytes(source.read_bytes() + b"\n// modified\n")
        with self.assertRaisesRegex(ValueError, "Frontend source mismatch"):
            check(self.dist)

    def test_missing_j83b_chset(self):
        (self.folder / "BonDriver_PX4-J83B.ChSet.txt").unlink()
        with self.assertRaisesRegex(ValueError, "Missing PX-S1UD file"):
            check(self.dist)

    def test_j83b_enabled_in_bondriver(self):
        p = self.folder / "BonDriver_PX-S1UD.ini"
        # 最初の System は [BonDriver] の行
        p.write_bytes(p.read_bytes().replace(b'System="ISDB-T,J.83B"', b'System="ISDB-T"', 1))
        with self.assertRaisesRegex(ValueError, "must enable J.83B"):
            check(self.dist)

    def test_j83b_enabled_in_host(self):
        p = self.folder / "DriverHost_PX4.ini"
        p.write_bytes(p.read_bytes().replace(b'\nJ83BFirmwarePath=', b'\n;J83BFirmwarePath='))
        with self.assertRaisesRegex(ValueError, "J83BFirmwarePath"):
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
