# SPDX-License-Identifier: GPL-2.0-only
"""Known terrestrial tuner sequence for the PIX-DT295 experiment."""
import time


def transfers(rows, dest, segments, receive):
    sequence = 2

    def xfer(tx, rx=0):
        nonlocal sequence
        sequence += 1
        yield 0x10, bytes(tx), rx
        return (dest / f'i2c-{sequence}.bin').read_bytes()

    def rw(reg, value):
        yield from xfer((0xfe, 0xc0, reg, value))

    def rf_field(reg, mask, value):
        yield from xfer((0xfe, 0xc0, 0xfb, reg))
        old = (yield from xfer((0xfe, 0xc1), 1))[0]
        yield from rw(reg, (old & mask) | value)

    def set_frequency(hz):
        divider = hz * 64 // 1000000
        yield from rf_field(0x0d, 0xfe, 0)
        yield from rw(0x0f, 0x20)
        yield from rw(0x10, divider & 255)
        yield from rw(0x11, divider >> 8)
        yield from rw(0, 1)
        yield from rw(0x31, 0x81)
        yield from rw(0, 0)
        yield from rw(0x7c, 0x1f)

    for row in rows:
        op = row['op']
        if op == 2:
            break
        if op == -2:
            continue
        if op == -1:
            time.sleep(row['delay_ms'] / 1000)
        elif op == 0:
            yield from xfer(bytes.fromhex(row['payload']))
        elif op == 128:
            _, reg, mask, value = bytes.fromhex(row['payload'])
            yield from rf_field(reg, mask, value)
        else:
            raise ValueError(f'unexpected initialization operation {op}')
    print('terrestrial_initialization=completed', flush=True)

    for segment in segments:
        channel = segment.channel
        frequency = 473142857 + (channel - 13) * 6000000
        started = time.monotonic()
        yield from rw(0x0b, 1)
        yield from rw(0x12, 1)
        time.sleep(.020)
        for register, value in ((3, 0), (0x1d, 0xaa), (0x1e, 0xaa), (0x1f, 0xaa), (0x25, 0), (0x23, 0x4d)):
            yield from xfer((register, value))
        yield from set_frequency(frequency)
        yield from rw(0x12, 0)
        yield from rf_field(0x6e, 0xff, 0x80)
        yield from rf_field(0x14, 0xdf, 0)
        yield from rw(0x12, 1)
        time.sleep(.020)
        yield from rf_field(0x6e, 0x7f, 0)
        status = (yield from xfer((0x0e,), 1))[0]
        for register, value in ((0x0e, (status & 0x70) | 1), (0x0f, 0x10), (1, 0x40), (0x23, 0x4c)):
            yield from xfer((register, value))
        time.sleep(.100)
        for register, value in ((0x1d, 2), (0x1e, 0x2a), (0x1f, 0x8a)):
            yield from xfer((register, value))
        for _ in range(20):
            values = yield from xfer((0x80,), 4)
            tmcc = (yield from xfer((0xb0,), 1))[0]
            if not (values[0] & 0x28) and (tmcc & 15) >= 8:
                break
            time.sleep(.1)
        else:
            raise TimeoutError(f'GR{channel} demodulator lock failed ({values.hex()}/{tmcc:02x})')
        print(f'GR{channel} lock verified ({values.hex()}/{tmcc:02x})', flush=True)
        receive(segment, time.monotonic() - started)
