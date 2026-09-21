"""Configure-only integration checks; no SDK or kernel runtime is simulated."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cmake') and shutil.which('gcc'), 'requires cmake and gcc')
class UpstreamBuild(unittest.TestCase):
    def configure(self, directory, arch='aarch64', sdk=True):
        p = Path(directory)
        (p / 'shim.c').write_text('int shim(void) { return 0; }\n')
        (p / 'ubs_mem.h').write_text('/* configure fixture only */\n')
        (p / 'libobmm.h').write_text('/* configure fixture only */\n')
        (p / 'libubsm.a').write_bytes(b'')
        (p / 'libobmm.a').write_bytes(b'')
        (p / 'toolchain.cmake').write_text(
            'set(CMAKE_SYSTEM_NAME Generic)\n'
            f'set(CMAKE_SYSTEM_PROCESSOR {arch})\n'
            f'set(CMAKE_C_COMPILER "{Path(shutil.which("gcc")).as_posix()}")\n')
        (p / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.19)\nproject(eRPC C)\n'
            'add_library(erpc STATIC shim.c)\ntarget_link_libraries(erpc m)\n'
            'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/attached.txt" CONTENT '
            '"$<TARGET_PROPERTY:erpc,SOURCES>\\n$<TARGET_PROPERTY:erpc,COMPILE_DEFINITIONS>")\n')
        return subprocess.run([
            shutil.which('cmake'), '-S', str(p), '-B', str(p / 'out'),
            '-G', 'Unix Makefiles', '-DCMAKE_MAKE_PROGRAM=' + (shutil.which('make') or 'make'),
            '-DCMAKE_C_COMPILER_WORKS=1',
            '-DCMAKE_TOOLCHAIN_FILE=' + str(p / 'toolchain.cmake'),
            '-DCMAKE_PROJECT_eRPC_INCLUDE=' + str(ROOT / 'cmake/ub-domain-upstream.cmake'),
            '-DUBSM_INCLUDE_DIR=' + str(p if sdk else p / 'missing'),
            '-DUBSM_LIBRARY=' + str(p / 'libubsm.a'),
            '-DOBMM_INCLUDE_DIR=' + str(p), '-DOBMM_LIBRARY=' + str(p / 'libobmm.a')
        ], text=True, capture_output=True)

    def test_attach_sources_and_definitions(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.configure(tmp)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            manifest = (Path(tmp) / 'out/attached.txt').read_text()
            self.assertIn('ub_domain_client.c', manifest)
            self.assertIn('ubsm_pa.c', manifest)
            self.assertIn('LRPC_UB_DOMAIN=1', manifest)

    def test_reject_wrong_architecture(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.configure(tmp, arch='x86_64')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Expected patched eRPC target on ARM64', result.stderr)

    def test_reject_missing_sdk(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.configure(tmp, sdk=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Set UBSM_INCLUDE_DIR and UBSM_LIBRARY', result.stderr)


if __name__ == '__main__':
    unittest.main()
