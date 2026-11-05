# SPDX-License-Identifier: GPL-2.0-only
"""Offline validation and planning only; contains no USB or process-launch API."""
import argparse
import hashlib
import json
from pathlib import Path

BOOT_BYTES = 0x3910
BOOT_SHA256 = '0bdf3af8c2d4ab49edd69740bd751e698e337e2df542ac4cedcbe4ba99c3e9b0'
ORIGINAL_HOOK = bytes.fromhex('c46d02b0a810e32b')
SOURCE_HOOK = bytes.fromhex('9bec1000970c9fa0')


def validate_boot(snapshot):
    if len(snapshot) != BOOT_BYTES or hashlib.sha256(snapshot).hexdigest() != BOOT_SHA256:
        raise ValueError('unknown DT295 boot snapshot; no install plan is available')
    if snapshot[0x16fc:0x1704] != ORIGINAL_HOOK:
        raise ValueError('DT295 boot hook preimage mismatch')


def inspect_build(build):
    build = Path(build)
    manifest = json.loads((build/'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('stage') != 'diagnostic-bootstrap-unverified' or manifest.get('deployable') is not False \
            or manifest.get('features') != [] or manifest.get('entry') != '0x80380000':
        raise ValueError('expected the unverified diagnostic build manifest')
    result = {}
    for key, name, address, limit in [('binary','resident.bin','0x80380000',0x1000),
                                      ('bootstrap','bootstrap.bin','0x000e1000',0x800),
                                      ('hook','boot-hook.bin','0x000ea6fc',8)]:
        data = (build/name).read_bytes()
        record = manifest[key]
        if record['file'] != name or not 0 < len(data) <= limit or record['size'] != len(data) \
                or record['sha256'] != hashlib.sha256(data).hexdigest():
            raise ValueError('firmware segment identity mismatch: '+name)
        if key != 'binary' and record['address'] != address:
            raise ValueError('firmware segment address mismatch: '+name)
        result[key] = dict(address=address, bytes=len(data), sha256=record['sha256'])
    if (build/'boot-hook.bin').read_bytes() != SOURCE_HOOK:
        raise ValueError('unexpected linked DT295 hook')
    return result


def plan(build, snapshot):
    validate_boot(snapshot)
    segments = inspect_build(build)
    # 手順の確認用。転送実装と実機確認がない段階では実行可能な計画を出さない。
    return dict(schema=1, executable=False, reason='USB adapter and recovery are not hardware-validated',
                target=dict(vid='0x06b8',pid='0x103b',boot_sha256=BOOT_SHA256), segments=segments,
                prerequisites=['exclusive device ownership','idle boot state',
                               'full RAM preimages and restoration path','checked native memory window'],
                intended_order=['save all preimages','write and verify resident core',
                                'write and verify bootstrap','install hook last and verify',
                                'CAPS/PING only','restore hook first, drain execution, then restore RAM'],
                diagnostic_exchange=dict(out=dict(request_type='0x40',request='0xbc',value='0x0c7e',
                                                 index='0x4620',bytes=64),
                                         read=dict(request_type='0xc0',request='0xbc',value='0x0c00',
                                                   index='0x4620',bytes=64)))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build',type=Path,required=True)
    ap.add_argument('--boot-snapshot',type=Path,required=True)
    args = ap.parse_args()
    print(json.dumps(plan(args.build,args.boot_snapshot.read_bytes()),indent=2))
