#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the assembled core in a deliberately limited FR instruction model.

The model follows GNU cpu/fr30.cpu encodings for this leaf routine only. It is
not a hardware emulator: interrupts, USB, MMIO and other FR instructions are
unsupported. Every load/store must remain inside the supplied packet.
"""
import argparse
from pathlib import Path
import random
import struct
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import protocol as p


def execute(code, packet, null=False):
    memory=bytearray(packet)
    ptr=0x1000
    regs=[0x12340000+i for i in range(16)]
    regs[4]=0 if null else ptr
    original=regs.copy()
    pc=0
    equal=False
    def word(address, value=None):
        if address&3 or not ptr<=address<=ptr+60:
            raise AssertionError(f'out-of-packet or unaligned access {address:#x}')
        off=address-ptr
        if value is None:return int.from_bytes(memory[off:off+4],'big')
        memory[off:off+4]=(value&0xffffffff).to_bytes(4,'big')
    for steps in range(512):
        if not 0<=pc<=len(code)-2 or pc&1:
            raise AssertionError('invalid instruction address')
        inst=int.from_bytes(code[pc:pc+2],'big')
        top=inst>>8
        i,j=(inst>>4)&15,inst&15
        pc+=2
        if inst==0x9720:
            if regs[4:]!=original[4:]:raise AssertionError('callee modified preserved registers')
            return bytes(memory),steps+1
        if inst&0xfff0==0x9f80:
            regs[j]=int.from_bytes(code[pc:pc+4],'big');pc+=4
        elif inst&0xf000==0xc000:regs[j]=(inst>>4)&255
        elif top==0x8b:regs[j]=regs[i]
        elif top==0xa0:regs[j]=(regs[j]+i)&0xffffffff
        elif top==0xa1:regs[j]=(regs[j]+i-16)&0xffffffff
        elif top==0xa8:equal=regs[j]==i
        elif top==0xaa:equal=regs[j]==regs[i]
        elif top==0x04:regs[j]=word(regs[i])
        elif top==0x14:word(regs[i],regs[j])
        elif top==0x92:regs[j]|=regs[i]
        elif top in (0xe0,0xe2,0xe3):
            disp=inst&255
            if disp&128:disp-=256
            if top==0xe0 or (top==0xe2 and equal) or (top==0xe3 and not equal):pc+=disp*2
        else:raise AssertionError(f'unsupported instruction {inst:04x} at {pc-2:#x}')
    raise AssertionError('core exceeded finite instruction budget')


class CoreTests(unittest.TestCase):
    def exchange(self, request):
        return execute(CODE,request)[0]

    def test_capabilities_and_padding(self):
        reply=self.exchange(p.request(0xffffffff,p.CAPS,b'\xff'*40))
        parsed=p.response(reply,0xffffffff,p.CAPS)
        self.assertEqual(parsed.status,p.OK)
        # I2C/TSの機能を実装済みと誤認させず、対象USB IDと容量を確認する。
        self.assertEqual(struct.unpack('>4I',parsed.payload),(0,40,40,0x06b8103b))

    def test_ping_preserves_sequence(self):
        for seq in (0,1,0x7fffffff,0xffffffff):
            self.assertEqual(p.response(self.exchange(p.request(seq,p.PING)),seq,p.PING),p.Response(p.OK,b''))

    def test_i2c_not_ready_and_unknown_operation(self):
        request=p.i2c_request(7,0x20,b'\xff',1,1000)
        self.assertEqual(p.response(self.exchange(request),7,p.I2C),p.Response(p.NOT_READY,b''))
        self.assertEqual(p.response(self.exchange(p.request(8,99)),8,99),p.Response(p.UNSUPPORTED,b''))

    def test_invalid_request_headers(self):
        for off,value,status in ((0,0,p.BAD_MAGIC),(12,2,p.BAD_VERSION)):
            request=bytearray(p.request(3,p.CAPS,b'\xaa'*40))
            request[off:off+4]=struct.pack('>I',value)
            self.assertEqual(p.response(self.exchange(request),3,p.CAPS),p.Response(status,b''))

    def test_finite_execution_with_arbitrary_payloads(self):
        rng=random.Random(295)
        for _ in range(500):
            seq=rng.getrandbits(32)
            op=rng.choice([p.CAPS,p.PING,p.I2C,0,0x7fffffff])
            request=p.request(seq,op,rng.randbytes(40))
            reply,steps=execute(CODE,request)
            self.assertLess(steps,200)
            expected={p.CAPS:p.OK,p.PING:p.OK,p.I2C:p.NOT_READY}.get(op,p.UNSUPPORTED)
            self.assertEqual(p.response(reply,seq,op).status,expected)

    def test_null_packet_does_not_access_memory(self):
        packet=bytes(range(64))
        self.assertEqual(execute(CODE,packet,null=True)[0],packet)

    def test_host_rejects_stale_truncated_and_corrupt_replies(self):
        reply=self.exchange(p.request(9,p.CAPS))
        for packet,seq,op in ((reply,8,p.CAPS),(reply,9,p.PING),(reply[:63],9,p.CAPS),(reply+b'\0',9,p.CAPS)):
            with self.assertRaises(ValueError):p.response(packet,seq,op)
        for off,value in ((0,0),(12,0xffffffff),(16,2),(20,41)):
            packet=bytearray(reply);packet[off:off+4]=struct.pack('>I',value)
            with self.assertRaises(ValueError):p.response(packet,9,p.CAPS)
        packet=bytearray(reply);packet[-1]=1
        with self.assertRaises(ValueError):p.response(packet,9,p.CAPS)

    def test_host_i2c_bounds(self):
        for address,tx,rx in ((128,b'x',0),(0,b'',0),(0,b'x'*41,0),(0,b'',41),(0,b'',-1)):
            with self.assertRaises(ValueError):p.i2c_request(1,address,tx,rx)


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',type=Path,required=True)
    args,remaining=ap.parse_known_args()
    CODE=args.binary.read_bytes()
    unittest.main(argv=[sys.argv[0]]+remaining)
