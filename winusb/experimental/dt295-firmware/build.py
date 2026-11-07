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
    ap.add_argument('--bulk', action='store_true', help='include the experimental TS Bulk pump')
    ap.add_argument('--compiler', default='fr30-elf-gcc')
    ap.add_argument('--compiler-arg', action='append', default=[])
    ap.add_argument('--out', type=Path, default=ROOT / 'build')
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    resident = out/'resident.o'
    bulk_define = ['--defsym', 'DTP_BULK=1'] if args.bulk else []
    subprocess.run([args.assembler, *bulk_define, '-o', str(resident), str(ROOT/'resident.s')], check=True)
    objects = [str(resident)]
    sources = ['resident.s','resident.ld','bootstrap.s','bootstrap.ld','build.py']
    if args.bulk:
        compiler = [args.compiler, *args.compiler_arg]
        if subprocess.check_output([*compiler, '-dumpmachine'], text=True).strip() != 'fr30-elf':
            raise ValueError('the Bulk diagnostic requires fr30-elf GCC')
        assembly = out/'bulk_pump.s'
        obj = out/'bulk_pump.o'
        subprocess.run([*compiler, '-S', '-std=c11', '-Os', '-ffreestanding', '-fno-builtin',
                        '-Wall', '-Wextra', '-Werror', '-o', str(assembly), str(ROOT/'bulk_pump.c')], check=True)
        subprocess.run([args.assembler, '-o', str(obj), str(assembly)], check=True)
        objects.append(str(obj))
        sources.append('bulk_pump.c')
    subprocess.run([args.linker, '-T', str(ROOT/'resident.ld'), '-o', str(out/'resident.elf'), *objects], check=True)
    subprocess.run([args.objcopy, '-O', 'binary', str(out/'resident.elf'), str(out/'resident.bin')], check=True)
    with (out/'resident.asm').open('wb') as stream:
        subprocess.run([args.objdump, '-d', str(out/'resident.elf')], stdout=stream, check=True)
    subprocess.run([args.assembler, '-o', str(out/'bootstrap.o'), str(ROOT/'bootstrap.s')], check=True)
    subprocess.run([args.linker, '-T', str(ROOT/'bootstrap.ld'), '-o', str(out/'bootstrap.elf'), str(out/'bootstrap.o')], check=True)
    # ELFの配置間隔をパディングした巨大なrawイメージとして出力しない。
    for section, name in [('.text','bootstrap.bin'),('.hook','boot-hook.bin')]:
        subprocess.run([args.objcopy, '-O', 'binary', '--only-section='+section,
                        str(out/'bootstrap.elf'), str(out/name)], check=True)
    with (out/'bootstrap.asm').open('wb') as stream:
        subprocess.run([args.objdump, '-d', str(out/'bootstrap.elf')], stdout=stream, check=True)
    if not 0 < (out/'resident.bin').stat().st_size <= 0x1000 or \
            not 0 < (out/'bootstrap.bin').stat().st_size <= 0x800 or \
            (out/'boot-hook.bin').stat().st_size != 8:
        raise ValueError('invalid extracted firmware segment size')
    versions = {name: subprocess.check_output([tool, '--version'], text=True).splitlines()[0]
                for name, tool in [('as',args.assembler),('ld',args.linker),('objcopy',args.objcopy),('objdump',args.objdump)]}
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    manifest = dict(schema=1, target='PIX-DT295', entry='0x000e1100', format='raw-fr-big-endian',
                    stage='diagnostic-bootstrap-unverified', deployable=False, features=[], tools=versions,
                    sources={name:digest(ROOT/name) for name in sources},
                    binary=dict(file='resident.bin', size=(out/'resident.bin').stat().st_size,
                                sha256=digest(out/'resident.bin')),
                    bootstrap=dict(file='bootstrap.bin',address='0x000e1000',
                                   size=(out/'bootstrap.bin').stat().st_size,sha256=digest(out/'bootstrap.bin')),
                    hook=dict(file='boot-hook.bin',address='0x000ea6fc',
                              size=(out/'boot-hook.bin').stat().st_size,sha256=digest(out/'boot-hook.bin')))
    (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
