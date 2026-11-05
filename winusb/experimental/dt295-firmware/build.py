#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the source-only FR protocol core; no USB access or firmware installation."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--assembler', default='fr30-elf-as')
    ap.add_argument('--linker', default='fr30-elf-ld')
    ap.add_argument('--objcopy', default='fr30-elf-objcopy')
    ap.add_argument('--objdump', default='fr30-elf-objdump')
    ap.add_argument('--out', type=Path, default=ROOT / 'build')
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run([args.assembler, '-o', str(out/'resident.o'), str(ROOT/'resident.s')], check=True)
    subprocess.run([args.linker, '-T', str(ROOT/'resident.ld'), '-o', str(out/'resident.elf'), str(out/'resident.o')], check=True)
    subprocess.run([args.objcopy, '-O', 'binary', str(out/'resident.elf'), str(out/'resident.bin')], check=True)
    with (out/'resident.asm').open('wb') as stream:
        subprocess.run([args.objdump, '-d', str(out/'resident.elf')], stdout=stream, check=True)
    versions = {name: subprocess.check_output([tool, '--version'], text=True).splitlines()[0]
                for name, tool in [('as',args.assembler),('ld',args.linker),('objcopy',args.objcopy),('objdump',args.objdump)]}
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    manifest = dict(schema=1, target='PIX-DT295', entry='0x80380000', format='raw-fr-big-endian',
                    stage='protocol-core-only', deployable=False, features=[], tools=versions,
                    sources={name:digest(ROOT/name) for name in ['resident.s','resident.ld','build.py']},
                    binary=dict(file='resident.bin', size=(out/'resident.bin').stat().st_size,
                                sha256=digest(out/'resident.bin')))
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
