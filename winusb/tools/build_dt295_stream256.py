#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate an opt-in PIX-DT295 SBC2 image with the tested 256-success limit."""

import argparse
import hashlib
import struct
from pathlib import Path

SOURCE_SHA256 = "0b2147e190afc7626165594fdedbedc6461c0e1f044b17742123b77fc5026fcc"


def build(source: bytes) -> bytes:
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA256:
        raise ValueError("expected the verified mAgicVISION 0.1.1 PIX-DT295 image")

    profile_offset, profile_count, body_offset, body_size = struct.unpack_from(">4I", source, 16)
    resident_offset, resident_size = struct.unpack_from(">2I", source, 40)
    profile = next(offset for offset in range(profile_offset, profile_offset + 64 * profile_count, 64)
                   if struct.unpack_from(">I", source, offset)[0] == 3)
    body_address, body_capacity = struct.unpack_from(">2I", source, profile + 4)
    resident = bytearray(source[resident_offset:resident_offset + resident_size])

    # Copy the verified drain loop. The wider immediate adds one halfword.
    # Retarget its two exit branches and the loop back edge accordingly.
    original = resident[0x1986C:0x19898]
    drain = (original[:26] + bytes.fromhex("ea09") + original[28:36]
             + bytes.fromhex("e204 9b01 0100") + original[40:42]
             + bytes.fromhex("e3f3 a301 0781 8d07 9720 0000"))
    body = source[body_offset:body_offset + body_size] + drain
    if len(body) > body_capacity:
        raise ValueError("drain loop exceeds the saved bootstrap area")

    # Change just the DT295 drain call. The original dispatcher and FIFO
    # routine stay on their original paths, including for SERVICE replies.
    struct.pack_into(">I", resident, 0x1942E, body_address + body_size)
    resident[-8:] = b"DTS2.256"
    struct.pack_into(">I", resident, 0x2FFF8, 0)
    checksum = -sum(word for (word,) in struct.iter_unpack(">I", resident[:0x30000]))
    struct.pack_into(">I", resident, 0x2FFF8, checksum & 0xFFFFFFFF)

    result = bytearray(source)
    result[resident_offset:resident_offset + resident_size] = resident
    struct.pack_into(">2I", result, 16, profile, 1)
    struct.pack_into(">2I", result, 24, len(result), len(body))
    result.extend(body)
    struct.pack_into(">I", result, 12, len(result))
    return bytes(result)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        result = build(args.source.read_bytes())
        with args.output.open("xb") as stream:
            stream.write(result)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"{hashlib.sha256(result).hexdigest()}  {args.output}")


if __name__ == "__main__":
    main()
