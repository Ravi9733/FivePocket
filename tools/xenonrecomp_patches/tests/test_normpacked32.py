"""Execute the real patched generator expressions, not a copied approximation."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import manage


def patched_sources(directory):
    manage.ensure_pinned()
    env = dict(os.environ, GIT_INDEX_FILE=str(directory / 'index'))
    subprocess.run(['git', 'read-tree', 'HEAD'], cwd=manage.SUBMODULE,
                   env=env, check=True, capture_output=True)
    for patch in manage.patches():
        subprocess.run(['git', 'apply', '--cached', str(patch)], cwd=manage.SUBMODULE,
                       env=env, check=True, capture_output=True)
    return tuple(subprocess.run(['git', 'show', ':' + path], cwd=manage.SUBMODULE,
                               env=env, check=True, text=True, capture_output=True).stdout
                 for path in ('XenonRecomp/recompiler.cpp', 'XenonUtils/ppc_context.h'))


class NormPacked32Tests(unittest.TestCase):
    def test_patched_emission_matches_numeric_and_alias_reference(self):
        compiler = shutil.which('clang++')
        if not compiler:
            self.skipTest('clang++ unavailable')
        with tempfile.TemporaryDirectory(prefix='vmx-packed32-emitter-') as temporary:
            directory = Path(temporary)
            source, _ = patched_sources(directory)
            unpack = source.split('case PPC_INST_VUPKD3D128:', 1)[1]
            block = re.split(r'\n\s*case [0-9]+:', unpack.split('case 2: // NORMPACKED32:', 1)[1], maxsplit=1)[0]
            templates = [json.loads(m[1]) for m in re.finditer(r'println\(("(?:\\.|[^"\\])*")', block)]
            self.assertEqual(len(templates), 4)
            lines = []
            for i in range(3):
                lines += [templates[0].format('temp', 'input', i * 10),
                          templates[1].format('result', 3 - i, 'temp', 'temp')]
            lines += [templates[2].format('result', 'input')]
            lines += ['if (&dst[0] == &src[0]) {', templates[3].format('input', 'result'),
                      'std::memcpy(dst,input.u32,16);', '} else {',
                      templates[3].format('output', 'result'), 'std::memcpy(dst,output.u32,16);', '}']
            program = '''#include <cstdint>
#include <cstring>
namespace emitted {
void unpackNormPacked32(std::uint32_t (&dst)[4],const std::uint32_t (&src)[4]) {
    struct Vector {std::uint32_t u32[4];};
    struct Scalar {std::uint32_t u32;};
    Vector input{},result{},output{};Scalar temp{};
    std::memcpy(input.u32,src,16);
''' + '\n'.join(lines) + '\n}}\n'
            # Same independent arithmetic and published vectors test both the
            # staged native helper and the actual generated alias-safe expressions.
            tests = (manage.ROOT / 'android/tests/guest_vmx_packed_test.cpp').read_text()
            program += tests.replace('#include "guest_vmx_packed.h"', '').replace(
                'recomp::vmx::unpackNormPacked32', 'emitted::unpackNormPacked32')
            path = directory / 'probe.cpp'
            path.write_text(program)
            executable = directory / 'probe'
            compiled = subprocess.run([compiler, '-std=c++20', '-O2', '-ffp-model=strict',
                            '-fno-strict-aliasing', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                            str(path), '-o', str(executable)], text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(executable)], text=True, check=True, capture_output=True)
            self.assertIn('PASS 1060869', result.stdout)

    def test_patched_pack_emission_and_helper_all_masks(self):
        compiler = shutil.which('clang++')
        if not compiler:
            self.skipTest('clang++ unavailable')
        with tempfile.TemporaryDirectory(prefix='vmx-packed32-pack-emitter-') as temporary:
            directory = Path(temporary)
            source, context = patched_sources(directory)
            pack = source.split('case PPC_INST_VPKD3D128:', 1)[1].split('case PPC_INST_VPKSHUS:', 1)[0]
            self.assertIn('if (insn.operands[2] != 2 && insn.operands[2] != 4)\n            printSetFlushMode(true);', pack)
            block = re.split(r'\n\s*case [0-9]+:', pack.split('case 2: // NORMPACKED32:', 1)[1], maxsplit=1)[0]
            self.assertIn('insn.operands[3] >= 1 && insn.operands[3] <= 3 && insn.operands[4] <= 3', block)
            self.assertIn('insn.operands[3], insn.operands[4], v(insn.operands[0]), v(insn.operands[1])', block)
            templates = [json.loads(m[1]) for m in re.finditer(r'println\(("(?:\\.|[^"\\])*")', block)]
            self.assertEqual(templates[1].strip(), '__builtin_debugtrap();')
            helper = context.split('namespace recomp::vmx {', 1)[1].split('} // namespace recomp::vmx', 1)[0]
            program = '#include <cstdint>\nnamespace recomp::vmx {' + helper + '}\n'
            program += 'namespace emitted {\ntemplate<unsigned Mask,unsigned Shift>\n'
            program += 'void packNormPacked32(std::uint32_t (&dst)[4],const std::uint32_t (&src)[4]) {\n'
            program += 'struct Vector {std::uint32_t u32[4];}; Vector input{},output{};\n'
            program += 'for(unsigned i=0;i<4;++i) {input.u32[i]=src[i];output.u32[i]=dst[i];}\n'
            program += 'if (&dst[0]==&src[0]) {\n' + templates[0].format('Mask', 'Shift', 'input', 'input')
            program += '\nfor(unsigned i=0;i<4;++i) dst[i]=input.u32[i];\n} else {\n'
            program += templates[0].format('Mask', 'Shift', 'output', 'input')
            program += '\nfor(unsigned i=0;i<4;++i) dst[i]=output.u32[i];\n}}}\n'
            tests = (manage.ROOT / 'android/tests/guest_vmx_packed_pack_test.cpp').read_text()
            program += tests.replace('#include "guest_vmx_packed.h"', '').replace(
                'recomp::vmx::packNormPacked32', 'emitted::packNormPacked32')
            path = directory / 'probe.cpp'
            path.write_text(program)
            executable = directory / 'probe'
            compiled = subprocess.run([compiler, '-std=c++20', '-O2', '-ffp-model=strict',
                            '-fno-strict-aliasing', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                            str(path), '-o', str(executable)], text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(executable)], text=True, check=True, capture_output=True)
            self.assertIn('PASS 787044', result.stdout)


if __name__ == '__main__':
    unittest.main()
