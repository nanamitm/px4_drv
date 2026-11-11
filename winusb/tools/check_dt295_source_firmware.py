# SPDX-License-Identifier: GPL-2.0-only
"""Check the committed source-built PIX-DT295 bundles against their sources. No hardware is opened.

The DTP3 and DTP4 bundles are generated with the FR30 toolchain and committed so that Windows builds do not
need that toolchain. This check fails when a firmware source changed without regenerating the
bundle, or when the bundle no longer matches the manifests recorded at generation time.
"""
from pathlib import Path
import hashlib
import json
import struct

root = Path(__file__).resolve().parents[1]
firmware = root / 'pkg/DriverHost_PX4/firmware'
sources = root / 'experimental/dt295-firmware'
for backend, magic, raw_ts in (('direct-fr', 'DTP3', False), ('source-fr', 'DTP4', True)):
    stem = f'dt295-{backend}'
    record = json.loads((firmware / f'{stem}.json').read_text(encoding='utf-8'))
    bundle = (firmware / f'{stem}.bin').read_bytes()
    digest = hashlib.sha256(bundle).hexdigest()
    if len(bundle) < 16 or record['format'] != magic or bundle[:4] != magic.encode('ascii'):
        raise SystemExit(f'{stem}.bin must be a {magic} bundle')
    if record['bundle'] != dict(file=f'{stem}.bin', bytes=len(bundle), sha256=digest):
        raise SystemExit(f'{stem}.bin differs from {stem}.json')
    manifest = record['direct']
    sizes = struct.unpack_from('>3I', bundle, 4)
    offset = 16
    for key, limit, size in zip(('binary', 'bootstrap', 'hook'), (0xfa0, 0x60, 8), sizes):
        section = bundle[offset:offset + size]
        expected = manifest[key]
        if not 0 < size <= limit or size != expected['size'] or len(section) != size or \
                hashlib.sha256(section).hexdigest() != expected['sha256']:
            raise SystemExit(f'{stem} section differs from its manifest: {key}')
        offset += size
    if offset != len(bundle):
        raise SystemExit(f'{magic} section sizes do not cover the bundle')
    files = manifest['sources']
    if ('direct_pump.c' not in files or ('bulk_pump.c' in files) != raw_ts
            or manifest['entry'] != '0x000e1060'):
        raise SystemExit(f'{stem} has the wrong main-CPU build configuration')
    for name, sha in files.items():
        if hashlib.sha256((sources / name).read_bytes()).hexdigest() != sha:
            raise SystemExit(f'Firmware source changed; regenerate {stem}.bin: {name}')
    print(f'PASS DT295 {backend} bundle {digest} matches its sources and manifest.')
