# SPDX-License-Identifier: GPL-2.0-only
"""PIX-DT295 satellite RF programming and TSID selection through PAF1."""
import time

# 0x22は書き込み方向ビット込みのアドレス。PAF1は7ビット値を受け取る。
DEMOD = 0x11
BANDS = (
    (975000, 0x1c, 1), (1050000, 0x18, 2), (1150000, 0x14, 2),
    (1250000, 0x10, 3), (1350000, 0x0c, 4), (1450000, 0x0a, 4),
    (1600000, 7, 5), (1800000, 4, 2), (2000000, 2, 1), (2151000, 0, 0),
)


def frequency(system, channel):
    if system == 'bs' and 1 <= channel <= 23 and channel % 2:
        return 1049480000 + (channel - 1) // 2 * 38360000
    if system == 'cs' and 2 <= channel <= 24 and not channel % 2:
        return 1613000000 + (channel - 2) // 2 * 40000000
    raise ValueError('expected odd BS01–BS23 or even CS02–CS24')


def rf_parameters(hz):
    # チューナーへ渡すIFは最寄りの1 MHzに丸める。
    khz = (hz // 1000 + 500) // 1000 * 1000
    low_band = khz <= 1100000
    divider = (khz * (2 if low_band else 1) + 500) // 1000
    band, mode = next((band, mode) for limit, band, mode in BANDS if khz < limit)
    pll = (divider >> 7, (divider * 2) & 255, 0, 0, 0x80 if low_band else 0)
    return pll, mode << 5 | band


class Satellite:
    def __init__(self, xfer):
        self.xfer = xfer

    def transfer(self, tx, rx=0):
        return self.xfer(tx, rx, address=DEMOD)

    def write(self, reg, value):
        return self.transfer((reg, value))

    def rf(self, reg, *values):
        return self.transfer((0xfe, 0xc0, reg, *values))

    def registers(self, values):
        for reg, value in values:
            yield from self.write(reg, value)

    def select(self, tsid):
        yield from self.registers(((0x8f, tsid >> 8), (0x90, tsid & 255)))

    def sleep(self):
        yield from self.registers(((0x1c, 0xaa), (0x1f, 0x2a), (0x1e, 0x28),
                                   (0x0a, 0), (3, 1), (0x17, 1)))
        value = (yield from self.transfer((0x1e,), 1))[0]
        yield from self.write(0x1e, (value & 0xcf) | 0x30)
        for reg, value in ((0x2a, 0xb9), (0x29, 0x70), (0x28, 0x3e), (0x2a, 0x19), (0x1c, 0)):
            yield from self.rf(reg, value)
        yield from self.rf(0x11, 0xc0, 0xa7)

    def tune(self, channel):
        value = (yield from self.transfer((0x1e,), 1))[0]
        yield from self.write(0x1e, (value & 0xcf) | 0x20)
        yield from self.rf(0x11, 0, 0xa7)
        for reg, value in ((0x2a, 0xb9), (0x1c, 0xc0), (0x29, 0x71)):
            yield from self.rf(reg, value)
        yield from self.registers(((0x17, 0), (0x1e, 0x20), (0x0a, 0xff), (3, 1),
                                   (0x1c, 0xaa), (0x1f, 0x2a)))
        # PLL変更の前に以前の多重の指定を解除する。
        yield from self.select(0)
        yield from self.registers(((0x1e, 0x28), (0x0a, 0), (3, 1)))
        pll, band = rf_parameters(frequency(channel.system, channel.channel))
        yield from self.rf(0, *pll)
        for reg, value in ((9, band), (0x37, 0xac), (5, 0x80), (0x2a, 0xbb)):
            yield from self.rf(reg, value)
        time.sleep(.010)
        for reg in (0x0e, 0x0f):
            yield from self.xfer((reg, 0x77))
        value = (yield from self.transfer((7,), 1))[0]
        yield from self.registers(((7, (value & 0x70) | 1), (8, 0x10)))
        rf = yield from self.transfer((0xfe, 0xc1), 2)
        yield from self.rf(0x50, 0xf1 if rf[1] & 0x60 == 0x60 else 0xf0)
        time.sleep(.040)
        yield from self.registers(((0x1e, 0x20), (0x0a, 0xff), (3, 1), (3, 1)))
        time.sleep(.100)
        yield from self.registers(((0x1c, 8), (0x1f, 8)))
        value = (yield from self.transfer((0xa5,), 1))[0]
        yield from self.write(0xa5, value | 0x40)
        value = (yield from self.transfer((0xa3,), 1))[0]
        yield from self.write(0xa3, (value & 0x88) | 0x11)
        yield from self.select(channel.tsid)

        deadline = time.monotonic() + 4
        while True:
            status = (yield from self.transfer((0xc3,), 1))[0]
            selected = int.from_bytes((yield from self.transfer((0xe6,), 2)), 'big')
            if not status & 0x10 and selected == channel.tsid:
                print(f'{channel.label} lock verified (status={status:#04x}, TSID={selected})', flush=True)
                return
            if time.monotonic() >= deadline:
                raise TimeoutError(f'{channel.label} lock/TSID failed (status={status:#04x}, TSID={selected})')
            time.sleep(.1)
