"""Execute assembled ARM64 service bytes, not a Python service reimplementation.

python tests/test_ub_domain_services.py --object /tmp/ud-services.o
Requires unicorn 2.x. Does not emulate Linux, OBMM, or remote page-table walks.
"""
import argparse
from pathlib import Path
import struct
import unittest
try:
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
    from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1,
        UC_ARM64_REG_X2, UC_ARM64_REG_X29, UC_ARM64_REG_X30,
        UC_ARM64_REG_SP, UC_ARM64_REG_PC)
    HAVE_UNICORN = True
except ImportError:
    HAVE_UNICORN = False

VA = 0x100000000
STACK, HEAP, ARGS = VA + 0x2000, VA + 0x4000, VA + 0x6000
IMAGES = {}


def read_images(path):
    data = Path(path).read_bytes()
    if data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError('Expected little-endian ELF64 AArch64')
    shoff = struct.unpack_from('<Q', data, 40)[0]
    entsize, count, strings_index = struct.unpack_from('<HHH', data, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * entsize)
                for i in range(count)]

    def contents(index):
        s = sections[index]
        return data[s[4]:s[4]+s[5]]

    def name(table, off):
        return table[off:table.index(b'\0', off)].decode()

    names = contents(strings_index)
    index = next(i for i, s in enumerate(sections)
                 if name(names, s[0]) == '.rodata.ud_services')
    for s in sections:
        if s[1] in (4, 9) and s[7] == index and s[5]:
            raise ValueError('Published payload has relocations')
    symbols = {}
    for i, s in enumerate(sections):
        if s[1] != 2:
            continue
        strings = contents(s[6])
        raw = contents(i)
        for off in range(0, len(raw), s[9]):
            symbol, _, _, section, value, _ = struct.unpack_from('<IBBHQQ', raw, off)
            if section == index:
                symbols[name(strings, symbol)] = value
    payload = contents(index)
    return {n: payload[symbols['ud_'+n+'_start']:symbols['ud_'+n+'_end']]
            for n in ('direct', 'middle', 'leaf', 'query')}


def machine(service, a, b=0):
    m = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for addr in (VA, STACK, HEAP, ARGS):
        m.mem_map(addr, 4096)
    m.mem_write(VA, IMAGES[service])
    m.mem_write(ARGS, struct.pack('<QQQ', a, b, 0))
    m.mem_write(HEAP, struct.pack('<Q', 100))
    for i in range(32):
        m.mem_write(HEAP+64+i*64, struct.pack('<QQQ', i, (i+1) & 31, 1000+i))
    m.reg_write(UC_ARM64_REG_X0, ARGS)
    m.reg_write(UC_ARM64_REG_X1, HEAP)
    m.reg_write(UC_ARM64_REG_SP, STACK+4096)
    m.reg_write(UC_ARM64_REG_X29, 0x1234)
    m.reg_write(UC_ARM64_REG_X30, 0x5678)
    m.hook_add(UC_HOOK_INTR, lambda uc, number, user: uc.emu_stop())
    return m


def execute(m, pc=VA):
    m.emu_start(pc, VA+4096, count=1000)
    pc = m.reg_read(UC_ARM64_REG_PC)
    instruction, = struct.unpack('<I', m.mem_read(pc, 4))
    if instruction & 0xffe0001f != 0xd4200000:
        raise AssertionError('Did not stop at a BRK')
    return (instruction >> 5) & 0xffff, pc


class Services(unittest.TestCase):
    def setUp(self):
        if not HAVE_UNICORN or not IMAGES:
            self.skipTest('Run explicitly with unicorn and --object assembled-services.o')

    def completed(self, service, a, b, expected):
        m = machine(service, a, b)
        self.assertEqual(execute(m), (0x4c0, VA+len(IMAGES[service])-4))
        self.assertEqual(struct.unpack('<Q', m.mem_read(ARGS+16, 8))[0], expected)
        self.assertEqual(m.reg_read(UC_ARM64_REG_SP), STACK+4096)
        self.assertEqual(m.reg_read(UC_ARM64_REG_X29), 0x1234)
        self.assertEqual(m.reg_read(UC_ARM64_REG_X30), 0x5678)

    def test_direct(self):
        self.completed('direct', 20, 22, 142)

    def test_direct_wrap(self):
        self.completed('direct', (1 << 64)-1, 0, 99)

    def test_leaf(self):
        self.completed('leaf', 41, 0, 42)

    def test_query_1(self):
        self.completed('query', 1, 0, 1000)

    def test_query_8(self):
        self.completed('query', 8, 7, 1007)

    def test_query_32(self):
        self.completed('query', 32, 31, 1031)

    def test_query_miss(self):
        self.completed('query', 32, 999, 0)

    def test_nested_preserves_middle_stack(self):
        middle = machine('middle', 41)
        self.assertEqual(execute(middle), (0x4c1, VA+12))
        sp = middle.reg_read(UC_ARM64_REG_SP)
        saved = bytes(middle.mem_read(sp, 16))
        self.assertEqual(sp, STACK+4096-16)
        leaf = machine('leaf', middle.reg_read(UC_ARM64_REG_X2))
        self.assertEqual(execute(leaf)[0], 0x4c0)
        result, = struct.unpack('<Q', leaf.mem_read(ARGS+16, 8))
        self.assertEqual(bytes(middle.mem_read(sp, 16)), saved)
        middle.reg_write(UC_ARM64_REG_X2, result)
        self.assertEqual(execute(middle, VA+16), (0x4c0, VA+len(IMAGES['middle'])-4))
        self.assertEqual(struct.unpack('<Q', middle.mem_read(ARGS+16, 8))[0], 43)
        self.assertEqual(middle.reg_read(UC_ARM64_REG_SP), STACK+4096)
        self.assertEqual(middle.reg_read(UC_ARM64_REG_X29), 0x1234)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--object', required=True)
    args = parser.parse_args()
    if not HAVE_UNICORN:
        parser.error('Install unicorn to run the ARM64 instruction tests')
    IMAGES.update(read_images(args.object))
    unittest.main(argv=['test_ub_domain_services'], verbosity=2)
