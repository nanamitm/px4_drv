# SPDX-License-Identifier: GPL-2.0-only
"""card_gpio_probe.pyの端子操作と復元を、実機なしの模擬MMIOで確認する。"""
import struct
import unittest
from unittest import mock

import card_gpio_probe as probe

ATR = bytes.fromhex('3bf01200ff9181b17c451f0399')


class FakeXz3:
    """端子のレジスターとカードUARTの模擬。

    cmdvccnのビットをLowで駆動した状態でUARTのPOWERに1を書くと、受信FIFOにATRが入る。
    """

    def __init__(self, cmdvccn=None, low_half=0x1234, present=True, fail_at=None, rstin=None):
        self.words = {
            probe.PIN_DIRECTION: probe.EXPECTED_DIRECTION << 16 | low_half,
            probe.PIN_OUTPUT: probe.EXPECTED_OUTPUT << 16 | low_half,
            probe.STATUS: probe.PRESENT if present else 0,
        }
        self.cmdvccn = cmdvccn
        self.rstin = rstin
        self.fail_at = fail_at
        self.rx = bytearray()
        self.writes = []

    def driven_low(self, bit):
        direction = self.words[probe.PIN_DIRECTION] >> 16
        output = self.words[probe.PIN_OUTPUT] >> 16
        return bool(direction >> bit & 1) and not output >> bit & 1

    def driven_high(self, bit):
        direction = self.words[probe.PIN_DIRECTION] >> 16
        output = self.words[probe.PIN_OUTPUT] >> 16
        return bool(direction >> bit & 1 and output >> bit & 1)

    def read(self, address, size):
        assert size == 4
        if address == probe.FIFO:
            return struct.pack('>I', len(self.rx) << 16)
        if address == probe.RX_DATA:
            return struct.pack('>I', self.rx.pop(0))
        return struct.pack('>I', self.words.get(address, 0))

    def write(self, address, data):
        assert len(data) == 4 and not address & 3
        value = struct.unpack('>I', data)[0]
        self.writes.append((address, value))
        if self.fail_at is not None and len(self.writes) == self.fail_at:
            raise OSError('stall')
        if address == probe.STATUS:
            return
        # RSTINの立ち上がりでATRが送られる。UARTの受信準備（POWER=1）前のATRは失われる。
        rst_before = self.rstin is not None and self.driven_high(self.rstin)
        self.words[address] = value
        if self.rstin is not None and not rst_before and self.driven_high(self.rstin) \
                and self.words.get(probe.POWER) == 1:
            self.rx.extend(ATR)
        if address == probe.POWER and value == 1 and self.cmdvccn is not None \
                and self.driven_low(self.cmdvccn):
            self.rx.extend(ATR)


def no_sleep(_):
    pass


class ProbeBitTest(unittest.TestCase):
    def setUp(self):
        self.direction = probe.EXPECTED_DIRECTION
        self.output = probe.EXPECTED_OUTPUT

    def probe(self, device, bit):
        return probe.probe_bit(probe.Pins(device), probe.Uart(device, no_sleep), bit,
                               self.direction, self.output, no_sleep)

    def test_default_candidates_skip_tuner_bits(self):
        self.assertEqual(probe.candidates(self.direction, self.output, None), [2, 5, 7])
        self.assertEqual(probe.candidates(self.direction, self.output, [6, 9]), [6, 9])

    def test_atr_is_received_only_on_the_cmdvccn_bit(self):
        device = FakeXz3(cmdvccn=5)
        self.assertEqual(self.probe(device, 5)['atr'], ATR)
        self.assertEqual(self.probe(device, 2)['atr'], b'')

    def test_pins_and_low_half_are_restored(self):
        device = FakeXz3(cmdvccn=7, low_half=0xbeef)
        before = dict(device.words)
        self.probe(device, 7)
        self.assertEqual(device.words[probe.PIN_DIRECTION], before[probe.PIN_DIRECTION])
        self.assertEqual(device.words[probe.PIN_OUTPUT], before[probe.PIN_OUTPUT])
        pin_writes = [value for address, value in device.writes
                      if address in (probe.PIN_DIRECTION, probe.PIN_OUTPUT)]
        self.assertTrue(all(value & 0xffff == 0xbeef for value in pin_writes))

    def test_input_pin_gets_output_low_before_direction(self):
        # bit 9は出力許可がない。Highを出さないよう、出力値をLowにしてから許可する。
        device = FakeXz3(cmdvccn=9)
        result = self.probe(device, 9)
        self.assertTrue(result['input'])
        self.assertEqual(result['atr'], ATR)
        pin_writes = [(address, value >> 16) for address, value in device.writes
                      if address in (probe.PIN_DIRECTION, probe.PIN_OUTPUT)]
        enable = next(i for i, (address, value) in enumerate(pin_writes)
                      if address == probe.PIN_DIRECTION and value >> 9 & 1)
        self.assertFalse(pin_writes[enable - 1][1] >> 9 & 1)
        self.assertEqual(device.words[probe.PIN_DIRECTION] >> 16, self.direction)
        self.assertEqual(device.words[probe.PIN_OUTPUT] >> 16, self.output)

    def test_power_down_and_pins_restored_after_failure(self):
        device = FakeXz3(cmdvccn=5)
        # UART_RESETの途中で転送が失敗した場合。
        device.fail_at = len(probe.UART_POWER_DOWN) + 6
        with self.assertRaises(OSError):
            self.probe(device, 5)
        tail = [address for address, _ in device.writes[-(len(probe.UART_POWER_DOWN) + 2):]]
        self.assertEqual(tail[:len(probe.UART_POWER_DOWN)],
                         [address for address, _, _ in probe.UART_POWER_DOWN])
        self.assertEqual(device.words[probe.PIN_DIRECTION] >> 16, self.direction)
        self.assertEqual(device.words[probe.PIN_OUTPUT] >> 16, self.output)

    def test_pin_readback_mismatch_is_an_error(self):
        class Stuck(FakeXz3):
            def write(self, address, data):
                if address == probe.PIN_OUTPUT:
                    return
                super().write(address, data)
        with self.assertRaises(OSError):
            self.probe(Stuck(), 5)


class ProbeBitHighTest(unittest.TestCase):
    def setUp(self):
        self.direction = probe.EXPECTED_DIRECTION
        self.output = probe.EXPECTED_OUTPUT

    def probe(self, device, bit):
        return probe.probe_bit_high(probe.Pins(device), probe.Uart(device, no_sleep), bit,
                                    self.direction, self.output, no_sleep)

    def test_default_high_candidates_are_low_outputs_and_inputs(self):
        self.assertEqual(probe.candidates(self.direction, self.output, None, 'high'),
                         [1, 3, 4, 8, 9, 10, 11, 12, 13, 14, 15])

    def test_atr_on_rstin_rise_after_uart_is_ready(self):
        for bit in (12, 9):
            device = FakeXz3(rstin=bit)
            result = self.probe(device, bit)
            self.assertEqual(result['atr'], ATR)
            self.assertEqual(device.words[probe.PIN_DIRECTION] >> 16, self.direction)
            self.assertEqual(device.words[probe.PIN_OUTPUT] >> 16, self.output)

    def test_other_bits_do_not_receive(self):
        self.assertEqual(self.probe(FakeXz3(rstin=12), 4)['atr'], b'')

    def test_restored_after_failure(self):
        device = FakeXz3(rstin=12, fail_at=len(probe.UART_POWER_DOWN) + 3)
        with self.assertRaises(OSError):
            self.probe(device, 12)
        self.assertEqual(device.words[probe.PIN_DIRECTION] >> 16, self.direction)
        self.assertEqual(device.words[probe.PIN_OUTPUT] >> 16, self.output)


class RunTest(unittest.TestCase):
    def run_probe(self, device, bits=None):
        with mock.patch.object(probe.sdram_probe, 'check_boot'), \
                mock.patch.object(probe.sdram_probe, 'require_idle'), \
                mock.patch('builtins.print'):
            return probe.run(device, bits, no_sleep)

    def test_unexpected_pin_state_writes_nothing(self):
        device = FakeXz3()
        device.words[probe.PIN_OUTPUT] = 0
        with self.assertRaises(ValueError):
            self.run_probe(device)
        self.assertEqual(device.writes, [])

    def test_missing_card_writes_nothing(self):
        device = FakeXz3(present=False)
        with self.assertRaises(ValueError):
            self.run_probe(device)
        self.assertEqual(device.writes, [])

    def test_reports_each_default_bit(self):
        results = self.run_probe(FakeXz3(cmdvccn=2))
        self.assertEqual([(r['bit'], bool(r['atr'])) for r in results],
                         [(2, True), (5, False), (7, False)])


if __name__ == '__main__':
    unittest.main()
