"""Regression tests for rejecting incomplete or incorrectly configured packages."""
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
import importlib.util
import shutil
import sys
import tempfile
import unittest

winusb = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("pxbcud_package", winusb / "tools/check_pxbcud_package.py")
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)


class PackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pxbcud-package-", dir=winusb / "build")
        self.addCleanup(self.temporary.cleanup)
        self.dist = Path(self.temporary.name)
        for name in ("Driver", "BonDriver_PX-BCUD_32bit", "BonDriver_PX-BCUD_64bit"):
            shutil.copytree(winusb / "dist" / name, self.dist / name)

    def validate(self):
        with redirect_stdout(StringIO()):
            validator.check_distribution(self.dist)

    def test_valid(self):
        self.validate()

    def test_missing_inf(self):
        (self.dist / "Driver/PX-BCUD.inf").unlink()
        with self.assertRaises(FileNotFoundError):
            self.validate()

    def test_wrong_architecture(self):
        shutil.copyfile(self.dist / "BonDriver_PX-BCUD_32bit/BonDriver_PX-BCUD.dll",
                        self.dist / "BonDriver_PX-BCUD_64bit/BonDriver_PX-BCUD.dll")
        with self.assertRaisesRegex(ValueError, "Incorrect architecture"):
            self.validate()

    def test_voltage_enabled(self):
        path = self.dist / "BonDriver_PX-BCUD_64bit/BonDriver_PX-BCUD.ini"
        path.write_bytes(path.read_bytes().replace(b"LNBPower=0", b"LNBPower=1"))
        with self.assertRaisesRegex(ValueError, "cannot supply LNB"):
            self.validate()

    def test_stream_epochs_disabled(self):
        path = self.dist / "BonDriver_PX-BCUD_64bit/BonDriver_PX-BCUD.ini"
        path.write_bytes(path.read_bytes().replace(b"FramedStream=true", b"FramedStream=false"))
        with self.assertRaisesRegex(ValueError, "requires framed TS"):
            self.validate()

    def test_device_definition_missing(self):
        path = self.dist / "BonDriver_PX-BCUD_64bit/DriverHost_PX4.ini"
        path.write_bytes(path.read_bytes().replace(b'Type="PXBCUD"', b'Type="UNKNOWN"'))
        with self.assertRaisesRegex(ValueError, "exactly one PX-BCUD"):
            self.validate()

    def test_shared_config_differs(self):
        path = self.dist / "BonDriver_PX-BCUD_32bit/DriverHost_PX4.ini"
        path.write_bytes(path.read_bytes() + b"\r\n; unexpected package-local change\r\n")
        with self.assertRaisesRegex(ValueError, "Shared device definitions differ"):
            self.validate()


if __name__ == "__main__":
    # 成功時の stderr 出力を古い PowerShell がエラー扱いしないようにする。
    unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout))
