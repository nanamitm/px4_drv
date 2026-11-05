#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build a source-only FR auxiliary image. Does not load it onto hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--compiler', default='fr30-elf-gcc')
    ap.add_argument('--compiler-arg', action='append', default=[])
    ap.add_argument('--assembler', default='fr30-elf-as')
    ap.add_argument('--linker', default='fr30-elf-ld')
    ap.add_argument('--objcopy', default='fr30-elf-objcopy')
    ap.add_argument('--objdump', default='fr30-elf-objdump')
    ap.add_argument('--out', type=Path, default=ROOT/'build'/'auxiliary')
    args = ap.parse_args()
    compiler = [args.compiler, *args.compiler_arg]
    target = subprocess.check_output([*compiler,'-dumpmachine'],text=True).strip()
    if target != 'fr30-elf':
        raise ValueError('FR auxiliary build requires fr30-elf, got '+target)
    out = args.out.resolve()
    out.mkdir(parents=True,exist_ok=True)
    objects = []
    for name in ['auxiliary','i2c_engine','runtime']:
        assembly = out/(name+'.s')
        subprocess.run([*compiler,'-S','-std=c11','-Os','-ffreestanding','-fno-builtin',
                        '-fno-stack-protector','-fno-pic','-fno-unwind-tables',
                        '-fno-asynchronous-unwind-tables','-Wall','-Wextra','-Werror',
                        '-I',str(ROOT),'-o',str(assembly),str(ROOT/(name+'.c'))],check=True)
        obj = out/(name+'.o')
        subprocess.run([args.assembler,'-o',str(obj),str(assembly)],check=True)
        objects.append(str(obj))
    entry = out/'auxiliary_entry.o'
    subprocess.run([args.assembler,'-o',str(entry),str(ROOT/'auxiliary_entry.s')],check=True)
    subprocess.run([args.linker,'-T',str(ROOT/'auxiliary.ld'),'-Map',str(out/'auxiliary.map'),
                    '-o',str(out/'auxiliary.elf'),str(entry),*objects],check=True)
    subprocess.run([args.objcopy,'-O','binary',str(out/'auxiliary.elf'),str(out/'auxiliary.bin')],check=True)
    data = (out/'auxiliary.bin').read_bytes()
    if not 0 < len(data) <= 0x2000:
        raise ValueError('auxiliary raw image exceeds code window')
    with (out/'auxiliary.asm').open('wb') as stream:
        subprocess.run([args.objdump,'-d',str(out/'auxiliary.elf')],stdout=stream,check=True)
    digest = lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    sources = ['auxiliary.c','auxiliary.h','auxiliary_entry.s','auxiliary.ld',
               'i2c_engine.c','i2c_engine.h','runtime.c','build_auxiliary.py']
    manifest = dict(schema=1,target='PIX-DT295 auxiliary FR',stage='source-auxiliary-unverified',
                    deployable=False,features=[],entry='0x000a0000',shared_ram='0x017a0000',
                    mailbox_cpu='0x000a3900',mailbox_shared='0x017a3900',mailbox_magic='PAF1',
                    compiler=subprocess.check_output([*compiler,'--version'],text=True).splitlines()[0],
                    tools={name:subprocess.check_output([tool,'--version'],text=True).splitlines()[0]
                           for name,tool in [('as',args.assembler),('ld',args.linker),
                                             ('objcopy',args.objcopy),('objdump',args.objdump)]},
                    sources={name:digest(ROOT/name) for name in sources},
                    binary=dict(file='auxiliary.bin',bytes=len(data),sha256=digest(out/'auxiliary.bin')))
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(manifest,indent=2))


if __name__ == '__main__':
    main()
