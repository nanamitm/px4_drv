# SPDX-License-Identifier: GPL-2.0-only
"""RAM-only PIX-DT295 TS engine setup and restoration."""
import time

from aux_probe import mapped_read, mapped_write
from boot_probe import replace, run

CLOCK = 0x01900404
RUN = 0x01902010
RING = 0x8138ec00
RING_SIZE = 0x30000
REGISTERS = (
    (0x01900000, 4, 0x20000), (0x01900004, 4, 0x1100),
    (0x01901040, 4, 0), (0x01901064, 4, 0), (0x019010a0, 4, 0),
    (0x019010a4, 4, 0), (0x019010a8, 4, 0),
    (0x01901000, 2, 0x600), (0x01901020, 4, 0x02000000),
    (0x01901004, 4, 0x04000000), (0x01901002, 2, 0),
    (0x01901044, 4, 0x00bc0000), (0x01901048, 4, 0x47000000),
    (0x0190104c, 4, 0x00ffffff), (0x01901050, 4, 0),
    (0x01901060, 4, 0), (0x01901080, 4, 0x01000000),
    (0x01901088, 4, 0), (0x0190108c, 4, 0xffffffff),
    (0x01901084, 4, 0), (0x01901090, 4, 0x01000000),
    (0x01902020, 4, RING & 0x7fffffff), (0x01902024, 4, RING_SIZE),
    (0x01902028, 4, 0), (0x0190202c, 4, 0), (0x01902030, 4, 0),
    (0x01902000, 4, 0x03020300),
)


def ring_ranges():
    address = RING
    while address < RING + RING_SIZE:
        size = min(0x20000 - (address & 0x1ffff), RING + RING_SIZE - address)
        yield address, size
        address += size


def capture(control, build, evidence, receive):
    system = (0xf10118, 0xf1011c, 0xf001f0)
    before = tuple(control(address, size) for address, size in zip(system, (2, 2, 4)))
    if before != (b'\x90\0', b'\x47\x9f', bytes(4)):
        raise OSError(f'unexpected TS system preimage: {before}')
    enabled = (int.from_bytes(before[1], 'big') & 0xfa1f) | 0x0580
    active = b'\xff\xff', enabled.to_bytes(2, 'big'), b'\x1a\0\0\0'
    try:
        for address, value in zip(system, active):
            replace(control, address, value)
        return capture_ring(control, build, evidence, receive)
    finally:
        for address, value in reversed(tuple(zip(system, before))):
            replace(control, address, value)
        print('ts_system_restored=verified', flush=True)


def capture_ring(control, build, evidence, receive):
    def get(address, size=4):
        return int.from_bytes(mapped_read(control, address, size), 'big')
    def put(address, value, size=4):
        mapped_write(control, address, value.to_bytes(size, 'big'))
    def checked(address, value, size=4):
        put(address, value, size)
        observed = get(address, size)
        if observed != value:
            raise OSError(f'TS register {address:08x}: expected {value:08x}, observed {observed:08x}')

    if get(RUN):
        raise OSError('TS engine is not idle')
    clock = get(CLOCK)
    ring = [mapped_read(control, address, size) for address, size in ring_ranges()]
    (evidence / 'ts-ring-before.bin').write_bytes(b''.join(ring))
    previous = []
    touched = False
    try:
        checked(CLOCK, clock | 0x40000)
        previous = [(address, size, get(address, size)) for address, size, _ in REGISTERS]
        print('ts_preimage=' + ' '.join(f'{a:x}={v:x}' for a, _, v in previous), flush=True)
        touched = True
        put(0x01900004, 0x1f11ff)
        put(0x01900000, 0x3002ffff)
        put(0x01900000, 0x20000)
        put(0x01900004, 0x1100)
        for address, size, value in REGISTERS[2:]:
            checked(address, value, size)
        put(0x01901000, 0x607, 2)
        put(0x01901040, 0)
        put(0x01901020, 0)
        put(0x01901040, 0x03000000)
        put(0x019010a4, 0x13170400)
        return run(control, build, evidence, True,
                   during_probe=lambda: receive(lambda: put(RUN, 0x02000000)))
    finally:
        if touched:
            put(0x01901040, 0)
            put(0x01901020, 0x02000000)
            put(RUN, 0x04000000)
            deadline = time.monotonic() + 1
            while get(RUN):
                if time.monotonic() >= deadline:
                    raise OSError('TS stop unverified; USB replug required')
            for address, size, value in reversed(previous):
                put(address, value, size)
            for (address, size), data in zip(ring_ranges(), ring):
                if mapped_read(control, address, size) != data:
                    mapped_write(control, address, data)
                if mapped_read(control, address, size) != data:
                    raise OSError('TS ring restoration mismatch')
            print('ts_ring_restored=verified', flush=True)
        checked(CLOCK, clock)
