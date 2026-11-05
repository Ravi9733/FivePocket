#include "xenos_texture_mip_chain.h"
#include <cassert>

int main() {
    using namespace recomp::gpu;
    // Synthetic128x128 BC3, tiled, three levels. Packed flag applies only at
    // level3 (16x16), which this descriptor never requests.
    std::array<std::uint32_t,6> words{0x82000002,0x10000014,127|(127u<<13),0,2u<<6,0x20000A00};
    const auto input=words;
    const auto plan=planXenosUnpackedMipChain(words);
    assert(plan && plan->levels.size()==3 && plan->submittedFetch==words && words==input);
    assert(plan->baseBytes==16384 && plan->mipBytes==32768);
    for(unsigned i=0;i<3;++i) {
        const auto& level=plan->levels[i];const auto& d=level.layout.description;
        assert(level.level==i && d.width==(128u>>i) && d.height==(128u>>i));
        assert(d.pitchTexels==256u/(i?2:1)); // Base deliberately larger than width.
        assert(d.baseAddress==(i?0x20000000+(i-1)*16384:0x10000000));
        assert(!d.mipMax && !d.mipAddress && !d.packedMips);
        assert(level.layout.sourceBytes==16384);
    }
    auto wrong=words;wrong[4]=3u<<6;
    assert(!planXenosUnpackedMipChain(wrong)); // Packed tail unsupported, not flattened.
    wrong=words;wrong[4]|=1u<<2;assert(!planXenosUnpackedMipChain(wrong));
    wrong=words;wrong[5]&=0xFFF;assert(!planXenosUnpackedMipChain(wrong));
    wrong=words;wrong[1]|=1u<<10;assert(!planXenosUnpackedMipChain(wrong));
    wrong=words;wrong[0]|=4;assert(!planXenosUnpackedMipChain(wrong));
    wrong=words;wrong[5]=0xFFFFEA00;assert(!planXenosUnpackedMipChain(wrong));
    wrong=words;wrong[1]=0;assert(!planXenosUnpackedMipChain(wrong));
    wrong=words;wrong[5]&=~0x800u;wrong[4]=8u<<6;assert(!planXenosUnpackedMipChain(wrong));
    // Non-power-of-two dimensions affect logical extents, not mip strides.
    words={0x82800002,0x10000014,299|(199u<<13),0,2u<<6,0x20000200};
    const auto npot=planXenosUnpackedMipChain(words);assert(npot && npot->levels.size()==3);
    assert(npot->levels[1].layout.description.width==150 && npot->levels[1].layout.description.height==100);
    assert(npot->levels[1].layout.description.pitchTexels==256 && npot->levels[1].storageStride==32768);
    assert(npot->levels[2].planeOffset==32768 && npot->levels[2].layout.description.pitchTexels==128);
    // Base depth-slice rows use the real height; only later mips round the
    // base dimensions to powers of two before shifting.
    words={0x81000002,0x10000014,127|(259u<<13),0,2u<<6,0x20000200};
    const auto tall=planXenosUnpackedMipChain(words);assert(tall);
    assert(tall->levels[0].storageStride==49152);
    assert(tall->levels[1].storageStride==32768 && tall->levels[2].planeOffset==32768);
    // Linear R8 mip rows require256-byte pitch even for tiny logical widths.
    words={4u<<22|2,0x10000002,127|(127u<<13),0,2u<<6,0x20000200};
    const auto linear=planXenosUnpackedMipChain(words);assert(linear);
    assert(linear->levels[0].layout.description.pitchTexels==128);
    assert(linear->levels[1].layout.description.pitchTexels==256);
    assert(linear->levels[1].storageStride==16384 && linear->levels[2].planeOffset==16384);
    assert(linear->levels[1].layout.sourceBytes==63*256+64);
    // No mip request is still an independently described base view.
    words[4]=0;words[5]=0x200;assert(planXenosUnpackedMipChain(words)->levels.size()==1);
    // Distinct authored BC3 colors at each level. Every level occupies a
    // complete independent16KiB tile here, so no padding is a missing plane.
    words={0x81000002,0x10000014,127|(127u<<13),0x688,2u<<6,0x20000A00};
    const auto bc3=planXenosUnpackedMipChain(words);assert(bc3);
    std::vector<std::uint8_t> base(bc3->baseBytes),mips(bc3->mipBytes);
    constexpr unsigned endpoints[]{0xF800,0x07E0,0x001F};
    for(const auto& level:bc3->levels) {
        auto& plane=level.level?mips:base;
        for(unsigned at=level.planeOffset;at<level.planeOffset+level.layout.sourceBytes;at+=16) {
            plane[at]=255;plane[at+8]=endpoints[level.level]&255;
            plane[at+9]=endpoints[level.level]>>8;
        }
    }
    const auto unchangedBase=base,unchangedMips=mips;
    const auto decoded=linearizeXenosUnpackedMipChain(words,base,mips);assert(decoded && decoded->size()==3);
    for(unsigned level=0;level<3;++level)for(unsigned i=0;i<(*decoded)[level].size();++i)
        assert((*decoded)[level][i]==((i%4==level || i%4==3)?255:0));
    assert(base==unchangedBase && mips==unchangedMips);
    auto shortMip=mips;shortMip.pop_back();assert(!linearizeXenosUnpackedMipChain(words,base,shortMip));
    auto shortBase=base;shortBase.pop_back();assert(!linearizeXenosUnpackedMipChain(words,shortBase,mips));
    assert(!linearizeXenosUnpackedMipChain(words,base,{}));
    wrong=words;wrong[4]=3u<<6;assert(!linearizeXenosUnpackedMipChain(wrong,base,mips));
}
