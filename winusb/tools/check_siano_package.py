#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import argparse
import hashlib
from check_msi2500_package import check_helper, check_sources
from check_pxbcud_package import configuration
from check_rtlsdr_package import check_pe
from package_siano import check_j83b_firmware

WINUSB = Path(__file__).resolve().parents[1]
FIRMWARE_SHA256 = "054520642d5d09cb7ab7d08dbd6fd9ba9365de56adf2e7d7d06927f9845ff818"

def check(dist):
    for bits in (32, 64):
        folder = dist / f"BonDriver_PX-S1UD_{bits}bit"
        for name in ("BonDriver_PX-S1UD.dll", "BonDriver_PX-S1UD.ini", "BonDriver_PX4-T.ChSet.txt",
                     "BonDriver_PX4-J83B.ChSet.txt", "DriverHost_PX4.exe", "DriverHost_PX4.ini", "WinSCard.dll",
                     "README.md", "LICENSE"):
            if not (folder / name).is_file():
                raise ValueError(f"Missing PX-S1UD file: {folder / name}")
        ini = (folder / "BonDriver_PX-S1UD.ini").read_bytes()
        if b'73ad569a-2c54-4710-bf5c-812de1f9dc52' not in ini:
            raise ValueError("PX-S1UD BonDriver DeviceGUID mismatch")
        bon = configuration(folder / "BonDriver_PX-S1UD.ini")
        if not bon["BonDriver"].getboolean("FramedStream"):
            raise ValueError("PX-S1UD requires framed TS epochs")
        host = configuration(folder / "DriverHost_PX4.ini")
        device = [s for s in host.sections() if host[s].get("Type", "").strip('"') == "SIANO"]
        if len(device) != 1:
            raise ValueError("Expected one Siano device definition")
        device = device[0]
        if host[device]["GUID"] != bon["ReceiverDefinition0"]["DeviceGUID"]:
            raise ValueError("PX-S1UD BonDriver/host GUID mismatch")
        receiver = host[device + ".Receiver0"]
        # J.83B は既定で有効。BonDriver と DriverHost の System、ChSet、firmware、helper がそろって初めて受信できる
        if receiver["System"].strip('"') != "ISDB-T,J.83B" or receiver.getint("Index") != 0:
            raise ValueError("PX-S1UD must expose one ISDB-T and J.83B receiver")
        for section in ("BonDriver", "ReceiverDefinition0"):
            if bon[section]["System"].strip('"') != "ISDB-T,J.83B":
                raise ValueError(f"PX-S1UD BonDriver {section} must enable J.83B")
        if bon["BonDriver.J.83B"]["ChSetPath"].strip('"') != "BonDriver_PX4-J83B.ChSet.txt":
            raise ValueError("Incorrect PX-S1UD J.83B ChSet path")
        if (folder / "BonDriver_PX4-J83B.ChSet.txt").read_bytes() != (WINUSB / "pkg/BonDriver_PX4/BonDriver_PX4-J83B.ChSet.txt").read_bytes():
            raise ValueError("PX-S1UD J.83B ChSet differs from the source")
        config = host[device + ".Config"]
        for key, expected in (("J83BFirmwarePath", r"firmware\s1ud_j83b.inp"), ("HelperPath", r"msi2500\msi2500_frontend.exe")):
            if config.get(key, "").strip('"') != expected:
                raise ValueError(f"PX-S1UD {key} must be {expected}")
        machine = 0x14C if bits == 32 else 0x8664
        for name in ("BonDriver_PX-S1UD.dll", "DriverHost_PX4.exe", "WinSCard.dll"):
            check_pe(folder / name, machine)
        check_helper(folder / "msi2500", machine)
        check_sources(folder / "Sources")
    for folder in dist.glob("BonDriver_*"):
        if not folder.is_dir():
            continue
        fw = folder / "firmware/isdbt_rio.inp"
        check_j83b_firmware(folder / "firmware/s1ud_j83b.inp")
        if hashlib.sha256(fw.read_bytes()).hexdigest() != FIRMWARE_SHA256:
            raise ValueError(f"PX-S1UD firmware hash mismatch: {fw}")
        for name in ("COPYING", "LICENCE.siano", "NOTICE.md"):
            if (folder / "licenses/siano" / name).read_bytes() != (WINUSB / "pkg/licenses/siano" / name).read_bytes():
                raise ValueError(f"PX-S1UD license mismatch: {folder / name}")
        host = (folder / "DriverHost_PX4.ini").read_bytes()
        if b'Type="SIANO"' not in host or b'ba60b2ee-5f40-40ca-bc5d-b4dd0fc56511' not in host:
            raise ValueError(f"Missing Siano host definition: {folder}")
    inf = (dist / "Driver/PX-S1UD.inf").read_bytes()
    if b"USB\\VID_3275&PID_0080" not in inf or b"ba60b2ee-5f40-40ca-bc5d-b4dd0fc56511" not in inf:
        raise ValueError("PX-S1UD INF hardware ID/interface GUID mismatch")
    if b"\n" in inf.replace(b"\r\n", b"") or inf.startswith(b"\xef\xbb\xbf"):
        raise ValueError("PX-S1UD INF must be CRLF, without BOM")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--dist", type=Path, default=WINUSB / "dist")
    args = parser.parse_args()
    check(args.dist.resolve())
    print("PASS: PX-S1UD firmware, J.83B helper and sources, licenses, package files and device identity")
