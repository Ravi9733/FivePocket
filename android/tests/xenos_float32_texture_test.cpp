// k_32_FLOAT (format 36) raw storage view. The primary case is the ACTUAL
// destination the device recorded for the stopped ABI22 resolve, taken verbatim
// from the six fetch words at byte 28 of its 52-byte header in
// local-analysis/app-game-cache-replay-qohsqof6/renderer-first-unsupported/
// native-resolve-failure-destination-header.bin: 640x360, tiled, pitch 640,
// base 0xa67fd000, endian 2, integerComponents set, sign 1 in all four fields,
// swizzle {0,5,5,5}.
//
// These cases pin that the view is byte addressing only: it must not widen the
// colour import path, and a descriptor that would require a real conversion is
// refused rather than approximated.
#include "xenos_float32_texture.h"
#include <cassert>
#include <cstdio>
#include <string_view>

using namespace recomp::gpu;

namespace {
constexpr std::array<std::uint32_t,6> Captured{0x85000156u,0xa67fd0a4u,0x2ce27fu,0x16d1u,0x0u,0x200u};
unsigned passed=0;
void expect(bool condition,const char* what) {
    if(!condition){std::fprintf(stderr,"FLOAT32_STORAGE_FAIL %s\n",what);std::abort();}
    ++passed;
}
XenosTextureDescription describe(std::array<std::uint32_t,6> words) {
    const auto d=XenosTextureDescription::decode(words);
    expect(d.has_value(),"fetch words decode");
    return *d;
}
}

int main() {
    const auto captured=describe(Captured);
    expect(captured.format==36,"captured destination is k_32_FLOAT");
    expect(captured.width==640 && captured.height==360,"captured extent");
    expect(captured.pitchTexels==640 && captured.tiled,"captured pitch and tiling");
    expect(captured.endian==2,"captured endian");
    expect(captured.integerComponents,"captured integer-components select");

    // The colour import path must still refuse it; this view does not widen it.
    expect(!planXenosTexture2D(captured),"colour import still refuses format 36");

    const auto plan=planXenosFloat32Storage(captured);
    expect(plan.has_value(),"raw float32 storage view admitted");
    expect(plan->bytesPerTexel==4,"four bytes per texel");
    expect(plan->linearBytes==640u*360u*4u,"linear bytes are the exact extent");
    expect(plan->description.format==6,"addressing view presents format 6");
    expect(plan->description.endian==2,"endian permutation is preserved");
    expect(plan->description.baseAddress==captured.baseAddress,"base address preserved");
    expect(plan->description.width==640 && plan->description.height==360,"extent preserved");
    expect(plan->description.pitchTexels==640,"pitch preserved");
    expect(plan->description.tiled,"tiling preserved");
    for(auto sign:plan->description.sign)expect(sign==0,"sign fields cleared for the raw view");
    expect(!plan->description.integerComponents,"integer-components cleared for the raw view");
    // Tiled storage rounds height to the 32-row tile, so it exceeds the linear
    // extent; the store path writes only real texels and preserves the padding.
    expect(plan->sourceBytes>plan->linearBytes,"tiled storage includes tile padding");

    // A store into that view must round-trip through the existing format 6
    // machinery, which is the entire reason for presenting the raw view.
    {
        std::vector<std::uint8_t> linear(plan->linearBytes);
        for(std::size_t i=0;i<linear.size();++i)linear[i]=static_cast<std::uint8_t>(i*31u+7u);
        const std::vector<std::uint8_t> previous(plan->sourceBytes,0x5A);
        const auto stored=storeXenosTexture2D(plan->description,linear,previous);
        expect(stored.has_value(),"store accepts the raw view");
        expect(stored->size()==plan->sourceBytes,"stored size is the storage extent");
        const auto back=linearizeXenosTexture2D(plan->description,*stored);
        expect(back.has_value(),"linearize accepts the raw view");
        expect(*back==linear,"tiling and endian round-trip byte for byte");
    }

    // Anything that would require a real conversion is refused, not guessed.
    {
        auto words=Captured;words[1]=(words[1]&~63u)|30u; // k_16_FLOAT
        expect(!planXenosFloat32Storage(describe(words)),"other formats refused");
    }
    {
        auto words=Captured;words[4]=1u<<2; // mipMin
        expect(!planXenosFloat32Storage(describe(words)),"mip levels refused");
    }
    {
        auto words=Captured;words[3]|=(3u<<13); // exponent bias
        expect(!planXenosFloat32Storage(describe(words)),"exponent bias refused");
    }
    {
        auto words=Captured;words[0]|=(3u<<2); // sign field 0 -> 3
        expect(!planXenosFloat32Storage(describe(words)),"unknown sign selection refused");
    }

    // The ABI24 tonemap pass SAMPLES this buffer rather than resolving into it.
    // The constant is the guest's own, read out of device state at 1152+24*4 in
    // local-analysis/app-game-cache-replay-2xzhqay8: the same 640x360 k_32_FLOAT
    // texture the ABI22 colour1 resolve writes, through the guest's physical
    // alias instead of the virtual one, with POINT filtering.
    {
        constexpr std::array<std::uint32_t,6> Sampled{0x85004956u,0x067fd0a4u,0x2ce27fu,0x16d1u,0x0u,0x210u};
        const auto described=describe(Sampled);
        const auto decoded=XenosTextureDescription::decode(Sampled);
        expect(decoded.has_value(),"sampled constant decodes");
        expect(decoded->format==36 && decoded->width==640 && decoded->height==360,
            "sampled constant is the 640x360 k_32_FLOAT buffer");
        expect(((Sampled[3]>>19)&63)==0,"sampled with all-POINT filtering");
        // The colour import must keep refusing it; only the raw view admits it.
        expect(!planXenosTexture2D(*decoded),"colour import still refuses k_32_FLOAT");
        const auto raw=planXenosFloatStorage(described);
        expect(raw.has_value(),"raw storage view admits the sampled constant");
        expect(raw->bytesPerTexel==4,"one binary32 per texel");
        expect(raw->linearBytes==640u*360u*4u,"linear extent");
        expect(raw->description.format==6,"addressed through the format 6 view");
    }

    // The tonemap pass reads back the k_16_16 texture its own earlier pair
    // resolved into. The constant is the guest's own, from device state at
    // 1152+24*2 in local-analysis/app-game-cache-replay-sftd8254.
    {
        constexpr std::array<std::uint32_t,6> Fixed{0x85004802u,0x065d2099u,0x2ce27fu,0x281690u,0x3u,0x210u};
        const auto decoded=XenosTextureDescription::decode(Fixed);
        expect(decoded.has_value(),"k_16_16 constant decodes");
        expect(decoded->format==25,"the resolved destination format");
        expect(decoded->sign==std::array<unsigned,4>{0,0,0,0},"read unsigned, as the resolve wrote it");
        expect(((Fixed[3]>>19)&63)==5,"mag/min LINEAR with mip POINT");
        expect(!planXenosTexture2D(*decoded),"colour import still refuses k_16_16");
        const auto raw=planXenosFloatStorage(describe(Fixed));
        expect(raw.has_value(),"raw storage view admits it");
        expect(raw->bytesPerTexel==4,"two 16-bit channels");
        expect(raw->description.format==6,"addressed through the format 6 view");
        expect(xenosRawFloatStorageTexelBytes(25)==4,"element size reported");
    }

    std::printf("XENOS_FLOAT32_STORAGE_PASS captured_destination=1 sampled_source=1 fixed16_source=1 raw_view_only=1 "
        "colour_import_unchanged=1 cases=%u gpu=0 gameplay=0\n",passed);
    return 0;
}
