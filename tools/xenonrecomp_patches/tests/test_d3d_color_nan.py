"""Execute the D3DCOLOR clamp emitted by the complete pinned patch queue."""
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


class D3DColorNaNTests(unittest.TestCase):
    def test_emitted_clamp_numeric_boundaries_and_nan_masks(self):
        compiler = shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler unavailable")
        manage.ensure_pinned()
        with tempfile.TemporaryDirectory(prefix="vmx-color-test-") as temporary:
            directory = Path(temporary)
            environment = dict(os.environ, GIT_INDEX_FILE=str(directory / "index"))
            subprocess.run(["git", "read-tree", "HEAD"], cwd=manage.SUBMODULE,
                           env=environment, check=True, capture_output=True)
            for patch in manage.patches():
                subprocess.run(["git", "apply", "--cached", str(patch)],
                               cwd=manage.SUBMODULE, env=environment,
                               check=True, capture_output=True)
            source = subprocess.run(["git", "show", ":XenonRecomp/recompiler.cpp"],
                                    cwd=manage.SUBMODULE, env=environment,
                                    text=True, capture_output=True, check=True).stdout
            block = source.split("case PPC_INST_VPKD3D128:", 1)[1].split("case PPC_INST_VPKSHUS:", 1)[0]
            line = next(line for line in block.splitlines() if "3.0f ? 3.0f" in line or "3.0f) ? 3.0f" in line)
            literal = re.search(r'println\(("(?:\\.|[^"\\])*")', line).group(1)
            template = json.loads(literal)
            expression = template.format("result", 0, "input", 0, "input", 0,
                                         "result", 0, "result", 0, "input", 0)
            self.assertIn("!(input.f32[0] >= 3.0f)", expression)
            program = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
union Register { std::uint32_t u32[4]; float f32[4]; std::uint8_t u8[16]; };
static std::uint8_t component(std::uint32_t bits) {
    Register input{}, result{};
    input.u32[0]=bits;result.u32[0]=0x404000ff;
    EMITTED_EXPRESSION
    return result.u8[0];
}
static unsigned reference(std::uint32_t bits) {
    if ((bits&0x80000000u) || (bits&0x7fffffffu)>0x7f800000u || bits<0x40400000u) return 0;
    if (bits>0x404000ffu) return 255;
    return bits&255;
}
int main() {
    const std::uint32_t edges[]={0,0x80000000,1,0x007fffff,0x00800000,
        0x3f800000,0x403fffff,0x40400000,0x40400001,0x404000fe,0x404000ff,
        0x40400100,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,
        0x7f800001,0xff800001,0x7fc00000,0xffc00000,0x7fffffff,0xffffffff};
    for(auto bits:edges)assert(component(bits)==reference(bits));
    for(std::uint32_t low=0;low<256;++low)assert(component(0x40400000|low)==low);
    for(std::uint32_t payload=1;payload<0x800000;payload+=8191) {
        assert(component(0x7f800000|payload)==0);
        assert(component(0xff800000|payload)==0);
    }
    // Actual idiom: pack an all-ones vector into its W lane to create XYZ=all
    // ones, W=zero. This is a mask construction, not a request to clamp game bounds.
    std::uint32_t packed=std::uint32_t(component(0xffffffff))*0x01010101u;
    assert(packed==0);
    const std::uint32_t centerW=0x7f7fffff;
    assert(((centerW&packed)|0x3f800000)==0x3f800000);
    std::puts("emitted D3DCOLOR: finite boundaries, infinity, signed NaNs, zero-mask idiom PASS");
}
'''.replace("EMITTED_EXPRESSION", expression)
            path = directory / "probe.cpp"
            path.write_text(program)
            executable = directory / "probe"
            subprocess.run([compiler, "-std=c++20", "-O2", "-fno-strict-aliasing",
                            "-ffp-model=strict", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
                            str(path), "-o", str(executable)], check=True, capture_output=True)
            result = subprocess.run([str(executable)], check=True, text=True, capture_output=True)
            self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
