#include "native_cube_base_capture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    unsigned cases=0;
    for(unsigned width:{32u,64u,128u,256u,512u})for(unsigned endian=0;endian<4;++endian) {
        const std::array<std::uint32_t,6> fetch{2u|((width/32)<<22)|0x800000FCu,
            0x100000u|6u|(endian<<6),(width-1)|((width-1)<<13)|(5u<<26),0xC14,0,0x600};
        const unsigned faceBytes=width*width*4;unsigned reads=0,failAt=99,shortAt=99;
        const Gta5ResourceReader reader=[&](unsigned address,unsigned size)->std::optional<std::vector<std::uint8_t>> {
            const unsigned face=reads++;assert(face<6 && size==faceBytes && address==0x100000+face*faceBytes);
            if(face==failAt)return std::nullopt;
            return std::vector<std::uint8_t>(size-(face==shortAt),face+1);
        };
        auto full=captureNativeCubeBase(4,fetch,reader);assert(full && full->fetch==fetch && full->slot==4 && !full->literalZero);
        assert(reads==6 && full->source.size()==6*faceBytes);
        for(unsigned i=0;i<full->source.size();++i)assert(full->source[i]==i/faceBytes+1);
        for(unsigned face=0;face<6;++face) {
            reads=0;failAt=face;assert(!captureNativeCubeBase(4,fetch,reader) && reads==face+1);failAt=99;
            reads=0;shortAt=face;assert(!captureNativeCubeBase(4,fetch,reader) && reads==face+1);shortAt=99;
        }
        for(const auto mutation:std::array<std::array<unsigned,2>,5>{{{5,0x400},{5,0x800},{4,4},{1,1024},{2,1}}}) {
            auto bad=fetch;bad[mutation[0]]^=mutation[1];reads=0;
            assert(!captureNativeCubeBase(4,bad,reader) && !reads);
        }
        auto wrap=fetch;wrap[1]=(wrap[1]&0xFFF)|0xFFFFF000;reads=0;
        assert(!captureNativeCubeBase(4,wrap,reader) && !reads);
        assert(!captureNativeCubeBase(32,fetch,reader) && !captureNativeCubeBase(4,fetch,{}));++cases;
    }
    std::printf("NATIVE_CUBE_BASE_CAPTURE_PASS layouts=%u all_six_owned=1 partial_reads_rejected=1 invalid_layouts_rejected_before_read=1 sampling_unimplemented=1\n",cases);
}
