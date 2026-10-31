#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Stage PX-S1UD and the firmware required by the common DriverHost."""
from pathlib import Path
import argparse
import hashlib
import shutil

WINUSB = Path(__file__).resolve().parents[1]
FIRMWARE_SHA256 = "054520642d5d09cb7ab7d08dbd6fd9ba9365de56adf2e7d7d06927f9845ff818"

def stage(dist):
    firmware = WINUSB / "pkg/DriverHost_PX4/firmware/isdbt_rio.inp"
    if hashlib.sha256(firmware.read_bytes()).hexdigest() != FIRMWARE_SHA256:
        raise ValueError("PX-S1UD firmware hash mismatch")
    for bits, platform in ((32, "x86"), (64, "x64")):
        folder = dist / f"BonDriver_PX-S1UD_{bits}bit"
        folder.mkdir(parents=True, exist_ok=True)
        binaries = WINUSB / "build" / platform / "Release-static"
        for source, name in (
            (binaries / "BonDriver_PX4.dll", "BonDriver_PX-S1UD.dll"),
            (binaries / "DriverHost_PX4.exe", "DriverHost_PX4.exe"),
            (binaries / "WinSCard.dll", "WinSCard.dll"),
            (WINUSB / "pkg/BonDriver_PX4/BonDriver_PX-S1UD.ini", "BonDriver_PX-S1UD.ini"),
            (WINUSB / "pkg/BonDriver_PX4/BonDriver_PX4-T.ChSet.txt", "BonDriver_PX4-T.ChSet.txt"),
            (WINUSB / "pkg/DriverHost_PX4/DriverHost_PX4.ini", "DriverHost_PX4.ini"),
            (WINUSB / "docs/px-s1ud.md", "README.md"),
            (WINUSB.parent / "LICENSE", "LICENSE"),
        ):
            shutil.copy2(source, folder / name)
    # どの機種のフォルダーから起動しても同じホストが PX-S1UD を管理する。
    # firmware と出典・再配布条件をそのホストと一緒に配置する。
    for folder in [dist, *(p for p in dist.glob("BonDriver_*") if p.is_dir())]:
        licenses = folder / "licenses/siano"
        licenses.mkdir(parents=True, exist_ok=True)
        for source in (WINUSB / "pkg/licenses/siano").iterdir():
            shutil.copy2(source, licenses / source.name)
        if folder != dist:
            (folder / "firmware").mkdir(exist_ok=True)
            shutil.copy2(firmware, folder / "firmware/isdbt_rio.inp")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--dist", type=Path, default=WINUSB / "dist")
    args = parser.parse_args()
    stage(args.dist.resolve())
    print("PASS: PX-S1UD packages, pinned firmware and license notices")
