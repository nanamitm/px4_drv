#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the actual linked boot hook and core through the limited FR model."""
import argparse
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import protocol as p
from fr_model import Machine

DESC, STAGING = 0x2000, 0x000c4620
DONE, MASK, ORDINARY = 0x000ea8c8, 0x000ea704, 0x000ea75a


def run(request, selector=0x7e, size=64, address=STAGING):
    descriptor = bytearray(0x50)
    struct.pack_into('>I', descriptor, 0, address)
    descriptor[4:68] = request
    descriptor[0x46] = selector
    struct.pack_into('>H', descriptor, 0x48, size)
    packet = bytearray(b'\xcc'*64)
    machine = Machine(CODE, {DESC:descriptor, STAGING:packet}, (DONE,MASK,ORDINARY))
    machine.regs[11] = DESC
    result, steps = machine.run(0x000ea6fc)
    return result, bytes(packet), machine, steps


class BootstrapTests(unittest.TestCase):
    def test_patch_to_caps_reply(self):
        result, packet, machine, steps = run(p.request(295,p.CAPS,b'\xff'*40))
        self.assertEqual(result,DONE)
        self.assertEqual(struct.unpack('>4I',p.response(packet,295,p.CAPS).payload),(0,40,40,0x06b8103b))
        self.assertTrue(all(STAGING <= a < STAGING+64 for a in machine.writes))
        self.assertEqual(machine.regs[11],DESC)
        self.assertLess(steps,400)

    def test_ping_and_not_ready(self):
        for op, status in ((p.PING,p.OK),(p.I2C,p.NOT_READY),(999,p.UNSUPPORTED)):
            result,packet,_,_=run(p.request(7,op))
            self.assertEqual(result,DONE)
            self.assertEqual(p.response(packet,7,op),p.Response(status,b''))

    def test_corrupt_payload_is_answered_without_stale_ram(self):
        for off,value,status in ((0,0,p.BAD_MAGIC),(12,9,p.BAD_VERSION)):
            request=bytearray(p.request(8,p.CAPS))
            struct.pack_into('>I',request,off,value)
            result,packet,_,_=run(request)
            self.assertEqual(result,DONE)
            self.assertEqual(p.response(packet,8,p.CAPS).status,status)

    def test_nonmatching_requests_follow_stock_branches(self):
        for selector,size,address,expected in ((1,64,STAGING,MASK),(0,64,STAGING,ORDINARY),
                                              (0x7d,64,STAGING,ORDINARY),(0x7e,63,STAGING,ORDINARY),
                                              (0x7e,64,STAGING+4,ORDINARY)):
            result,packet,machine,_=run(p.request(9,p.PING),selector,size,address)
            self.assertEqual(result,expected)
            self.assertEqual(packet,b'\xcc'*64)
            self.assertEqual(machine.writes,[])


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build',type=Path,required=True)
    args,remaining=ap.parse_known_args()
    CODE={0x000e1100:(args.build/'resident.bin').read_bytes(),
          0x000e1000:(args.build/'bootstrap.bin').read_bytes(),
          0x000ea6fc:(args.build/'boot-hook.bin').read_bytes()}
    unittest.main(argv=[sys.argv[0]]+remaining)
