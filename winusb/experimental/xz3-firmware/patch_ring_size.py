# SPDX-License-Identifier: GPL-2.0-only
"""ビルド済みのxz3-runtime.binのTSリングサイズを書き換える。

FR30 GCCのない環境で、ts_probe.cのRING_SIZEを変えた場合と同じ4か所だけを置き換える。
- 初期化表の {0xd02024, RING_SIZE} の値
- stream_ring()の `ldi:20 #RING_SIZE, r2` 2命令
- stream_ring()の `producer>=RING_SIZE` を `producer>RING_SIZE-1` とした `ldi:20 #RING_SIZE-1, r13`

ldi:20の即値は20ビットのため、上限は0xffe00。元のバイト列が一致しない場合は書き換えない。
"""
import argparse
from pathlib import Path
import struct

ORIGINAL = 0x3fe00
TABLE = 0x818  # 0x00d02024 の次のワードがサイズ
LDI20 = (0x4d2, 0x5ba)
LDI20_LAST = 0x450  # r13 = RING_SIZE-1
LIMIT = 0xfffff


def ldi20(value, register=2):
    return bytes((0x9b, (value >> 16) << 4 | register)) + struct.pack('>H', value & 0xffff)


def patch(image, size, original=ORIGINAL):
    if size % 512 or not 0 < size <= LIMIT:
        raise ValueError('ring size must be a positive multiple of 512 within ldi:20')
    data = bytearray(image)
    expected = {TABLE: struct.pack('>II', 0x00d02024, original)}
    expected.update({at: ldi20(original) for at in LDI20})
    expected[LDI20_LAST] = ldi20(original - 1, 13)
    for at, bytes_ in expected.items():
        if data[at:at + len(bytes_)] != bytes_:
            raise ValueError(f'unexpected bytes at {at:#x}: {data[at:at + len(bytes_)].hex()}')
    # 対象外の箇所に同じ値が残っていれば、ソースの構成が想定と異なる。
    others = [at for at in range(0, len(data) - 3, 2)
              if (data[at:at + 4] in (struct.pack('>I', original), struct.pack('>I', original - 1))
                  or data[at] == 0x9b and data[at + 1] >> 4 == original >> 16
                  and data[at + 2:at + 4] in (struct.pack('>H', original & 0xffff),
                                              struct.pack('>H', original - 1 & 0xffff)))
              and at not in (TABLE + 4, LDI20_LAST, *LDI20)]
    if others:
        raise ValueError('ring size also appears at ' + ', '.join(hex(at) for at in others))
    data[TABLE + 4:TABLE + 8] = struct.pack('>I', size)
    for at in LDI20:
        data[at:at + 4] = ldi20(size)
    data[LDI20_LAST:LDI20_LAST + 4] = ldi20(size - 1, 13)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image', type=Path)
    parser.add_argument('--size', type=lambda text: int(text, 0), required=True)
    parser.add_argument('--original', type=lambda text: int(text, 0), default=ORIGINAL)
    args = parser.parse_args()
    args.image.write_bytes(patch(args.image.read_bytes(), args.size, args.original))
    print(f'{args.image}: ring size {args.original:#x} -> {args.size:#x}')


if __name__ == '__main__':
    main()
