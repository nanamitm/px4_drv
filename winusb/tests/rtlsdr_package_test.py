"""Exercise missing/corrupt RTL-SDR packages using generated binaries; never load USB."""
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_rtlsdr_package import WINUSB, check_package


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="px4-rtlsdr-package-")
        self.addCleanup(self.scratch.cleanup)
        self.folder = Path(self.scratch.name) / "package"
        shutil.copytree(WINUSB / "dist/BonDriver_RTLSDR_64bit", self.folder)

    def rejected(self):
        with self.assertRaises((ValueError, FileNotFoundError)):
            check_package(self.folder, 64)

    def test_complete_package(self):
        check_package(self.folder, 64)

    def test_missing_helper(self):
        (self.folder / "rtlsdr/rtlsdr_frontend.exe").unlink()
        self.rejected()

    def test_wrong_helper_architecture(self):
        path = self.folder / "rtlsdr/rtlsdr_frontend.exe"
        data = bytearray(path.read_bytes())
        struct.pack_into("<H", data, struct.unpack_from("<I", data, 0x3C)[0] + 4, 0x14C)
        path.write_bytes(data)
        self.rejected()

    def test_corrupt_dependency(self):
        path = self.folder / "rtlsdr/rtlsdr.dll"
        data = bytearray(path.read_bytes()); data[-1] ^= 1; path.write_bytes(data)
        self.rejected()

    def test_unexpected_runtime_executable(self):
        shutil.copyfile(self.folder / "rtlsdr/rtlsdr_frontend.exe", self.folder / "rtlsdr/unexpected.exe")
        self.rejected()

    def test_missing_offline_sources(self):
        (self.folder / "Sources/.cargo/config.toml").unlink()
        self.rejected()

    def test_missing_dsp_source(self):
        (self.folder / "Sources/winusb/tools/rtlsdr_frontend/vendor/isdbt-dsp/src/demod.rs").unlink()
        self.rejected()

    def test_missing_runtime_source(self):
        (self.folder / "Sources/rtl-sdr-blog-V1.4.0.zip").unlink()
        self.rejected()

    def test_wrong_helper_path(self):
        path = self.folder / "DriverHost_PX4.ini"
        data = path.read_bytes().replace(b'rtlsdr\\rtlsdr_frontend.exe', b'rtlsdr_frontend.exe')
        path.write_bytes(data)
        self.rejected()


if __name__ == "__main__":
    unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout))
