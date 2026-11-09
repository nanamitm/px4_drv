#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract a pinned reference's compressed auxiliary image for static analysis.

This tool is separate from building our firmware and never accesses a device.
The resulting reference image is not a source-built firmware artifact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

BUNDLE_SHA256='d9f549b29d286c2ff1e7daf4237e2205bb8a5a2affbb98362bce137b8b42a33f'
RESIDENT_SHA256='17148723122fe132d3f5438e9e3192eefa0b734aba6403348bbd9ba796bdd941'


def decompress(data, expected):
    out=bytearray()
    cursor=0
    while cursor<len(data):
        control=data[cursor];cursor+=1
        if control&128:
            length=((control>>4)&7)+3
            if cursor>=len(data):raise ValueError('truncated back reference')
            distance=((control&15)<<8)|data[cursor];cursor+=1
            if distance+1>len(out):raise ValueError('back reference precedes output')
            if len(out)+length>expected:raise ValueError('output exceeds expected size')
            for _ in range(length):out.append(out[-distance-1])
        else:
            length=control+1
            if cursor+length>len(data):raise ValueError('truncated literal')
            if len(out)+length>expected:raise ValueError('output exceeds expected size')
            out.extend(data[cursor:cursor+length]);cursor+=length
    if len(out)!=expected:raise ValueError('output size mismatch')
    return bytes(out)


def extract(bundle):
    if hashlib.sha256(bundle).hexdigest()!=BUNDLE_SHA256:
        raise ValueError('unknown reference firmware SHA-256')
    offset,size=struct.unpack_from('>II',bundle,40)
    resident=bundle[offset:offset+size]
    if hashlib.sha256(resident).hexdigest()!=RESIDENT_SHA256:
        raise ValueError('reference resident SHA-256 mismatch')
    data=decompress(resident[0x302f0:0x302f0+0x1811],0x1fe8)
    checksum=sum(struct.unpack('>2042I',data))&0xffffffff
    if checksum!=0xc23550a9:raise ValueError('reference auxiliary checksum mismatch')
    return data,dict(source_va='0x803b02f0',compressed_bytes=0x1811,bytes=len(data),checksum=hex(checksum),
                     sha256=hashlib.sha256(data).hexdigest(),shared_ram='0x017a0000',cpu_view='0x000a0000',
                     mailbox_shared='0x017a3900',mailbox_cpu='0x000a3900')


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--reference',type=Path,required=True)
    ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args()
    data,meta=extract(args.reference.read_bytes())
    args.out.mkdir(parents=True,exist_ok=True)
    (args.out/'aux-i2c.bin').write_bytes(data)
    (args.out/'extraction.json').write_text(json.dumps(meta,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(meta,indent=2))
