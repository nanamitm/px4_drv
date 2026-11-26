"""Exercise missing/corrupt RTL-SDR packages using generated binaries; never load USB."""
import shutil
import struct
import subprocess
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

    def test_missing_vc_runtime(self):
        (self.folder / "rtlsdr/vcruntime140.dll").unlink()
        self.rejected()

    def test_missing_helper(self):
        (self.folder / "rtlsdr/rtlsdr_frontend.exe").unlink()
        self.rejected()

    def test_wrong_helper_architecture(self):
        path = self.folder / "rtlsdr/rtlsdr_frontend.exe"
        data = bytearray(path.read_bytes())
        struct.pack_into("<H", data, struct.unpack_from("<I", data, 0x3C)[0] + 4, 0x14C)
        path.write_bytes(data)
        self.rejected()

    def test_unbundled_helper_dependency(self):
        path = self.folder / "rtlsdr/rtlsdr_frontend.exe"
        data = path.read_bytes()
        self.assertIn(b"KERNEL32.dll\0", data)
        path.write_bytes(data.replace(b"KERNEL32.dll\0", b"missing.dll\0\0"))
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

    def test_missing_path_runtime(self):
        (self.folder / "rtlsdr/rtlsdr_path.dll").unlink()
        self.rejected()

    @unittest.skipUnless(sys.platform == "win32", "requires the Windows RTL-SDR helper")
    def test_unavailable_device_path_never_falls_back_to_serial(self):
        helper = self.folder / "rtlsdr/rtlsdr_frontend.exe"
        with subprocess.Popen(
            [str(helper), "--device-path", "nonexistent-test-device-interface",
             "--serial", "00000001", "--frequency", "473142857"],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        ) as process:
            # Keep stdin open so the stop request cannot mask the USB open error.
            try:
                self.assertEqual(process.wait(timeout=10), 1)
            except subprocess.TimeoutExpired:
                process.kill()
                raise
            self.assertIn(b"Open selected USB interface: librtlsdr error -4", process.stderr.read())

    def test_missing_path_runtime_source(self):
        (self.folder / "Sources/rtl-sdr-blog/src/librtlsdr.c").unlink()
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
