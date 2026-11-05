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
    ap.add_argument('--with-torne-hal', action='store_true',
                    help='link dormant torne HAL for compile verification; does not enable I2C')
    ap.add_argument('--torne-i2c', action='store_true',
                    help='build PAF2 torne I2C diagnostic with host-serviced clock')
    ap.add_argument('--torne-device-clock', action='store_true',
                    help='use measured USB frame clock in PAF2 instead of host-serviced clock')
    args = ap.parse_args()
    if args.torne_device_clock and not args.torne_i2c:
        ap.error('--torne-device-clock requires --torne-i2c')
    compiler = [args.compiler, *args.compiler_arg]
    target = subprocess.check_output([*compiler,'-dumpmachine'],text=True).strip()
    if target != 'fr30-elf':
        raise ValueError('FR auxiliary build requires fr30-elf, got '+target)
    out = args.out.resolve()
    out.mkdir(parents=True,exist_ok=True)
    objects = []
    for name in (['torne_auxiliary'] if args.torne_i2c else ['auxiliary']) + ['i2c_engine','runtime'] + (['torne_hal'] if args.with_torne_hal or args.torne_i2c else []) + (['torne_clock'] if args.torne_device_clock else []):
        assembly = out/(name+'.s')
        subprocess.run([*compiler,*(['-DDTP_TORNE_DEVICE_CLOCK'] if args.torne_device_clock else []),'-S','-std=c11','-Os','-ffreestanding','-fno-builtin',
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
    if args.with_torne_hal or args.torne_i2c:
        sources += ['torne_hal.c','torne_hal.h']
    if args.torne_i2c:
        sources = [name for name in sources if name not in ('auxiliary.c','auxiliary.h')]
        sources += ['torne_auxiliary.c','torne_auxiliary.h']
    if args.torne_device_clock:
        sources += ['torne_clock.c','torne_clock.h']
    manifest = dict(schema=1,target='CECH-ZD1J auxiliary FR' if args.torne_i2c else 'PIX-DT295 auxiliary FR',stage='source-auxiliary-unverified',
                    deployable=False,features=[],torne_hal_linked=args.with_torne_hal or args.torne_i2c,
                    diagnostic=('torne-paf2-device-clock' if args.torne_device_clock else 'torne-paf2-host-clock') if args.torne_i2c else 'paf1',
                    entry='0x000a0000',shared_ram='0x017a0000',
                    mailbox_cpu='0x000a3900',mailbox_shared='0x017a3900',mailbox_magic='PAF2' if args.torne_i2c else 'PAF1',
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
