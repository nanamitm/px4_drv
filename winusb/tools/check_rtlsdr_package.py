"""Verify complete x86/x64 RTL-SDR packages without loading a DLL or opening USB."""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

from check_pxbcud_package import configuration, require

WINUSB = Path(__file__).resolve().parents[1]
LOCK = json.loads((WINUSB / "tools/rtlsdr/runtime-lock.json").read_text(encoding="utf-8"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def check_pe(path, machine):
    with path.open("rb") as stream:
        dos = stream.read(64)
        require(len(dos) == 64 and dos[:2] == b"MZ", f"Invalid DOS header: {path}")
        stream.seek(struct.unpack_from("<I", dos, 0x3C)[0])
        pe = stream.read(6)
    require(len(pe) == 6 and pe[:4] == b"PE\0\0", f"Invalid PE header: {path}")
    require(struct.unpack_from("<H", pe, 4)[0] == machine, f"Incorrect architecture: {path}")


def check_package(folder, bits):
    arch, machine = ("x86", 0x14C) if bits == 32 else ("x64", 0x8664)
    binaries = ("BonDriver_RTLSDR.dll", "DriverHost_PX4.exe", "WinSCard.dll", "rtlsdr/rtlsdr_frontend.exe")
    for name in binaries:
        check_pe(folder / name, machine)
    runtime = folder / "rtlsdr"
    libraries = LOCK["libraries"][arch]
    expected = {"rtlsdr_frontend.exe", *libraries}
    actual = {p.name for p in runtime.iterdir() if p.suffix.lower() in (".exe", ".dll")}
    require(actual == expected, f"Unexpected or missing runtime binaries: {folder}: {actual ^ expected}")
    for name, expected_hash in libraries.items():
        file = runtime / name
        check_pe(file, machine)
        require(digest(file) == expected_hash, f"RTL-SDR dependency hash mismatch: {file}")
    metadata = json.loads((runtime / "runtime.json").read_text(encoding="utf-8-sig"))
    for key in ("runtime", "url", "sha256", "source", "license"):
        require(metadata[key] == LOCK[key], f"Incorrect runtime metadata: {key}")
    require(metadata["architecture"] == arch, f"Incorrect runtime metadata architecture: {folder}")
    require((runtime / "COPYING.librtlsdr").stat().st_size > 1000, "Missing librtlsdr license")
    require((runtime / "COPYING.frontend").read_bytes() == (WINUSB / "tools/rtlsdr_frontend/LICENSE").read_bytes(),
            "Incorrect frontend license")
    require((folder / "LICENSE").read_bytes() == (WINUSB.parent / "LICENSE").read_bytes(), "Incorrect driver license")
    for name in ("README.md", "TECHNICAL.md", "Install-Driver.ps1", "BonDriver_RTLSDR.ChSet.txt", "Driver/RTL2838UHIDIR.inf",
                 "Driver/px4_drv_winusb.cat", "Driver/px4_drv_winusb.cer", "Driver/cert-install.jse", "Driver/verify.ps1"):
        require((folder / name).is_file(), f"Missing RTL-SDR package file: {folder / name}")
    require("(TECHNICAL.md)" in (folder / "README.md").read_text(encoding="utf-8"), "Broken packaged installation guide link")
    bon = configuration(folder / "BonDriver_RTLSDR.ini")
    require(bon["BonDriver"]["system"].strip('"') == "ISDB-T", "Incorrect RTL-SDR system")
    require(bon["BonDriver"].getboolean("framedstream"), "RTL-SDR requires framed stream epochs")
    require(bon["BonDriver.ISDB-T"]["chsetpath"].strip('"') == "BonDriver_RTLSDR.ChSet.txt", "Incorrect RTL-SDR ChSet path")
    host = configuration(folder / "DriverHost_PX4.ini")
    devices = [section for section in host if re.fullmatch(r"DeviceDefinition\d+", section)
               and host[section].get("type", "").strip('"') == "RTLSDR"]
    require(len(devices) == 1, "Expected one RTL-SDR device definition")
    device = devices[0]
    require(host[device]["guid"] == bon["ReceiverDefinition0"]["deviceguid"], "RTL-SDR device GUID mismatch")
    require(host[device + ".Config"]["helperpath"].strip('"') == r"rtlsdr\rtlsdr_frontend.exe", "Incorrect helper path")
    sources = folder / "Sources"
    require(digest(sources / "rtl-sdr-blog-V1.4.0.zip") == LOCK["source_sha256"], "Incorrect runtime source archive")
    frontend = sources / "winusb/tools/rtlsdr_frontend"
    for name in ("Cargo.toml", "Cargo.lock", "build.rs", "frontend.rc", "LICENSE", "src/main.rs", "src/pat.rs",
                 "vendor/README.md", "vendor/isdbt-dsp/Cargo.toml", "vendor/isdbt-dsp/src/lib.rs"):
        require((frontend / name).read_bytes() == (WINUSB / "tools/rtlsdr_frontend" / name).read_bytes(),
                f"Frontend source differs from build source: {name}")
    # module 宣言だけが残った欠損ソースを配布しないよう、復調器を含む全ソースを照合する。
    for subtree in ("src", "vendor/isdbt-dsp/src"):
        original = WINUSB / "tools/rtlsdr_frontend" / subtree
        for file in original.rglob("*"):
            if file.is_file():
                name = Path(subtree) / file.relative_to(original)
                require((frontend / name).read_bytes() == file.read_bytes(), f"Frontend source mismatch: {name}")
    config = (sources / ".cargo/config.toml").read_text(encoding="utf-8")
    require('directory = "vendor"' in config and 'replace-with = "vendored-sources"' in config, "Missing offline Cargo source configuration")
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
        folder = dist / f"BonDriver_RTLSDR_{bits}bit"
        check_package(folder, bits)
        host = (folder / "DriverHost_PX4.ini").read_bytes()
        require(reference is None or host == reference, "RTL-SDR architecture configurations differ")
        reference = host
        print(f"PASS: RTL-SDR {bits}bit DLL, helper, pinned runtime and corresponding offline sources")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, default=WINUSB / "dist")
    check_distribution(parser.parse_args().dist)
