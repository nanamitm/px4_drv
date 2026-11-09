#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

"""Build an ARC RAM probe for the Siano RIO PX-S1UD."""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

LOAD_ADDRESS = 0x40260
MARKER_OFFSET = 0x40


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--probe", choices=("boot", "bulk"), default="boot")
    parser.add_argument("--cross-prefix", default="arc-linux-gnu-")
    args = parser.parse_args()
    source = Path(__file__).with_name(f"{args.probe}_probe.S")

    def run(tool, *options):
        subprocess.run([args.cross_prefix + tool, *options], check=True)

    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        obj = directory / "probe.o"
        elf = directory / "probe.elf"
        raw = directory / "probe.bin"
        run("as", "-EL", "-mcpu=arc700", "-o", obj, source)
        run("ld", "-EL", f"-Ttext={LOAD_ADDRESS:#x}", "-e", "_start", "-o", elf, obj)
        run("objcopy", "-O", "binary", "-j", ".text", elf, raw)
        payload = raw.read_bytes()

    if not payload or len(payload) % 4:
        raise RuntimeError("Expected a nonempty, word-aligned payload")
    if args.probe == "boot" and (len(payload) != MARKER_OFFSET + 4 or payload[MARKER_OFFSET:] != bytes(4)):
        raise RuntimeError("Unexpected boot probe layout")
    header = struct.pack("<III", 0xAD, len(payload), LOAD_ADDRESS)
    with args.output.open("xb") as output:
        output.write(header + payload)


if __name__ == "__main__":
    main()
