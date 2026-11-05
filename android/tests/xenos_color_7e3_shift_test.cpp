#include "native_ordered_fragment7e3_physical.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
namespace {
constexpr auto policy=Color7e3Rounding::XeniaShiftRound;
// Literal scalar counterpart of Xenia's SPIR-V operations, independent of the
// production helper. This is source-policy agreement, not an Xbox oracle.
unsigned sourceAlgorithm(unsigned bits) {
    const float value=colorFloat(bits);assert(std::isfinite(value));
    if(value<=0)return 0;
    if(value>=31.875f)return 1023;
    const unsigned inserted=(bits&0x7FFFFF)|0x800000;
    const unsigned shifted=inserted>>std::min(125-(bits>>23),24u);
    const unsigned biased=bits<0x3E800000?shifted:bits-0x3E000000;
    return ((biased+32767+((biased>>16)&1))>>16)&1023;
}
}
int main() {
    unsigned samples=0,disagreements=0;
    const auto check=[&](unsigned bits) {
        unsigned out=0xDEADBEEF;
        assert(pack7e3Finite(bits,out,policy) && out==sourceAlgorithm(bits));++samples;
    };
    for(unsigned code=0;code<1024;++code) {
        unsigned decoded=0,out=0;assert(unpack7e3(code,decoded));check(decoded);
        assert(pack7e3Finite(decoded,out,policy) && out==code);
        for(unsigned a=0;a<4;++a) {
            const auto word=code|((1023-code)<<10)|(((code*37)&1023)<<20)|(a<<30);
            assert(packColor7e3(unpackColor7e3(word),out,policy) && out==word);
        }
        if(code==1023)continue;
        unsigned next=0;assert(unpack7e3(code+1,next));
        const auto midpoint=colorBits((colorFloat(decoded)+colorFloat(next))*.5f);
        for(int d=-512;d<=512;++d)check(midpoint+d);
        out=99;
        if(!pack7e3Checked(midpoint+1,out)) {
            assert(out==99 && code<128 && !(code&1));++disagreements;
            assert(pack7e3Finite(midpoint+1,out,policy) && out==code);
        }
    }
    assert(disagreements==64);
    unsigned rng=0x196CC353;
    for(unsigned i=0;i<100000;++i) {
        rng=1664525u*rng+1013904223u;
        if((rng&0x7FFFFFFF)<0x7F800000)check(rng);
    }
    for(auto bits:{0u,1u,0x007FFFFFu,0x00800000u,0x80000000u,0xBF800000u,0xFF7FFFFFu,0x41FF0000u,0x7F7FFFFFu})check(bits);
    for(auto bits:{0x7F800000u,0xFF800000u,0x7FC00000u,0xFFC00000u}) {
        unsigned out=71;assert(!pack7e3Finite(bits,out,policy) && out==71);
        for(unsigned lane=0;lane<4;++lane) {
            std::array<unsigned,4> rgba{};rgba[lane]=bits;
            assert(!packColor7e3(rgba,out,policy) && out==71);
        }
    }
    unsigned out=71;const auto bad=static_cast<Color7e3Rounding>(99);
    assert(!pack7e3Finite(0,out,bad) && out==71);
    // Minimized witness from the captured failure: one float ULP above the
    // 104/105 midpoint. Old policy refuses; Xenia shifts away that sticky bit.
    assert(!pack7e3Checked(0x3E510001,out) && out==71);
    assert(pack7e3Finite(0x3E510001,out,policy) && out==104);
    NativeFragment7e3Depth sample{{colorFloat(0x3E510001),.5f,1.f,1.f},.5f};
    assert(!mergeNativeFragment7e3Depth(sample,0,0));
    const auto merged=mergeNativeFragment7e3Depth(sample,0,0xA5,policy);
    assert(merged && merged->color==(104u|(256u<<10)|(384u<<20)|(3u<<30)) &&
        merged->depthStencil==((*truncateXenosFloat24Depth(.5f)<<8)|0xA5));
    assert(!mergeNativeFragment7e3Depth(sample,0,0,bad));
    constexpr unsigned pixels=1280*720;
    const auto seed=[] {
        NativeEdram32Bands owner;
        assert(owner.recordCompletedRawColorRows(720,1280,1280,720,std::vector<std::uint8_t>(pixels*4),1));
        assert(owner.recordCompletedDepthWords(0,1280,1280,720,std::vector<unsigned>(pixels,0xA5),2));return owner;
    };
    for(unsigned mode=0;mode<4;++mode) {
        auto owner=seed();const char* why=nullptr;
        NativeEdram32Mutation mutation(owner);
        const bool ok=executeNativeOrderedFragment7e3Physical(mutation,[&] {
            NativeOrderedFragment7e3Batch batch{2,{{0,19,sample},{1,20,sample}}};
            if(mode==2)batch.fragments.back().sample.color[0]=std::numeric_limits<float>::infinity();
            return std::optional{batch};
        },&why,mode==0?Color7e3Rounding::RequireNearestEvenAgreement:mode==3?bad:policy);
        assert(ok==(mode==1));
        if(ok) {
            const auto color=owner.readColorWords(720,1280,1280,720),depth=owner.readDepthWords(0,1280,1280,720);
            assert(color && depth);
            for(unsigned p=0;p<pixels;++p) {
                assert(color->at(p)==(p==19 || p==20?merged->color:0));
                assert(depth->at(p)==(p==19 || p==20?merged->depthStencil:0xA5));
            }
        }else assert(why && !owner.knownTiles());
    }
    std::printf("COLOR_7E3_XENIA_SHIFT_PASS samples=%u roundtrips=4096 retained_disagreements=64 witness=104 paired_owner=1 late_invalid_atomic=1 xbox_conformance=0\n",samples);
}
