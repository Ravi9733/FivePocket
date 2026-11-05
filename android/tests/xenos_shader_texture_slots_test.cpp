#include "xenos_shader_texture_slots.h"
#include "xenos_copy_shader_fixture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <cstdlib>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
int main(int argc,char** argv) {
    const auto texture=[](unsigned slot){return Words{1|(1u<<19)|(slot<<20)|(1u<<28),0x1F1FF688,0x4000};};
    const auto code=program({alloc(2),exec(1,5)|(0x55u<<16)},
        {texture(5),texture(6),texture(3),texture(4),alu(0)});
    assert(inspectStraightLineTextureSlots(code.data(),code.size())==((1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)));
    // Live diagnostic capture must retain a mixed fetch-all/valid-only set,
    // without changing the old restricted inspection contract.
    auto allThree=texture(3);allThree[0]&=~(1u<<19);
    const auto mixedInvocations=program({alloc(2),exec(1,4)|(0x15u<<16)},
        {allThree,texture(2),texture(11),alu(0)});
    assert(!inspectStraightLineTextureSlots(mixedInvocations.data(),mixedInvocations.size()));
    assert(inspectCapturedPixelTextureSlots(mixedInvocations.data(),mixedInvocations.size())==((1u<<2)|(1u<<3)|(1u<<11)));
    for(unsigned slot=0;slot<32;++slot) {
        const auto code=program({alloc(2),exec(1,2)|(1u<<16)},{texture(slot),alu(0)});
        assert(inspectStraightLineTextureSlots(code.data(),code.size())==(1u<<slot));
        auto all=texture(slot);all[0]&=~(1u<<19);
        const auto fetchAll=program({alloc(2),exec(1,2)|(1u<<16)},{all,alu(0)});
        assert(!inspectStraightLineTextureSlots(fetchAll.data(),fetchAll.size()));
        assert(inspectStraightLineTextureSlots(fetchAll.data(),fetchAll.size(),true)==(1u<<slot));
        for(unsigned bit:{11u,18u,25u}) {
            auto bad=all;bad[0]|=1u<<bit;
            const auto modified=program({alloc(2),exec(1,2)|(1u<<16)},{bad,alu(0)});
            assert(!inspectStraightLineTextureSlots(modified.data(),modified.size(),true));
            assert(!inspectCapturedPixelTextureSlots(modified.data(),modified.size()));
        }
    }
    const auto solid=program({alloc(2),exec(1,1)},{alu(0)});
    assert(inspectStraightLineTextureSlots(solid.data(),solid.size())==0);
    // Offsets are sampling parameters, not resource selectors. Retain every
    // signed XY encoding in the explicit capture policy only.
    for(unsigned x=0;x<32;++x)for(unsigned y=0;y<32;++y) {
        auto offset=texture(15);offset[0]&=~(1u<<19);
        offset[2]|=(x<<16)|(y<<21);
        const auto shader=program({alloc(2),exec(1,2)|(1u<<16)},{offset,alu(0)});
        assert(inspectCapturedPixelTextureSlots(shader.data(),shader.size())==(1u<<15));
        assert(bool(inspectStraightLineTextureSlots(shader.data(),shader.size(),true))==(!x&&!y));
    }
    const Words setLod{24u|(1u<<19)|(2u<<5),0x0F1FFFFF,0};
    auto lodFetch=texture(1);lodFetch[1]=0x2F1FF688;
    const auto explicitCode=program({alloc(2),exec(1,4)|(0x15u<<16)},
        {texture(2),setLod,lodFetch,alu(0)});
    assert(!inspectStraightLineTextureSlots(explicitCode.data(),explicitCode.size(),true));
    assert(inspectCapturedPixelTextureSlots(explicitCode.data(),explicitCode.size())==6);
    const auto noLod=program({alloc(2),exec(1,2)|(1u<<16)},{lodFetch,alu(0)});
    assert(!inspectCapturedPixelTextureSlots(noLod.data(),noLod.size()));
    for(unsigned component=0;component<4;++component) {
        auto replicated=setLod;replicated[0]|=(component*21u)<<26;
        const auto shader=program({alloc(2),exec(1,3)|(5u<<16)},{replicated,lodFetch,alu(0)});
        assert(inspectCapturedPixelTextureSlots(shader.data(),shader.size())==2);
    }
    for(unsigned destination=0;destination<64;++destination) {
        auto unused=setLod;unused[0]|=destination<<12;
        const auto shader=program({alloc(2),exec(1,3)|(5u<<16)},{unused,lodFetch,alu(0)});
        assert(inspectCapturedPixelTextureSlots(shader.data(),shader.size())==2);
    }
    for(unsigned word=0;word<3;++word)for(unsigned bit=0;bit<32;++bit) {
        auto bad=setLod;bad[word]^=1u<<bit;
        const auto shader=program({alloc(2),exec(1,3)|(5u<<16)},{bad,lodFetch,alu(0)});
        // Source and unused destination index bits may change independently.
        assert(bool(inspectCapturedPixelTextureSlots(shader.data(),shader.size()))==
            (word==0 && ((bit>=5 && bit<=10) || (bit>=12 && bit<=17))));
    }
    for(unsigned bit:{0u,1u,2u,15u,26u,31u}) {
        auto bad=texture(15);bad[2]|=1u<<bit;
        const auto shader=program({alloc(2),exec(1,2)|(1u<<16)},{bad,alu(0)});
        assert(!inspectCapturedPixelTextureSlots(shader.data(),shader.size()));
    }
    const Words gradient{18u|(1u<<19)|(1u<<28),0x1F1FF688,0};
    const auto mixed=program({alloc(2),exec(1,3)|(5u<<16)},{texture(7),gradient,alu(0)});
    assert(inspectStraightLineTextureSlots(mixed.data(),mixed.size())==(1u<<7));
    const auto onlyGradient=program({alloc(2),exec(1,2)|(1u<<16)},{gradient,alu(0)});
    assert(inspectStraightLineTextureSlots(onlyGradient.data(),onlyGradient.size())==0);
    for(unsigned word=0;word<3;++word)for(unsigned bit:word==0?std::array<unsigned,3>{11,20,30}:
            word==1?std::array<unsigned,3>{28,29,31}:std::array<unsigned,3>{0,14,15}) {
        auto bad=gradient;bad[word]^=1u<<bit;
        const auto modified=program({alloc(2),exec(1,2)|(5u<<16)},{texture(0),bad});
        assert(!inspectStraightLineTextureSlots(modified.data(),modified.size()));
    }
    assert(!inspectStraightLineTextureSlots(nullptr,code.size()));
    assert(!inspectStraightLineTextureSlots(code.data(),code.size()-1));
    const auto unsupported=program({alloc(2),std::uint64_t{3}<<44},{texture(0)});
    assert(!inspectStraightLineTextureSlots(unsupported.data(),unsupported.size()));
    assert(!inspectCapturedPixelTextureSlots(unsupported.data(),unsupported.size()));
    for(unsigned word=0;word<3;++word) {
        auto bad=texture(0);bad[word]^=word==0?1u:word==1?1u<<28:1u<<15;
        const auto changed=program({alloc(2),exec(1,1)|(1u<<16)},{bad});
        assert(!inspectStraightLineTextureSlots(changed.data(),changed.size()));
        assert(!inspectStraightLineTextureSlots(changed.data(),changed.size(),true));
    }
    std::puts("SHADER_TEXTURE_SLOTS_PASS exact_mask=1 gradient_not_texture=1 unknown_rejected=1 gpu_admission=0");
    std::puts("SHADER_CAPTURE_SAMPLING_VARIANTS_PASS offset_pairs=1024 lod_mutations=96 strict_legacy=1 execution=0");
    if(argc!=1) {
        assert(argc==3);
        std::ifstream input(argv[1],std::ios::binary|std::ios::ate);assert(input);
        const auto size=input.tellg();assert(size>0 && size<=4096 && size%12==0);
        Bytes shader(static_cast<std::size_t>(size));input.seekg(0);
        assert(input.read(reinterpret_cast<char*>(shader.data()),size));
        char* end=nullptr;const auto expected=std::strtoull(argv[2],&end,0);
        assert(end && *end==0 && expected<=UINT32_MAX);
        assert(inspectCapturedPixelTextureSlots(shader.data(),shader.size())==expected);
        // This capture needs the new diagnostic policy, not native admission.
        assert(!inspectStraightLineTextureSlots(shader.data(),shader.size(),true));
        std::printf("SHADER_CAPTURE_CURRENT_PASS bytes=%zu expected_mask=%08x execution=0\n",
            shader.size(),static_cast<unsigned>(expected));
    }
}
