"""Execute actual AArch64 route instructions with a flat Unicorn memory fixture.

Usage: python3 tests/test_social_services.py --object /tmp/social_services.o
This checks instructions and RO accesses, not MMU, NC timing, UB or Linux gates.
"""
import argparse
import struct
import unittest
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn import UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC
import unicorn.arm64_const as arm
from test_nested_gate import elf_section

ARTIFACT = None


@unittest.skipUnless(ARTIFACT, 'run with --object')
class Services(unittest.TestCase):
    def test_write_profile_only_touches_bounded_data_page(self):
        payload, symbols = ARTIFACT
        code = payload[symbols['us_nginx_forward_write_start']:
                       symbols['us_nginx_forward_write_end']]
        for request_type, expected_target in ((13, 104), (9, 102)):
            with self.subTest(request_type=request_type):
                cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
                args, heap, base, data = 0x100000, 0x200000, 0x300000, 0x100008000
                cpu.mem_map(args, 4096, UC_PROT_READ | UC_PROT_WRITE)
                cpu.mem_map(heap, 4096, UC_PROT_READ | UC_PROT_WRITE)
                cpu.mem_map(base, 4096, UC_PROT_READ | UC_PROT_EXEC)
                cpu.mem_map(data, 4096, UC_PROT_READ | UC_PROT_WRITE)
                cfg = (1, 2, 15360, 1, 2, 0, 0, 0, 101, 102, 103, 1, 0)
                cpu.mem_write(heap, struct.pack('<13I', *cfg))
                cpu.mem_protect(heap, 4096, UC_PROT_READ)
                message = (17, 513, 99, 4096, 128, 2048, 42, 0,
                           request_type, 0, 0, 0)
                cpu.mem_write(args, struct.pack('<8Q4I', *message))
                cpu.mem_write(base, code)
                cpu.reg_write(arm.UC_ARM64_REG_X0, args)
                cpu.reg_write(arm.UC_ARM64_REG_X1, heap)
                stopped = []
                def hook(uc, pc, size, unused):
                    instruction = struct.unpack('<I', bytes(uc.mem_read(pc, 4)))[0]
                    if instruction & 0xffe0001f == 0xd4200000:
                        self.assertEqual(pc, base + len(code) - 4)
                        stopped.append(pc)
                        uc.emu_stop()
                cpu.hook_add(UC_HOOK_CODE, hook)
                cpu.emu_start(base, base + len(code), count=64)
                self.assertEqual(len(stopped), 1)
                result = struct.unpack('<8Q4I', bytes(cpu.mem_read(args, 80)))
                self.assertEqual(result[7], 42 if request_type == 13 else 0)
                self.assertEqual(result[9:11], (1, expected_target))
                self.assertEqual(struct.unpack('<Q', bytes(cpu.mem_read(data+8, 8)))[0],
                                 42 if request_type == 13 else 0)
                self.assertEqual(bytes(cpu.mem_read(heap, 52)), struct.pack('<13I', *cfg))

    def test_actual_instructions_preserve_message_and_only_read_config(self):
        payload, symbols = ARTIFACT
        for domain, name in enumerate(('lb_forward','nginx_forward','nginx_return','lb_return')):
            code = payload[symbols['us_'+name+'_start']:symbols['us_'+name+'_end']]
            for key, home in ((key, home) for key in (0,1,15359,15360,2**32-1)
                              for home in (False, True)):
                with self.subTest(domain=domain,key=key,home=home):
                    cpu = Uc(UC_ARCH_ARM64,UC_MODE_ARM)
                    args,heap,base = 0x100000,0x200000,0x300000
                    cpu.mem_map(args,4096,UC_PROT_READ|UC_PROT_WRITE)
                    cpu.mem_map(heap,4096,UC_PROT_READ|UC_PROT_WRITE)
                    cpu.mem_map(base,4096,UC_PROT_READ|UC_PROT_EXEC)
                    cfg = (1,1 if domain in (0,3) else 2,15360,1,2,0,0,0,101,102,103,1,0)
                    cpu.mem_write(heap,struct.pack('<13I',*cfg))
                    cpu.mem_protect(heap,4096,UC_PROT_READ)
                    message = (17,key,99,4096,128,2048,123,0,
                               (11 if home else 9) if domain<2 else (12 if home else 10),0,0,0)
                    cpu.mem_write(args,struct.pack('<8Q4I',*message));cpu.mem_write(base,code)
                    cpu.reg_write(arm.UC_ARM64_REG_X0,args);cpu.reg_write(arm.UC_ARM64_REG_X1,heap)
                    stopped,reads = [],[]
                    def hook(uc,pc,size,unused):
                        instruction = struct.unpack('<I',bytes(uc.mem_read(pc,4)))[0]
                        if instruction & 0xffe0001f == 0xd4200000:
                            self.assertEqual((instruction>>5)&0xffff,0x4c0)
                            self.assertEqual(pc,base+len(code)-4)
                            stopped.append(pc);uc.emu_stop()
                    def read(uc,access,address,size,value,unused):
                        if heap<=address<heap+4096: reads.append(address)
                    cpu.hook_add(UC_HOOK_CODE,hook);cpu.hook_add(UC_HOOK_MEM_READ,read)
                    cpu.emu_start(base,base+len(code),count=64)
                    self.assertEqual(len(stopped),1)
                    actual = struct.unpack('<8Q4I',bytes(cpu.mem_read(args,80)))
                    expected = list(message)
                    expected[9],expected[10] = ((2,2),(1,103 if home else 102),(2,1),(3,0))[domain]
                    self.assertEqual(actual,tuple(expected))
                    self.assertEqual(bytes(cpu.mem_read(heap,52)),struct.pack('<13I',*cfg))
                    if domain<3: self.assertTrue(reads)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--object',required=True)
    args = parser.parse_args()
    ARTIFACT = elf_section(args.object,'.rodata.us_services')
    # The decorator is evaluated on import, before CLI artifacts are loaded.
    Services.__unittest_skip__ = False
    unittest.main(argv=['social_services'],verbosity=2)
