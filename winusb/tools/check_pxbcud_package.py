"""Validate PX-BCUD files, architecture and shared configuration without opening USB."""
from pathlib import Path
import argparse
import configparser
import re
import struct


def configuration(path):
    data = path.read_bytes()
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = data.decode("cp932")
    result = configparser.ConfigParser(interpolation=None)
    result.read_string(text)
    return result


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_distribution(dist):
    inf = (dist / "Driver/PX-BCUD.inf").read_text(encoding="utf-8-sig")
    require(r"USB\VID_3275&PID_0085" in inf, "PX-BCUD hardware ID is missing from INF")
    for name in ("px4_drv_winusb.cat", "px4_drv_winusb.cer", "cert-install.jse", "cert-uninstall.jse"):
        require((dist / "Driver" / name).is_file(), "Missing driver file: " + name)
    for bits, machine in ((32, 0x14C), (64, 0x8664)):
        folder = dist / f"BonDriver_PX-BCUD_{bits}bit"
        for name in ("BonDriver_PX-BCUD.dll", "DriverHost_PX4.exe", "WinSCard.dll"):
            image = (folder / name).read_bytes()
            require(image[:2] == b"MZ" and len(image) >= 64, "Invalid DOS header: " + str(folder / name))
            offset = struct.unpack_from("<I", image, 0x3C)[0]
            require(offset + 6 <= len(image) and image[offset:offset + 4] == b"PE\0\0",
                    "Invalid PE header: " + str(folder / name))
            require(struct.unpack_from("<H", image, offset + 4)[0] == machine,
                    "Incorrect architecture: " + str(folder / name))
        for name in ("BonDriver_PX4-S.ChSet.txt", "LICENSE", "README.md"):
            require((folder / name).is_file(), "Missing: " + str(folder / name))
        bon = configuration(folder / "BonDriver_PX-BCUD.ini")
        require(bon["BonDriver"].get("driverhostpath", "").strip('"') == r".\DriverHost_PX4.exe", "Incorrect host path")
        require(bon["BonDriver"].getboolean("framedstream"), "PX-BCUD requires framed TS epochs")
        satellite = bon["BonDriver.ISDB-S"]
        require(satellite.getint("lnbpower") == 0, "PX-BCUD cannot supply LNB power")
        require(satellite["chsetpath"].strip('"') == "BonDriver_PX4-S.ChSet.txt", "Incorrect channel table path")
        host = configuration(folder / "DriverHost_PX4.ini")
        definitions = [section for section in host.sections() if re.fullmatch(r"DeviceDefinition\d+", section)]
        numbers = sorted(int(section[len("DeviceDefinition"):]) for section in definitions)
        require(numbers == list(range(len(numbers))), "Device definitions must be contiguous")
        devices = [section for section in definitions if host[section].get("type", "").strip('"') == "PXBCUD"]
        require(len(devices) == 1, "Expected exactly one PX-BCUD device definition")
        device = devices[0]
        receiver = host[device + ".Receiver0"]
        require(receiver["system"].strip('"') == "ISDB-S" and receiver.getint("index") == 0, "Incorrect satellite receiver")
        require(host[device]["guid"] == bon["ReceiverDefinition0"]["deviceguid"], "BonDriver device GUID differs from host")
        interface = host[device]["deviceinterfaceguid"].strip('"').lower()
        require(interface in inf.lower(), "INF interface GUID differs from host")
        # 共通ホスト設定を一機種専用の設定で置き換えると、ほかの BonDriver が開けなくなる。
        require(any(host[s].get("type", "").strip('"') == "PX4" for s in definitions), "Existing PX4 definitions are missing")
        require(any(host[s].get("type", "").strip('"') == "CECHZD1J" for s in definitions), "Torne definition is missing")
        print(f"PASS: standard PX-BCUD {bits}bit package, satellite receiver and interface GUID")
    reference = (dist / "BonDriver_PX-BCUD_64bit/DriverHost_PX4.ini").read_bytes()
    for folder in dist.glob("BonDriver_*"):
        if folder.is_dir():
            require((folder / "DriverHost_PX4.ini").read_bytes() == reference,
                    "Shared device definitions differ: " + str(folder))
    print("PASS: common device definitions across all BonDriver folders")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, default=Path(__file__).resolve().parents[1] / "dist")
    args = parser.parse_args()
    try:
        check_distribution(args.dist)
    except (ValueError, OSError, KeyError, configparser.Error, struct.error) as error:
        raise SystemExit("PX-BCUD package validation failed: " + str(error)) from error
