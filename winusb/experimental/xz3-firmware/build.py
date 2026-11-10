#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build GV-MVP/XZ3 FR30 firmware."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="fr30-elf-gcc")
    parser.add_argument("--compiler-arg", action="append", default=[])
    parser.add_argument("--assembler", default="fr30-elf-as")
    parser.add_argument("--linker", default="fr30-elf-ld")
    parser.add_argument("--objcopy", default="fr30-elf-objcopy")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--ts-probe", action="store_true", help="build the TS ring and EP81 probe")
    mode.add_argument("--bootstrap", action="store_true", help="build the one-time board initializer")
    mode.add_argument("--runtime", "--ts-runtime", action="store_true",
                      help="build I2C tuning, continuous TS and C/N support")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    compiler = [args.compiler, *args.compiler_arg]
    if subprocess.check_output([*compiler, "-dumpmachine"], text=True).strip() != "fr30-elf":
        parser.error("FR30 GCC is required")
    args.out.mkdir(parents=True, exist_ok=True)
    asm, obj, elf, image = (args.out / f"relay.{ext}" for ext in ("s", "o", "elf", "bin"))
    source = "ts_probe.c" if args.ts_probe or args.runtime else "relay.c"
    defines = []
    if args.runtime:
        defines.append("-DXZ_TS_RUNTIME")
    if args.bootstrap:
        defines.append("-DXZ_BOOTSTRAP")
    subprocess.run([*compiler, "-std=c11", "-Os", "-ffreestanding", "-fno-builtin",
                    "-Wall", "-Wextra", "-Werror", *defines, "-S", ROOT / source, "-o", asm],
                   check=True)
    subprocess.run([args.assembler, "-o", obj, asm], check=True)
    entry = args.out / "entry.o"
    subprocess.run([args.assembler, "-o", entry, ROOT / "entry.s"], check=True)
    subprocess.run([args.linker, "-T", ROOT / "relay.ld", "-o", elf, entry, obj], check=True)
    subprocess.run([args.objcopy, "-O", "binary", "--only-section=.text", elf, image], check=True)
    subprocess.run([args.objcopy, "-O", "binary", "--only-section=.hook", elf,
                    args.out / "hook.bin"], check=True)
    print(f"{image} ({image.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
