# SPDX-License-Identifier: GPL-2.0-only
"""Limited FR basic-instruction model. No device, interrupt or cache emulation."""


class Machine:
    def __init__(self, code, memory, stops=()):
        self.code = code
        self.memory = memory
        self.stops = set(stops)
        self.regs = [0x12340000+i for i in range(16)]
        self.rp = 0xfffffffe
        self.equal = False
        self.writes = []

    def load(self, address, size):
        if size == 4 and address & 3 or size == 2 and address & 1:
            raise AssertionError('unaligned access')
        for base, data in self.memory.items():
            if base <= address and address+size <= base+len(data):
                return int.from_bytes(data[address-base:address-base+size], 'big')
        raise AssertionError(f'unmapped read {address:#x}')

    def store(self, address, value):
        if address & 3:
            raise AssertionError('unaligned store')
        for base, data in self.memory.items():
            if base <= address and address+4 <= base+len(data):
                data[address-base:address-base+4] = (value & 0xffffffff).to_bytes(4, 'big')
                self.writes.append(address)
                return
        raise AssertionError(f'unmapped write {address:#x}')

    def instruction(self, pc, size):
        if pc & 1:
            raise AssertionError('unaligned instruction')
        for base, data in self.code.items():
            if base <= pc and pc+size <= base+len(data):
                return int.from_bytes(data[pc-base:pc-base+size], 'big')
        raise AssertionError(f'unmapped instruction {pc:#x}')

    def run(self, entry, limit=512):
        pc = entry
        r = self.regs
        for steps in range(limit):
            if pc in self.stops or pc == 0xfffffffe:
                return pc, steps
            inst = self.instruction(pc, 2)
            top = inst >> 8
            i, j = (inst >> 4) & 15, inst & 15
            pc += 2
            if inst == 0x9720:
                pc = self.rp
            elif inst == 0x9fa0:
                pass
            elif inst & 0xfff0 == 0x9710:
                self.rp, pc = pc, r[j]
            elif inst & 0xfff0 == 0x9700:
                pc = r[j]
            elif inst & 0xfff0 == 0x9f80:
                r[j] = self.instruction(pc, 4)
                pc += 4
            elif top == 0x9b:
                r[j] = (i << 16) | self.instruction(pc, 2)
                pc += 2
            elif inst & 0xf000 == 0xc000:
                r[j] = (inst >> 4) & 255
            elif top == 0x8b:
                r[j] = r[i]
            elif top == 0xa0:
                r[j] = (r[j]+i) & 0xffffffff
            elif top == 0xa1:
                r[j] = (r[j]+i-16) & 0xffffffff
            elif top == 0xa8:
                self.equal = r[j] == i
            elif top == 0xaa:
                self.equal = r[j] == r[i]
            elif top == 0x04:
                r[j] = self.load(r[i], 4)
            elif top in (0x01, 0x02):
                r[j] = self.load((r[13]+r[i]) & 0xffffffff, 2 if top == 0x01 else 1)
            elif top == 0x14:
                self.store(r[i], r[j])
            elif top == 0x92:
                r[j] |= r[i]
            elif top in (0xe0, 0xe2, 0xe3):
                disp = inst & 255
                if disp & 128:
                    disp -= 256
                if top == 0xe0 or (top == 0xe2 and self.equal) or (top == 0xe3 and not self.equal):
                    pc += disp*2
            else:
                raise AssertionError(f'unsupported instruction {inst:04x} at {pc-2:#x}')
        raise AssertionError('finite instruction budget exceeded')
