#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Reject INI sections that GetPrivateProfileSectionW() reads as empty.

GetPrivateProfileSectionW() drops blank lines and lines that start with ';'.
A section left with only such lines returns 0, which older DriverHost_PX4 and
BonDriver_PX4 builds treat as a failure of the whole file, so every device
definition disappears. The distributed INI files must never contain one.
"""
import argparse
from pathlib import Path
import re

WINUSB = Path(__file__).resolve().parents[1]
SECTION = re.compile(r"^\s*\[([^\]]*)\]")


def decode(data):
    try:
        return data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return data.decode("cp932")


def empty_sections(path):
    """Return the names of sections without any line that Windows would return."""
    empty = []
    current = None
    has_value = False
    for line in decode(path.read_bytes()).splitlines():
        match = SECTION.match(line)
        if match:
            if current is not None and not has_value:
                empty.append(current)
            current = match[1]
            has_value = False
            continue
        stripped = line.strip()
        if current is not None and stripped and not stripped.startswith(";"):
            has_value = True
    if current is not None and not has_value:
        empty.append(current)
    return empty


def check(paths):
    problems = []
    for path in paths:
        problems += [f"{path}: [{name}] has no keys" for name in empty_sections(path)]
    if problems:
        raise ValueError("Key-less INI sections:\n" + "\n".join(problems))
    print(f"PASS: {len(paths)} INI files have no key-less sections")


def default_paths(dist):
    paths = sorted((WINUSB / "pkg").rglob("*.ini"))
    if dist.is_dir():
        paths += sorted(dist.rglob("*.ini"))
    return paths


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, default=WINUSB / "dist")
    parser.add_argument("paths", nargs="*", type=Path)
    arguments = parser.parse_args()
    check(arguments.paths or default_paths(arguments.dist))
