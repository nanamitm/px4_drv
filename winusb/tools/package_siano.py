#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Stage PX-S1UD and the firmware required by the common DriverHost."""
from pathlib import Path
import argparse
import hashlib
import shutil

WINUSB = Path(__file__).resolve().parents[1]
FIRMWARE_SHA256 = "054520642d5d09cb7ab7d08dbd6fd9ba9365de56adf2e7d7d06927f9845ff818"
J83B_FIRMWARE_SHA256 = "f4bb6b499fe96d6aa8061618f6377241bf66d5afd6830a34c1d20664e4e4b8ce"


def check_j83b_firmware(firmware):
    # 別の外部ファームウェアや古い生成物を配布しないよう、ソースビルドの結果を固定する。
    if hashlib.sha256(firmware.read_bytes()).hexdigest() != J83B_FIRMWARE_SHA256:
        raise ValueError(f"PX-S1UD J.83B firmware hash mismatch: {firmware}")

def stage(dist):
    firmware = WINUSB / "pkg/DriverHost_PX4/firmware/isdbt_rio.inp"
    j83b_firmware = WINUSB / "build/s1ud/s1ud_j83b.inp"
    check_j83b_firmware(j83b_firmware)
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
            (WINUSB / "pkg/BonDriver_PX4/BonDriver_PX4-J83B.ChSet.txt", "BonDriver_PX4-J83B.ChSet.txt"),
            (WINUSB / "pkg/DriverHost_PX4/DriverHost_PX4.ini", "DriverHost_PX4.ini"),
            (WINUSB / "docs/px-s1ud.md", "README.md"),
            (WINUSB.parent / "LICENSE", "LICENSE"),
        ):
            shutil.copy2(source, folder / name)
        # J.83B を既定で有効にするため、MSi2500 と同じ復調用の helper を同梱する。helper は GPL-3.0 のため、
        # MSi2500 の配布物と同じ対応ソース (Sources) も一緒に置く。build.ps1 は MSi2500 を先に作る
        msi2500 = dist / f"BonDriver_MSI2500_{bits}bit"
        helper = msi2500 / "msi2500/msi2500_frontend.exe"
        if not helper.is_file() or not (msi2500 / "Sources").is_dir():
            raise FileNotFoundError(f"Stage the MSi2500 package first: {msi2500}")
        (folder / "msi2500").mkdir(exist_ok=True)
        for name in ("msi2500_frontend.exe", "COPYING.frontend"):
            shutil.copy2(msi2500 / "msi2500" / name, folder / "msi2500" / name)
        if (folder / "Sources").exists():
            shutil.rmtree(folder / "Sources")
        shutil.copytree(msi2500 / "Sources", folder / "Sources")
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
            shutil.copy2(j83b_firmware, folder / "firmware/s1ud_j83b.inp")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--dist", type=Path, default=WINUSB / "dist")
    args = parser.parse_args()
    stage(args.dist.resolve())
    print("PASS: PX-S1UD packages, pinned firmware, J.83B helper and license notices")
