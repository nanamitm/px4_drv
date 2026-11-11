#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify the GV-MVP/XZ3 WinUSB distribution without loading its binaries."""
import argparse
from pathlib import Path
import re
from uuid import UUID

from check_pxbcud_package import configuration
from check_rtlsdr_package import check_pe

WINUSB = Path(__file__).resolve().parents[1]
OLD_INTERFACE = UUID("dee824ef-729b-4a0e-9c14-b7117d33a817")
USB_ID = r"USB\VID_04BB&PID_0530"


def check(dist):
    inf_path = dist / "Driver/GV-MVP-XZ3.inf"
    inf = inf_path.read_text(encoding="ascii")
    for arch in ("NTx86", "NTamd64", "NTarm64"):
        section = f"[DeviceList.{arch}]\n%DeviceName% = WinUSBInstall, {USB_ID}"
        if section not in inf:
            raise ValueError(f"XZ3 USB ID is missing for {arch}: {inf_path}")
    match = re.search(r'(?im)^HKR,,DeviceInterfaceGUIDs,0x10000,"(\{[0-9a-f-]+\})"$', inf)
    if not match or UUID(match[1]) == OLD_INTERFACE:
        raise ValueError(f"Dedicated WinUSB GUID is missing: {inf_path}")

    firmware = WINUSB / "pkg/DriverHost_PX4/firmware"
    images = {name: (firmware / name).read_bytes()
              for name in ("xz3-bootstrap.bin", "xz3-runtime.bin", "xz3-hook.bin")}
    for bits, machine in ((32, 0x14C), (64, 0x8664)):
        folder = dist / f"BonDriver_XZ3_{bits}bit"
        for name in ("BonDriver_XZ3.dll", "DriverHost_PX4.exe", "WinSCard.dll"):
            check_pe(folder / name, machine)
        executable = (folder / "DriverHost_PX4.exe").read_bytes()
        for name, payload in images.items():
            if payload not in executable or (folder / "firmware" / name).exists():
                raise ValueError(f"XZ3 image must be embedded in DriverHost: {name}")
        for name in ("it930x-firmware.bin", "firmware/isdbt_rio.inp", "licenses/siano",
                     "README.md", "LICENSE"):
            if not (folder / name).exists():
                raise ValueError(f"Missing package dependency: {folder / name}")

        host = configuration(folder / "DriverHost_PX4.ini")
        section, = (key for key in host.sections() if host[key].get("Type", "").strip('"') == "GVMVPXZ3")
        if UUID(host[section]["DeviceInterfaceGUID"].strip('"')) != UUID(match[1]):
            raise ValueError(f"XZ3 INF and DriverHost GUID do not match: {folder}")
        if not host[section + ".Config"].getboolean("Experimental"):
            raise ValueError(f"XZ3 is disabled in the distribution: {folder}")

        bon = configuration(folder / "BonDriver_XZ3.ini")
        if (not bon["BonDriver"].getboolean("FramedStream") or
                bon["BonDriver"]["System"].strip('"') != "ISDB-T,ISDB-S" or
                bon["ReceiverDefinition0"]["DeviceGUID"] != host[section]["GUID"]):
            raise ValueError(f"XZ3 BonDriver configuration does not match DriverHost: {folder}")
        for system in ("ISDB-T", "ISDB-S"):
            channels = bon[f"BonDriver.{system}"]["ChSetPath"].strip('"')
            if not (folder / channels).is_file():
                raise ValueError(f"Missing channel definitions: {folder / channels}")
        print(f"PASS: GV-MVP/XZ3 {bits}-bit package, INF, configuration and embedded firmware")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, default=WINUSB / "dist")
    check(parser.parse_args().dist)

