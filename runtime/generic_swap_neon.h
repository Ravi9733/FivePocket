#pragma once
// Attempt v123: swapCopy (generic_renderer_cpu.h) with the source's whole dwords swapped 64 bytes per step by NEON.
// The NDK build compiles swapCopy's per-dword loop to one ldr/rev/str per dword (llvm-objdump of the v119 object: no
// vector instruction in it), and it was the renderer thread's third-largest self symbol on the device (v119 profile:
// 6.2% of recomp-render-w). swapCopyFast writes exactly swapCopy's bytes for every input: the bulk (a multiple of 64
// bytes of whole dwords) is swapped here, the rest (remaining dwords, a straddling partial dword, the zero extension
// to n) by swapCopy itself on the shifted range, which is equivalent because swapCopy's result for a byte depends only
// on its offset within its dword and on whether the dword is whole and below take/n (tests/swap_copy_test.cpp checks
// every endian, take/n combination, alignment and length up to 1 KB against swapCopy, plus random large cases).
// Non-AArch64 builds (host x86) and endian 0 (a plain memcpy in swapCopy) use swapCopy unchanged.
#include "generic_renderer_cpu.h"
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace recomp::gpu::generic::cpu {
inline void swapCopyFast(std::uint8_t* __restrict dst,const std::uint8_t* __restrict src,std::size_t take,std::size_t n,unsigned endian) {
#if defined(__aarch64__)
    const std::size_t t=std::min(take,n);
    const std::size_t bulk=(t&~std::size_t{3})&~std::size_t{63};
    if(bulk && endian>=1 && endian<=3) {
        std::size_t k=0;
        switch(endian) {
        case 1: // 8-in-16: bytes swapped within each 16-bit half
            for(;k<bulk;k+=64){uint8x16x4_t v=vld1q_u8_x4(src+k);
                v.val[0]=vrev16q_u8(v.val[0]);v.val[1]=vrev16q_u8(v.val[1]);v.val[2]=vrev16q_u8(v.val[2]);v.val[3]=vrev16q_u8(v.val[3]);
                vst1q_u8_x4(dst+k,v);}
            break;
        case 2: // 8-in-32: bytes reversed within each dword
            for(;k<bulk;k+=64){uint8x16x4_t v=vld1q_u8_x4(src+k);
                v.val[0]=vrev32q_u8(v.val[0]);v.val[1]=vrev32q_u8(v.val[1]);v.val[2]=vrev32q_u8(v.val[2]);v.val[3]=vrev32q_u8(v.val[3]);
                vst1q_u8_x4(dst+k,v);}
            break;
        default: // 3, 16-in-32: the two 16-bit halves of each dword exchanged
            for(;k<bulk;k+=64){uint8x16x4_t v=vld1q_u8_x4(src+k);
                for(unsigned q=0;q<4;++q)v.val[q]=vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v.val[q])));
                vst1q_u8_x4(dst+k,v);}
            break;
        }
        swapCopy(dst+bulk,src+bulk,t-bulk,n-bulk,endian);
        return;
    }
#endif
    swapCopy(dst,src,take,n,endian);
}
}
