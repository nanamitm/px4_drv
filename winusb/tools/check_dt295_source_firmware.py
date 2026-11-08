# SPDX-License-Identifier: GPL-2.0-only
"""Check the committed source-built PIX-DT295 bundle against its sources. No hardware is opened.

The DTP2 bundle is generated with the FR30 toolchain and committed so that Windows builds do not
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
record = json.loads((firmware / 'dt295-source-fr.json').read_text(encoding='utf-8'))
bundle = (firmware / 'dt295-source-fr.bin').read_bytes()
digest = hashlib.sha256(bundle).hexdigest()
if record['format'] != 'DTP2' or bundle[:4] != b'DTP2':
    raise SystemExit('dt295-source-fr.bin must be a DTP2 bundle')
if record['bundle'] != dict(file='dt295-source-fr.bin', bytes=len(bundle), sha256=digest):
    raise SystemExit('dt295-source-fr.bin differs from dt295-source-fr.json')
sizes = struct.unpack_from('>4I', bundle, 4)
offset, sections = 20, []
for size in sizes:
    sections.append(bundle[offset:offset + size])
    offset += size
if offset != len(bundle):
    raise SystemExit('DTP2 section sizes do not cover the bundle')
auxiliary, bulk = record['auxiliary'], record['bulk']
expected = [(sections[0][:auxiliary['binary']['bytes']], auxiliary['binary']['sha256'], 'auxiliary.bin'),
            (sections[1], bulk['binary']['sha256'], 'resident.bin'),
            (sections[2], bulk['bootstrap']['sha256'], 'bootstrap.bin'),
            (sections[3], bulk['hook']['sha256'], 'boot-hook.bin')]
for data, sha, name in expected:
    if hashlib.sha256(data).hexdigest() != sha:
        raise SystemExit('Bundle section differs from its manifest: ' + name)
if 'bulk_pump.c' not in bulk['sources']:
    raise SystemExit('Bundle was not built with build.py --bulk')
for manifest in (auxiliary, bulk):
    for name, sha in manifest['sources'].items():
        if hashlib.sha256((sources / name).read_bytes()).hexdigest() != sha:
            raise SystemExit('Firmware source changed; regenerate dt295-source-fr.bin: ' + name)
print('PASS DT295 source-fr bundle ' + digest + ' matches its sources and manifests.')

# direct-frのDTP3は補助CPUを使わないため、主CPU側の3区画だけを持つ。
record = json.loads((firmware / 'dt295-direct-fr.json').read_text(encoding='utf-8'))
bundle = (firmware / 'dt295-direct-fr.bin').read_bytes()
digest = hashlib.sha256(bundle).hexdigest()
if record['format'] != 'DTP3' or bundle[:4] != b'DTP3':
    raise SystemExit('dt295-direct-fr.bin must be a DTP3 bundle')
if record['bundle'] != dict(file='dt295-direct-fr.bin', bytes=len(bundle), sha256=digest):
    raise SystemExit('dt295-direct-fr.bin differs from dt295-direct-fr.json')
sizes = struct.unpack_from('>3I', bundle, 4)
offset, sections = 16, []
for size in sizes:
    sections.append(bundle[offset:offset + size])
    offset += size
if offset != len(bundle):
    raise SystemExit('DTP3 section sizes do not cover the bundle')
direct = record['direct']
for data, sha, name in ((sections[0], direct['binary']['sha256'], 'resident.bin'),
                        (sections[1], direct['bootstrap']['sha256'], 'bootstrap.bin'),
                        (sections[2], direct['hook']['sha256'], 'boot-hook.bin')):
    if hashlib.sha256(data).hexdigest() != sha:
        raise SystemExit('Bundle section differs from its manifest: ' + name)
if 'direct_pump.c' not in direct['sources'] or direct['entry'] != '0x000e1060':
    raise SystemExit('Bundle was not built with build.py --direct-usb')
for name, sha in direct['sources'].items():
    if hashlib.sha256((sources / name).read_bytes()).hexdigest() != sha:
        raise SystemExit('Firmware source changed; regenerate dt295-direct-fr.bin: ' + name)
print('PASS DT295 direct-fr bundle ' + digest + ' matches its sources and manifest.')
