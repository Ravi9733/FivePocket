"""Exercise actual patched helper and emitted operand placement for type4."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_normpacked32 import patched_sources, manage


class NormShort4Tests(unittest.TestCase):
    def test_patched_emission_and_helper(self):
        compiler = shutil.which('clang++')
        if not compiler:
            self.skipTest('clang++ unavailable')
        with tempfile.TemporaryDirectory(prefix='vmx-short4-emitter-') as temporary:
            directory = Path(temporary)
            source, context = patched_sources(directory)
            pack = source.split('case PPC_INST_VPKD3D128:', 1)[1].split('case PPC_INST_VPKSHUS:', 1)[0]
            self.assertIn('if (insn.operands[2] != 2 && insn.operands[2] != 4)', pack)
            pack = pack.split('case 4: // NORMSHORT4:', 1)[1].split('case 3:', 1)[0]
            self.assertIn('insn.operands[3] >= 1 && insn.operands[3] <= 3 && insn.operands[4] <= 3', pack)
            self.assertIn('insn.operands[3], insn.operands[4], v(insn.operands[0]), v(insn.operands[1])', pack)
            unpack = source.split('case PPC_INST_VUPKD3D128:', 1)[1].split('case 4: // NORMSHORT4:', 1)[1].split('case 3:', 1)[0]
            self.assertIn('v(insn.operands[0]), v(insn.operands[1])', unpack)
            templates = lambda text: [json.loads(m[1]) for m in re.finditer(r'println\(("(?:\\.|[^"\\])*")', text)]
            p, u = templates(pack), templates(unpack)
            self.assertEqual(p[1].strip(), '__builtin_debugtrap();')
            self.assertEqual(len(u), 1)
            helper = context.split('namespace recomp::vmx {', 1)[1].split('} // namespace recomp::vmx', 1)[0]
            program = '#include <cstdint>\nnamespace recomp::vmx {' + helper + '}\n'
            refs = 'struct Output {std::uint32_t (&u32)[4];}; struct Input {const std::uint32_t (&u32)[4];}; Output output{dst};Input input{src};\n'
            program += 'namespace emitted {\ntemplate<unsigned Mask,unsigned Shift>\nvoid packNormShort4(std::uint32_t (&dst)[4],const std::uint32_t (&src)[4]) {\n'
            program += refs + p[0].format('Mask', 'Shift', 'output', 'input') + '\n}\n'
            program += 'void unpackNormShort4(std::uint32_t (&dst)[4],const std::uint32_t (&src)[4]) {\n'
            program += refs + u[0].format('output', 'input') + '\n}}\n'
            tests = (manage.ROOT / 'android/tests/guest_vmx_short_test.cpp').read_text()
            program += tests.replace('#include "guest_vmx_short.h"', '').replace('recomp::vmx::', 'emitted::')
            path = directory / 'probe.cpp'; path.write_text(program)
            executable = directory / 'probe'
            compiled = subprocess.run([compiler, '-std=c++20', '-O2', '-ffp-model=strict',
                '-fno-strict-aliasing', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                str(path), '-o', str(executable)], text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(executable)], text=True, capture_output=True, check=True)
            self.assertIn('PASS 65536 909348', result.stdout)
