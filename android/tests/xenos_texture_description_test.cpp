#include "xenos_texture_description.h"
#include <cassert>

using recomp::gpu::XenosTextureDescription;
using recomp::gpu::describeGta5TextureHeader;

int main() {
    // Observed backbuffer layout, synthetic address (not game bytes).
    std::array<std::uint32_t,6> words{0x8a000002,0x00010006,0x0059e4ff,0x1414,0,0x200};
    auto d=XenosTextureDescription::decode(words);
    assert(d && d->width==1280 && d->height==720 && d->depthOrLayers==1);
    assert(d->tiled && d->pitchTexels==1280 && d->format==6 && d->endian==0);
    assert(d->baseAddress==0x10000 && !d->mipAddress && !d->mipMax);
    const std::array<unsigned,4> bgr1{2,1,0,5};
    assert(d->swizzle==bgr1);
    std::array<std::uint8_t,52> header{};
    for (unsigned i=0;i<6;++i) for (unsigned b=0;b<4;++b)
        header[28+4*i+b]=static_cast<std::uint8_t>(words[i]>>(24-8*b));
    assert(describeGta5TextureHeader(header)->fetch==words);
    assert(!describeGta5TextureHeader(header.data(),51));

    // Dimension and signed-field decoding must not be tied to the first frame.
    words[2]=(3u<<22)|(15u<<11)|31u;
    words[3]=63u<<13;
    words[4]=(2u<<2)|(4u<<6);
    words[5]=0x00020800|(2u<<9);
    d=XenosTextureDescription::decode(words);
    assert(d && d->width==32 && d->height==16 && d->depthOrLayers==4);
    assert(d->exponentBias==-1 && d->mipMin==2 && d->mipMax==4);
    assert(d->packedMips && d->mipAddress==0x20000);
    words[1]|=1024u;
    words[2]=(4u<<26)|(31u<<13)|63u;
    words[5]=1u<<9;
    d=XenosTextureDescription::decode(words);
    assert(d && d->width==64 && d->height==32 && d->depthOrLayers==5);
    words[5]=3u<<9;
    assert(XenosTextureDescription::decode(words)->depthOrLayers==6);
    words[5]=0; words[2]=255;
    d=XenosTextureDescription::decode(words);
    assert(d && d->width==256 && d->height==1 && d->depthOrLayers==1);
    words[0]=0;
    assert(!XenosTextureDescription::decode(words));
}
