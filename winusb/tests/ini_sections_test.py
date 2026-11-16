# SPDX-License-Identifier: GPL-2.0-only
"""Check that key-less INI sections are rejected and normal ones pass."""
from pathlib import Path
import sys
import tempfile
import unittest

WINUSB = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(WINUSB / "tools"))
from check_ini_sections import check, empty_sections


class IniSectionTests(unittest.TestCase):
    def write(self, text, encoding="cp932"):
        area = tempfile.TemporaryDirectory()
        self.addCleanup(area.cleanup)
        path = Path(area.name) / "DriverHost_PX4.ini"
        path.write_bytes(text.replace("\n", "\r\n").encode(encoding))
        return path

    def test_comment_only_section(self):
        path = self.write('[Device]\nName="x"\n\n[Device.Config]\n; 説明だけ\n\n[Device.Receiver0]\nIndex=0\n')
        self.assertEqual(empty_sections(path), ["Device.Config"])
        with self.assertRaises(ValueError):
            check([path])

    def test_last_section_empty(self):
        path = self.write('[Device]\nName="x"\n[Device.Config]\n')
        self.assertEqual(empty_sections(path), ["Device.Config"])

    def test_hash_comment_counts_as_line(self):
        # GetPrivateProfileSectionW() は # の行を返すため、Config::Load() は成功する
        path = self.write('[Device.Config]\n# note\n')
        self.assertEqual(empty_sections(path), [])

    def test_shipped_host_ini(self):
        check([WINUSB / "pkg/DriverHost_PX4/DriverHost_PX4.ini"])


if __name__ == "__main__":
    unittest.main()
