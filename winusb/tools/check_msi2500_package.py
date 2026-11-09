"""Verify complete x86/x64 MSi2500 (RSP1 compatible) packages without loading a DLL or opening USB."""
import argparse
import re
from pathlib import Path

from check_pxbcud_package import configuration, require
from check_rtlsdr_package import check_pe, imported_dlls

WINUSB = Path(__file__).resolve().parents[1]
FRONTEND = WINUSB / "tools/msi2500_frontend"
DSP = WINUSB / "tools/rtlsdr_frontend/vendor/isdbt-dsp"
# helper は静的 CRT でリンクし、OS 標準の DLL 以外を使わない
SYSTEM_DLLS = {"kernel32.dll", "kernelbase.dll", "ntdll.dll", "advapi32.dll", "bcrypt.dll", "bcryptprimitives.dll",
               "userenv.dll", "ws2_32.dll", "synchronization.dll", "setupapi.dll", "winusb.dll", "cfgmgr32.dll"}


def same_tree(original, copy, label):
    for file in original.rglob("*"):
        if file.is_file():
            name = file.relative_to(original)
            require((copy / name).is_file() and (copy / name).read_bytes() == file.read_bytes(), f"{label} source mismatch: {name}")


def check_package(folder, bits):
    machine = 0x14C if bits == 32 else 0x8664
    for name in ("BonDriver_MSI2500.dll", "DriverHost_PX4.exe", "WinSCard.dll", "msi2500/msi2500_frontend.exe"):
        check_pe(folder / name, machine)
    runtime = folder / "msi2500"
    actual = {p.name for p in runtime.iterdir() if p.suffix.lower() in (".exe", ".dll")}
    require(actual == {"msi2500_frontend.exe"}, f"Unexpected or missing helper binaries: {folder}: {actual}")
    for dll in imported_dlls(runtime / "msi2500_frontend.exe"):
        require(dll in SYSTEM_DLLS or dll.startswith(("api-ms-win-", "ext-ms-win-")), f"Unbundled helper dependency: {dll}")
    require((runtime / "COPYING.frontend").read_bytes() == (FRONTEND / "LICENSE").read_bytes(), "Incorrect frontend license")
    require((folder / "LICENSE").read_bytes() == (WINUSB.parent / "LICENSE").read_bytes(), "Incorrect driver license")
    for name in ("README.md", "TECHNICAL.md", "TECHNICAL-J83B.md", "BonDriver_MSI2500.ChSet.txt", "BonDriver_PX4-J83B.ChSet.txt", "it930x-firmware.bin"):
        require((folder / name).is_file(), f"Missing MSi2500 package file: {folder / name}")
    require("(TECHNICAL-J83B.md)" in (folder / "README.md").read_text(encoding="utf-8"), "Broken packaged J.83B design link")
    require("(TECHNICAL.md)" in (folder / "TECHNICAL-J83B.md").read_text(encoding="utf-8"), "Broken packaged full-seg design link")
    require((folder / "BonDriver_PX4-J83B.ChSet.txt").read_bytes() == (WINUSB / "pkg/BonDriver_PX4/BonDriver_PX4-J83B.ChSet.txt").read_bytes(),
            "J.83B ChSet differs from the source")
    require("(TECHNICAL.md)" in (folder / "README.md").read_text(encoding="utf-8"), "Broken packaged installation guide link")

    bon = configuration(folder / "BonDriver_MSI2500.ini")
    require(bon["BonDriver"]["system"].strip('"') == "ISDB-T,J.83B", "Incorrect MSi2500 system")
    require(bon["BonDriver"].getboolean("framedstream"), "MSi2500 requires framed stream epochs")
    require(bon["BonDriver.ISDB-T"]["chsetpath"].strip('"') == "BonDriver_MSI2500.ChSet.txt", "Incorrect MSi2500 ChSet path")
    require(bon["BonDriver.J.83B"]["chsetpath"].strip('"') == "BonDriver_PX4-J83B.ChSet.txt", "Incorrect MSi2500 J.83B ChSet path")
    require(bon["ReceiverDefinition0"]["system"].strip('"') == "ISDB-T,J.83B", "Incorrect MSi2500 receiver systems")
    host = configuration(folder / "DriverHost_PX4.ini")
    devices = [section for section in host if re.fullmatch(r"DeviceDefinition\d+", section)
               and host[section].get("type", "").strip('"') == "MSI2500"]
    require(len(devices) == 1, "Expected one MSi2500 device definition")
    device = devices[0]
    require(host[device]["guid"] == bon["ReceiverDefinition0"]["deviceguid"], "MSi2500 device GUID mismatch")
    # SDRplay のドライバーが登録するインターフェース GUID。独自の INF は同梱しない
    require(host[device]["deviceinterfaceguid"].strip('"').lower() == "{2d93e8f2-bc67-4f7d-8891-574830aa637d}",
            "Unexpected MSi2500 interface GUID")
    require(host[device + ".Config"]["helperpath"].strip('"') == r"msi2500\msi2500_frontend.exe", "Incorrect helper path")
    require(0 <= int(host[device + ".Config"]["gaindbj83b"]) <= 102, "Incorrect MSi2500 J.83B gain")
    require(host[device + ".Receiver0"]["system"].strip('"') == "ISDB-T,J.83B", "Incorrect MSi2500 DriverHost receiver systems")

    sources = folder / "Sources"
    frontend = sources / "winusb/tools/msi2500_frontend"
    for name in ("Cargo.toml", "Cargo.lock", "cargo-config.toml", "build.rs", "frontend.rc", "LICENSE"):
        require((frontend / name).read_bytes() == (FRONTEND / name).read_bytes(), f"Frontend source differs from build source: {name}")
    # module 宣言だけが残った欠損ソースを配布しないよう、試験を含む全ソースを照合する。
    same_tree(FRONTEND / "src", frontend / "src", "Frontend")
    same_tree(FRONTEND / "tests", frontend / "tests", "Frontend test")
    same_tree(DSP, sources / "winusb/tools/rtlsdr_frontend/vendor/isdbt-dsp", "isdbt-dsp")
    require((sources / "winusb/tools/rtlsdr_frontend/vendor/README.md").is_file(), "Missing isdbt-dsp provenance")
    config = (sources / ".cargo/config.toml").read_text(encoding="utf-8")
    require('directory = "vendor"' in config and 'replace-with = "vendored-sources"' in config, "Missing offline Cargo source configuration")
    require("target-feature=+crt-static" in config, "Missing static CRT configuration")
    lock = (frontend / "Cargo.lock").read_text(encoding="utf-8")
    for package in re.split(r"(?m)^\[\[package\]\]", lock):
        if 'source = "registry+' not in package:
            continue
        name = re.search(r'(?m)^name = "([^"]+)"', package)[1]
        version = re.search(r'(?m)^version = "([^"]+)"', package)[1]
        vendor = sources / "vendor" / f"{name}-{version}"
        require((vendor / "Cargo.toml").is_file() and (vendor / ".cargo-checksum.json").is_file(), f"Missing vendored Cargo dependency: {name}-{version}")
    require((sources / "winusb/src/common/version.h").read_bytes() == (WINUSB / "src/common/version.h").read_bytes(),
            "Frontend version source mismatch")


def check_distribution(dist):
    reference = None
    for bits in (32, 64):
        folder = dist / f"BonDriver_MSI2500_{bits}bit"
        check_package(folder, bits)
        host = (folder / "DriverHost_PX4.ini").read_bytes()
        require(reference is None or host == reference, "MSi2500 architecture configurations differ")
        reference = host
        print(f"PASS: MSi2500 {bits}bit DLL, helper and corresponding offline sources")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, default=WINUSB / "dist")
    check_distribution(parser.parse_args().dist)
