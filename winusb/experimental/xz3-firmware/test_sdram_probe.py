# SPDX-License-Identifier: GPL-2.0-only
"""sdram_probe.pyの判定と復元を、実機なしの模擬メモリで確認する。"""
import struct
import unittest

import sdram_probe as probe


class FakeMemory:
    """size未満を実体、それ以上を読み捨てとして扱う模擬SDRAM。

    periodを指定すると、wrap_base以降をperiodごとに同じ実体へ折り返す。
    """

    def __init__(self, size=None, period=None, wrap_base=0, run=0):
        self.size = size
        self.period = period
        self.wrap_base = wrap_base
        self.cells = {}
        self.writes = []
        self.run = run

    def _cell(self, address):
        if self.period is not None and address >= self.wrap_base:
            return self.wrap_base + (address - self.wrap_base) % self.period
        if self.size is not None and address >= self.size:
            return None
        return address

    def read(self, address, size):
        if address == probe.RUN:
            return struct.pack('>I', self.run)
        out = bytearray()
        for at in range(address, address + size):
            cell = self._cell(at)
            out.append(0xff if cell is None else self.cells.get(cell, at * 7 & 0xff))
        return bytes(out)

    def write(self, address, data):
        assert len(data) == 4 and not address & 3
        self.writes.append(address)
        for i, byte in enumerate(data):
            cell = self._cell(address + i)
            if cell is not None:
                self.cells[cell] = byte


START = 0xad0000


class AliasTest(unittest.TestCase):
    def test_full_window_keeps_every_tag_and_restores(self):
        memory = FakeMemory()
        before = {a: memory.read(a, 4) for a in range(START, probe.WINDOW_LIMIT, probe.BLOCK)}
        rows = probe.alias(memory, START, probe.WINDOW_LIMIT)
        self.assertTrue(all(ok for _, ok, _ in rows))
        self.assertEqual(probe.contiguous(rows), probe.WINDOW_LIMIT - probe.BLOCK)
        self.assertEqual({a: memory.read(a, 4) for a in before}, before)

    def test_missing_memory_ends_contiguous_range(self):
        memory = FakeMemory(size=0xb80000)
        rows = probe.alias(memory, START, probe.WINDOW_LIMIT)
        self.assertEqual(probe.contiguous(rows), 0xb70000)

    def test_wrapping_memory_is_detected_after_all_writes(self):
        # 1 MiBごとに折り返すと、後半の書き込みが前半の値を上書きする。
        memory = FakeMemory(period=0x100000, wrap_base=START)
        rows = probe.alias(memory, START, probe.WINDOW_LIMIT)
        self.assertFalse(dict((a, ok) for a, ok, _ in rows)[START])
        self.assertIsNone(probe.contiguous(rows))

    def test_restore_runs_after_a_failed_write(self):
        class Failing(FakeMemory):
            def write(self, address, data):
                if address == START + 2 * probe.BLOCK and data == probe.tag(address, 0xa11a5000):
                    raise OSError('stall')
                super().write(address, data)
        memory = Failing()
        before = memory.read(START, 4)
        with self.assertRaises(OSError):
            probe.alias(memory, START, probe.WINDOW_LIMIT)
        self.assertEqual(memory.read(START, 4), before)


class MarkTest(unittest.TestCase):
    def test_mark_then_check_reports_overwritten_words(self):
        memory = FakeMemory()
        total, changed = probe.mark(memory, START, START + 0x10000)
        self.assertEqual((total, changed), (16, []))
        memory.write(START + 0x3000, b'\0\0\0\0')
        self.assertEqual(probe.check(memory, START, START + 0x10000)[1], [(START + 0x3000, '00000000')])


class GuardTest(unittest.TestCase):
    def test_writes_stay_inside_ring_and_below_mmio(self):
        memory = FakeMemory()
        for start, end in ((0x80000, 0xb00000), (START, probe.WINDOW_LIMIT + probe.BLOCK)):
            with self.assertRaises(ValueError):
                probe.alias(memory, start, end)
        self.assertEqual(memory.writes, [])

    def test_running_dma_blocks_writes(self):
        with self.assertRaises(ValueError):
            probe.require_idle(FakeMemory(run=0x02000000))

    def test_tags_are_unique_per_block(self):
        tags = {probe.tag(a, 0xa11a5000) for a in range(probe.RING_BASE, probe.WINDOW_LIMIT, probe.BLOCK)}
        self.assertEqual(len(tags), (probe.WINDOW_LIMIT - probe.RING_BASE) // probe.BLOCK)


class RingTest(unittest.TestCase):
    def test_counts_records_with_sync_after_prefix(self):
        record = bytes(4) + b'\x47' + bytes(probe.RECORD - 5)
        self.assertEqual(probe.ts_records(record * 3 + bytes(probe.RECORD)), 3)

    def test_boot_check_rejects_other_images(self):
        with self.assertRaises(ValueError):
            probe.check_boot(FakeMemory())


if __name__ == '__main__':
    unittest.main()
