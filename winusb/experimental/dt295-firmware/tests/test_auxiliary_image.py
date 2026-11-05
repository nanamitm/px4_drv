#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run GCC-emitted FR mailbox routines, not a Python rewrite of their behavior."""
import argparse
import json
from pathlib import Path
import struct
import unittest
import sys
from fr_model import Machine

MAILBOX = 0x000a3900


def symbols(elf):
    if elf[:6] != b'\x7fELF\x01\x02' or struct.unpack_from('>H',elf,18)[0] != 0x54:
        raise ValueError('expected big-endian ELF32 for Fujitsu FR30')
    shoff=struct.unpack_from('>I',elf,32)[0]
    width,count=struct.unpack_from('>HH',elf,46)
    sections=[struct.unpack_from('>10I',elf,shoff+i*width) for i in range(count)]
    result={}
    for section in sections:
        if section[1] != 2:continue
        strings=sections[section[6]]
        names=elf[strings[4]:strings[4]+strings[5]]
        for off in range(section[4],section[4]+section[5],section[9]):
            name,address,_,_,_,_=struct.unpack_from('>IIIBBH',elf,off)
            label=names[name:names.find(b'\0',name)].decode('ascii')
            result[label]=address
    return result


def call(name, mailbox):
    machine=Machine({0xa0000:BINARY},{MAILBOX:mailbox})
    machine.regs[4]=MAILBOX
    machine.run(SYMBOLS[name],limit=2000)
    return machine


class AuxiliaryImageTests(unittest.TestCase):
    def test_entry_and_reserved_layout(self):
        elf=(BUILD/'auxiliary.elf').read_bytes()
        self.assertEqual(struct.unpack_from('>I',elf,24)[0],0xa0000)
        self.assertLessEqual(len(BINARY),0x2000)
        self.assertLessEqual(SYMBOLS['__bss_end'],0xa3000)
        self.assertEqual(SYMBOLS['__stack_top'],0xa37fc)
        self.assertIn('dtp_i2c_start',SYMBOLS)
        self.assertIn('dtp_i2c_poll',SYMBOLS)
        manifest=json.loads((BUILD/'manifest.json').read_text())
        self.assertFalse(manifest['deployable'])
        self.assertEqual(manifest['features'],[])

    def test_actual_compiled_initialization(self):
        mailbox=bytearray(b'\xaa'*64)
        machine=call('dtp_aux_init',mailbox)
        self.assertEqual(struct.unpack('>16I',mailbox),(0x50414631,1)+(0,)*14)
        self.assertEqual(machine.writes[-1],MAILBOX)

    def test_actual_compiled_request_sequence_and_not_ready(self):
        mailbox=bytearray(64)
        call('dtp_aux_init',mailbox)
        for seq,op,status in ((1,1,0),(2,2,0),(3,3,2),(4,99,1),(0xffffffff,1,0),(1,1,0)):
            struct.pack_into('>I',mailbox,12,seq)
            struct.pack_into('>I',mailbox,20,op)
            machine=call('dtp_aux_step',mailbox)
            self.assertEqual(machine.regs[4],1)
            self.assertEqual(struct.unpack_from('>I',mailbox,16)[0],seq)
            self.assertEqual(struct.unpack_from('>I',mailbox,24)[0],status)
            self.assertEqual(struct.unpack_from('>I',mailbox,28)[0],0)
            self.assertEqual(machine.writes[-1],MAILBOX+16)
            machine=call('dtp_aux_step',mailbox)
            self.assertEqual(machine.regs[4],0)
            self.assertNotIn(MAILBOX+16,machine.writes)

    def test_bad_identity_does_not_acknowledge(self):
        for offset,value in ((0,0),(4,2)):
            mailbox=bytearray(64)
            call('dtp_aux_init',mailbox)
            struct.pack_into('>I',mailbox,offset,value)
            struct.pack_into('>I',mailbox,12,1)
            machine=call('dtp_aux_step',mailbox)
            self.assertEqual(machine.regs[4],0)
            self.assertEqual(machine.writes,[])


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build',type=Path,required=True)
    args,remaining=ap.parse_known_args()
    BUILD=args.build
    BINARY=(BUILD/'auxiliary.bin').read_bytes()
    SYMBOLS=symbols((BUILD/'auxiliary.elf').read_bytes())
    unittest.main(argv=[sys.argv[0]]+remaining)
