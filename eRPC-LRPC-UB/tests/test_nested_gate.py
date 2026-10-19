"""Execute actual fused gate AND published services with register fixtures.

Requires Unicorn 2.x and separately assembled objects (see README). MRS/MSR,
ERET and exception entry are fixtures; this is not MMU, VHE or UB validation.
"""
import argparse
from pathlib import Path
import struct
import unittest

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
import unicorn.arm64_const as arm

BASE, FRAME, KSTACK, STOP = 0x100000, 0x200000, 0x300000, 0x400000
VA, SSTACK, HEAP, ARGS = 0x100000000, 0x100002000, 0x100004000, 0x100006000
MARGS, LARGS = FRAME + 0x1000, FRAME + 0x2000
REG_NAMES = ('ttbr0', 'tcr', 'vbar', 'sp_el0', 'daif', 'elr', 'spsr', 'esr', 'far')
ARTIFACTS = None


def elf_section(path, wanted):
    raw = Path(path).read_bytes()
    if raw[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', raw, 18)[0] != 183:
        raise ValueError('Expected little-endian AArch64 ELF64')
    off = struct.unpack_from('<Q', raw, 40)[0]
    width, count, names_index = struct.unpack_from('<HHH', raw, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', raw, off + i * width)
                for i in range(count)]
    def data(s): return raw[s[4]:s[4] + s[5]]
    def name(table, n): return table[n:table.index(b'\0', n)].decode()
    names = data(sections[names_index])
    index = next(i for i, s in enumerate(sections) if name(names, s[0]) == wanted)
    symbols = {}
    for s in sections:
        if s[1] in (4, 9) and s[7] == index and s[5]:
            raise ValueError('Unresolved relocations in ' + wanted)
        if s[1] != 2:
            continue
        strings = data(sections[s[6]])
        for pos in range(s[4], s[4] + s[5], s[9]):
            n, _, _, sh, value, _ = struct.unpack_from('<IBBHQQ', raw, pos)
            if sh == index:
                symbols[name(strings, n)] = value
    return data(sections[index]), symbols


class NestedGate(unittest.TestCase):
    def execute(self, mode='el1', value=41, inject=None):
        artifact = ARTIFACTS[mode]
        code, symbols, fixture = artifact
        images = ARTIFACTS['images']
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for address, size in ((BASE, 0x4000), (FRAME, 0x4000), (KSTACK, 0x4000),
                              (STOP, 4096), (VA, 4096), (SSTACK, 4096),
                              (HEAP, 4096), (ARGS, 4096)):
            cpu.mem_map(address, size)
        cpu.mem_write(BASE, code)
        xreg = lambda n: getattr(arm, 'UC_ARM64_REG_X' + str(n))
        read64 = lambda address: struct.unpack('<Q', cpu.mem_read(address, 8))[0]
        write64 = lambda address, v: cpu.mem_write(address, struct.pack('<Q', v))
        contexts = []
        for d in range(2):
            r = [0x7000 + d * 0x1000 + n for n in range(37)]
            r[0], r[1], r[31], r[32] = ARGS, HEAP, SSTACK + 4096, VA
            contexts.append(r)
        middle_root, leaf_root, empty_root = (0x42 << 48) | 0x800000, (0x43 << 48) | 0x900000, 0xa00000
        tail = [middle_root, leaf_root, MARGS, LARGS, VA + 12,
                VA + len(images['leaf']) - 4, VA + len(images['middle']) - 4,
                0, 0, 0]
        cpu.mem_write(FRAME, struct.pack('<84Q', *contexts[0], *contexts[1], *tail))
        cpu.mem_write(MARGS, struct.pack('<QQQ', value, 0, 0))
        cpu.mem_write(LARGS, struct.pack('<QQQ', 0xdead, 0xbeef, 0xbad))
        encodings = {(word >> 5) & 0xffff: name for (word,), name in
                     zip(struct.iter_unpack('<I', fixture), REG_NAMES)}
        regs = {name: 0x50000 + i * 0x100 for i, name in enumerate(REG_NAMES)}
        saved = dict(regs)
        caller = [0x1100 + n for n in range(31)]
        caller[30] = STOP
        for n, v in enumerate(caller):
            cpu.reg_write(xreg(n), v)
        caller_sp = KSTACK + 0x3000
        cpu.reg_write(arm.UC_ARM64_REG_SP, caller_sp)
        shadow_tcr = 0x123456
        for n, v in enumerate((FRAME, shadow_tcr, empty_root)):
            cpu.reg_write(xreg(n), v)
        mode_state = {'user': False, 'entry': 0, 'domain': None, 'faulted': False}
        stacks = {'middle': bytes(4096), 'leaf': bytes(4096)}
        writes, entries, suspensions, tlbis = [], [], [], []

        def trap(uc, pc, immediate, vector=8, ec=0x3c):
            domain = mode_state['domain']
            pointer = MARGS if domain == 'middle' else LARGS
            uc.mem_write(pointer, bytes(uc.mem_read(ARGS, 4096)))
            stacks[domain] = bytes(uc.mem_read(SSTACK, 4096))
            suspensions.append([uc.reg_read(xreg(n)) for n in range(31)])
            regs.update(elr=pc, esr=(ec << 26) | immediate,
                        far=0xdeadbeef if ec != 0x3c else 0,
                        sp_el0=uc.reg_read(arm.UC_ARM64_REG_SP), spsr=0x3c0)
            mode_state['user'] = False
            uc.reg_write(arm.UC_ARM64_REG_SP, caller_sp - 256)
            uc.reg_write(arm.UC_ARM64_REG_PC, regs['vbar'] + vector * 128)

        def hook(uc, pc, size, unused):
            if pc == STOP:
                uc.emu_stop()
                return
            instruction = struct.unpack('<I', uc.mem_read(pc, 4))[0]
            if mode_state['user']:
                if instruction & 0xffe0001f == 0xd4200000:
                    immediate = (instruction >> 5) & 0xffff
                    phase = mode_state['entry'] - 1
                    fault = inject if inject and inject[0] == phase else None
                    if fault:
                        mode_state['faulted'] = True
                    trap(uc, pc + (4 if fault and fault[1] == 'pc' else 0),
                         immediate ^ (1 if fault and fault[1] == 'immediate' else 0),
                         9 if fault and fault[1] == 'vector' else 8,
                         0x24 if fault and fault[1] == 'abort' else 0x3c)
                return
            if instruction == 0xd69f03e0:  # ERET fixture
                domain = 'middle' if regs['ttbr0'] == middle_root else 'leaf'
                self.assertIn(regs['ttbr0'], (middle_root, leaf_root))
                self.assertEqual(regs['tcr'], shadow_tcr)
                self.assertEqual(regs['spsr'], 0x3c0)
                pointer = MARGS if domain == 'middle' else LARGS
                uc.mem_write(VA, images[domain] + bytes(4096 - len(images[domain])))
                # Different shadow roots map the same VA to different code.
                # Discard Unicorn's host translations when changing the fixture.
                uc.ctl_remove_cache(VA, VA + 4096)
                uc.mem_write(ARGS, bytes(uc.mem_read(pointer, 4096)))
                uc.mem_write(SSTACK, stacks[domain])
                mode_state.update(user=True, domain=domain, entry=mode_state['entry'] + 1)
                entries.append((domain, regs['elr']))
                uc.reg_write(arm.UC_ARM64_REG_SP, regs['sp_el0'])
                uc.reg_write(arm.UC_ARM64_REG_PC, regs['elr'])
                return
            kind = instruction & 0xfff00000
            if kind == 0xd5300000:  # MRS fixture
                name = encodings[(instruction >> 5) & 0xffff]
                uc.reg_write(xreg(instruction & 31), regs[name])
                uc.reg_write(arm.UC_ARM64_REG_PC, pc + 4)
            elif kind == 0xd5100000:  # MSR fixture
                name = encodings[(instruction >> 5) & 0xffff]
                regs[name] = uc.reg_read(xreg(instruction & 31))
                writes.append((name, regs[name]))
                uc.reg_write(arm.UC_ARM64_REG_PC, pc + 4)
            elif instruction & 0xfff80000 == 0xd5080000:  # SYS/TLBI fixture
                tlbis.append(instruction)
                uc.reg_write(arm.UC_ARM64_REG_PC, pc + 4)
            elif instruction & 0xfffff01f == 0xd503401f:  # DAIFSet
                regs['daif'] |= 0x3c0
                uc.reg_write(arm.UC_ARM64_REG_PC, pc + 4)

        cpu.hook_add(UC_HOOK_CODE, hook)
        entry = 'ud_enter_nested_vhe' if mode == 'vhe' else 'ud_enter_nested'
        try:
            cpu.emu_start(BASE + symbols[entry], STOP, count=5000)
        except Exception as error:
            pc = cpu.reg_read(arm.UC_ARM64_REG_PC)
            raise AssertionError(f'{error}: pc={pc:#x} state={mode_state} entries={entries}') from error
        self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_PC), STOP, 'gate must return within instruction budget')
        self.assertEqual(cpu.reg_read(arm.UC_ARM64_REG_SP), caller_sp)
        for n in range(18, 31):
            self.assertEqual(cpu.reg_read(xreg(n)), caller[n], f'caller x{n}')
        for name in ('ttbr0', 'tcr', 'vbar', 'sp_el0', 'daif', 'elr', 'spsr'):
            self.assertEqual(regs[name], saved[name], name)
        self.assertEqual(tlbis, [], 'steady gate must retain ASID translations')
        root_writes = [v for name, v in writes if name == 'ttbr0']
        self.assertEqual(root_writes[-2:], [empty_root, saved['ttbr0']])
        self.assertNotIn(saved['ttbr0'], root_writes[:-1], 'no intermediate restore of A')
        self.assertEqual([v for name, v in writes if name == 'tcr'], [shadow_tcr, saved['tcr']])
        self.assertEqual(len([1 for name, _ in writes if name == 'vbar']), 2)
        if inject:
            self.assertTrue(mode_state['faulted'])
            self.assertEqual(read64(FRAME + 656), 0)
            self.assertEqual(read64(FRAME + 648), inject[0] + 2)
            self.assertEqual(len(entries), inject[0] + 1)
            self.assertEqual(read64(FRAME + 664), 2 if inject[0] == 1 else 1)
            diagnostic = FRAME + (296 if inject[0] == 1 else 0)
            self.assertEqual(read64(diagnostic + 288), 9 if inject[1] == 'vector' else 8)
            if inject[1] == 'abort':
                self.assertEqual(read64(diagnostic + 272) >> 26, 0x24)
                self.assertEqual(read64(diagnostic + 280), 0xdeadbeef)
            if inject[1] == 'pc':
                expected_pc = (VA + 12, VA + len(images['leaf']) - 4,
                               VA + len(images['middle']) - 4)[inject[0]]
                self.assertEqual(read64(diagnostic + 256), expected_pc + 4)
            return
        self.assertEqual(entries, [('middle', VA), ('leaf', VA), ('middle', VA + 16)])
        self.assertEqual(root_writes, [empty_root, middle_root, empty_root, leaf_root,
                                      empty_root, middle_root, empty_root, saved['ttbr0']])
        self.assertEqual(read64(MARGS + 16), (value + 2) & ((1 << 64) - 1))
        self.assertEqual(read64(FRAME + 648), 4)
        self.assertEqual(read64(FRAME + 656), 1)
        self.assertEqual(read64(FRAME + 664), 1)
        # Only the middle suspension saves GPRs. Leaf's dead GPRs stay initial.
        middle = struct.unpack('<31Q', cpu.mem_read(FRAME, 31 * 8))
        expected_middle = list(suspensions[0])
        expected_middle[2] = (value + 1) & ((1 << 64) - 1)
        self.assertEqual(list(middle), expected_middle)
        self.assertEqual(list(struct.unpack('<31Q', cpu.mem_read(FRAME + 296, 31 * 8))), contexts[1][:31])
        self.assertEqual(suspensions[2][29:31], contexts[0][29:31], 'middle stack frame must survive leaf')

    def test_el1_chain(self): self.execute()
    def test_vhe_chain(self): self.execute('vhe')

    def test_vhe_register_encodings(self):
        el1, _, old_fixture = ARTIFACTS['el1']
        vhe, _, new_fixture = ARTIFACTS['vhe']
        old = [((word >> 5) & 0xffff) for (word,) in struct.iter_unpack('<I', old_fixture)]
        new = [((word >> 5) & 0xffff) for (word,) in struct.iter_unpack('<I', new_fixture)]
        self.assertEqual(len(el1), len(vhe))
        pairs = {(old[i], new[i]) for i in (0, 1, 2, 5, 6, 7, 8)}
        seen = set()
        for (left,), (right,) in zip(struct.iter_unpack('<I', el1), struct.iter_unpack('<I', vhe)):
            if left == right: continue
            self.assertEqual(left & 0xfff00000, right & 0xfff00000)
            self.assertEqual(left & 0x1f, right & 0x1f)
            pair = ((left >> 5) & 0xffff, (right >> 5) & 0xffff)
            self.assertIn(pair, pairs)
            seen.add(pair)
        self.assertEqual(seen, pairs)

    def test_argument_forwarding_and_wrap(self):
        for mode in ('el1', 'vhe'):
            for value in (0, 1, 99, (1 << 64) - 2, (1 << 64) - 1):
                with self.subTest(mode=mode, value=value): self.execute(mode, value)

    def test_faults_restore_A_in_each_phase(self):
        for mode in ('el1', 'vhe'):
            for phase in range(3):
                for fault in ('immediate', 'pc', 'vector', 'abort'):
                    with self.subTest(mode=mode, phase=phase, fault=fault):
                        self.execute(mode, inject=(phase, fault))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--objects', required=True, type=Path)
    args = parser.parse_args()
    ARTIFACTS = {}
    for mode, suffix in (('el1', ''), ('vhe', '_vhe')):
        code, symbols = elf_section(args.objects / ('gate_nested' + suffix + '.o'), '.text')
        fixture, _ = elf_section(args.objects / ('sysregs' + suffix + '.o'), '.text')
        ARTIFACTS[mode] = code, symbols, fixture
    payload, symbols = elf_section(args.objects / 'services.o', '.rodata.ud_services')
    ARTIFACTS['images'] = {name: payload[symbols['ud_' + name + '_start']:symbols['ud_' + name + '_end']]
                           for name in ('middle', 'leaf')}
    unittest.main(argv=['test_nested_gate'], verbosity=2)
