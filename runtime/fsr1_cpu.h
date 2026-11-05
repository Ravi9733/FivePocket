#pragma once
// agent fsr1 (v450): the CPU side of AMD FidelityFX Super Resolution 1 for the presenter's FSR scaler (direct_present.h,
// diag-v450-scale-fsr1): AMD's own constant setup, FsrEasuCon and FsrRcasCon, compiled from ffx_a.h + ffx_fsr1.h with A_CPU
// (unmodified copies of ROOT/third_party/fsr1, placed next to this file; sha256 in third_party/fsr1/README.md, whose table lists
// the two headers' hashes on swapped rows: ffx_a.h f60e2722..., ffx_fsr1.h 93c39223...). The headers are included inside
// namespace recomp::gpu::direct::ffx (they include nothing themselves) with A_STATIC = inline (the headers' own override hook:
// one definition across the library's translation units), and every macro they define is undefined again below (the list is
// the preprocessor diff, tools/fsr1/macro_audit.sh; tools/fsr1/fsr1_test.cpp checks none survives).
//
// ffx_a.h / ffx_fsr1.h: Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved. MIT licence (Notice below; the
// same text as third_party/fsr1/license.txt). The library carries the notice as fsr1::Notice; the app's licence files must
// carry it too (see the fsr1 agent report).
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace recomp::gpu::direct::ffx {
#pragma float_control(push)
#pragma clang fp contract(off) // (AMD's constant setup unfused whatever the TU's -ffp-contract: the host tests' bits)
#define A_CPU 1
#define A_STATIC inline
#include "ffx_a.h"
#include "ffx_fsr1.h"
#pragma float_control(pop)
}
#undef AD1_
#undef AF1_
#undef AL1_
#undef ASL1_
#undef ASU1_
#undef AU1_
#undef A_2PI
#undef A_CPU
#undef A_FALSE
#undef A_RESTRICT
#undef A_STATIC
#undef A_TRUE
#undef FSR_RCAS_LIMIT
#undef inAD2
#undef inAD3
#undef inAD4
#undef inAF2
#undef inAF3
#undef inAF4
#undef inAL2
#undef inAL3
#undef inAL4
#undef inAU2
#undef inAU3
#undef inAU4
#undef initAD2
#undef initAD3
#undef initAD4
#undef initAF2
#undef initAF3
#undef initAF4
#undef initAL2
#undef initAL3
#undef initAL4
#undef initAU2
#undef initAU3
#undef initAU4
#undef inoutAD2
#undef inoutAD3
#undef inoutAD4
#undef inoutAF2
#undef inoutAF3
#undef inoutAF4
#undef inoutAL2
#undef inoutAL3
#undef inoutAL4
#undef inoutAU2
#undef inoutAU3
#undef inoutAU4
#undef outAD2
#undef outAD3
#undef outAD4
#undef outAF2
#undef outAF3
#undef outAF4
#undef outAL2
#undef outAL3
#undef outAL4
#undef outAU2
#undef outAU3
#undef outAU4
#undef retAD2
#undef retAD3
#undef retAD4
#undef retAF2
#undef retAF3
#undef retAF4
#undef retAL2
#undef retAL3
#undef retAL4
#undef retAU2
#undef retAU3
#undef retAU4
#undef varAD2
#undef varAD3
#undef varAD4
#undef varAF2
#undef varAF3
#undef varAF4
#undef varAL2
#undef varAL3
#undef varAL4
#undef varAU2
#undef varAU3
#undef varAU4

namespace recomp::gpu::direct::fsr1 {
// The shaders' push constants (direct_present_fsr_easu.frag / _rcas.frag: ivec4 rect, then uvec4 con0..con3): EASU's four
// words from FsrEasuCon for the 1280x720 source scaled to the image rect, RCAS's from FsrRcasCon(stops).
struct Constants {std::uint32_t easu[16];std::uint32_t rcas[4];float stops;std::uint32_t width,height;
                  std::uint32_t srcWidth,srcHeight;}; // (lane fsr-stage-b: the picture EASU reads; 1280x720 unless stage B hands over the render size)
// lane fsr-stage-b: srcWidth x srcHeight = the size of the picture EASU reads (AMD's "input viewport" = "input size": the
// whole map picture is the viewport). The defaults are v463's literals: float(1280u) and 1280.0f are the same value.
inline Constants constants(std::uint32_t imageWidth,std::uint32_t imageHeight,float stops,std::uint32_t srcWidth=1280,
                           std::uint32_t srcHeight=720) noexcept {
    Constants c{};
    ffx::FsrEasuCon(c.easu,c.easu+4,c.easu+8,c.easu+12,float(srcWidth),float(srcHeight),float(srcWidth),float(srcHeight),float(imageWidth),float(imageHeight));
    ffx::FsrRcasCon(c.rcas,stops);
    c.stops=stops;c.width=imageWidth;c.height=imageHeight;c.srcWidth=srcWidth;c.srcHeight=srcHeight;
    return c;
}
// RCAS sharpness in stops (0 = AMD's maximum; each stop halves it). 0.2 is the default of AMD's FSR 1 sample as recalled (not
// verifiable offline: the vendored headers state no default); files/private-trace/diag-v450-fsr1-stops (its text, 0..4) overrides.
constexpr float DefaultStops=0.2f;
// AMD's MIT notice (third_party/fsr1/license.txt) and ffx_a.h's second MIT notice (Michal Drobot), carried in the library.
inline constexpr char Notice[]=
    "AMD FidelityFX Super Resolution 1 (ffx_a.h, ffx_fsr1.h)\n"
    "Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.\n"
    "Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation "
    "files (the \"Software\"), to deal in the Software without restriction, including without limitation the rights to use, copy, "
    "modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software "
    "is furnished to do so, subject to the following conditions:\n"
    "The above copyright notice and this permission notice shall be included in all copies or substantial portions of the "
    "Software.\n"
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE "
    "WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR "
    "COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, "
    "ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\n"
    // ffx_a.h's second notice: its float approximations (APrxLoRcpF1, APrxLoRsqF1, APrxMedRcpF1: used by EASU and RCAS)
    "Copyright (c) 2014 Michal Drobot (for concepts used in \"FLOAT APPROXIMATIONS\").\n"
    "Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation "
    "files (the \"Software\"), to deal in the Software without restriction, including without limitation the rights to use, copy, "
    "modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software "
    "is furnished to do so, subject to the following conditions:\n"
    "The above copyright notice and this permission notice shall be included in all copies or substantial portions of the "
    "Software.\n"
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE "
    "WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR "
    "COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, "
    "ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\n";
}
