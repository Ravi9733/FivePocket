#include "indexed_blend_depth_fixture.h"
#include "native_indexed_blend_depth_draw.h"
#include "process_limits.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
int main(int argc,char** argv) {
    assert(argc==2 && initializeProbeProcess());std::setvbuf(stdout,nullptr,_IONBF,0);
    const std::string folder=argv[1];IndexedBlendDepthFixture f(folder);
    ProbeDevice device;assert(device.inspect() && device.create({}));
    NativeIndexedBlendDepthProgram program{f.program,
        {f.program.vertexIdentity,f.program.pixelIdentity,indexedBlendCode(folder+"/position.comp.spv"),28},
        indexedBlendCode(folder+"/authored-count.frag.spv")};
    constexpr unsigned pixels=1280*720;unsigned seed=0;
    assert(packColor7e3({colorBits(.125f),colorBits(.25f),colorBits(.5f),colorBits(1.f)},seed));
    std::vector<std::uint8_t> colors(pixels*4);std::vector<unsigned> depth(pixels,7);
    for(unsigned i=0;i<pixels;++i)for(unsigned c=0;c<4;++c)colors[4*i+c]=seed>>(8*c);
    const auto refs=indexedBlendRead(folder+"/ordered-references.bin");assert(indexedBlendBe(refs,0)==12 && refs.size()==196);
    unsigned maxRgbError=0,changed=0;
    for(unsigned mode=0;mode<8;++mode) {
        // A different actual depth/stencil owner must change coverage, not
        // inherit the all-zero depth used in the earlier isolated test.
        if(mode==1)std::fill(depth.begin(),depth.end(),(*encodeExactXenosFloat24(.5f)<<8)|7);
        if(mode==2)std::fill(depth.begin(),depth.end(),0);
        if(mode>=3)std::fill(depth.begin(),depth.end(),7);
        if(mode>=6)std::fill(depth.begin(),depth.end(),((mode==6?1u:512u)<<8)|7);
        NativeEdram32Bands owner;
        assert(owner.recordCompletedRawColorRows(720,1280,1280,720,colors,1));
        assert(owner.recordCompletedDepthWords(0,1280,1280,720,depth,2));
        assert(owner.recordCompletedDepthWords(1500,80,80,16,std::vector<unsigned>(1280,0x12345678),3));
        auto indices=f.indices;if(mode==3)indices.insert(indices.end(),f.indices.begin(),f.indices.end());
        auto draw=f.draw;
        // Original PS's final alpha multiplies c62.x. Counter-coverage is
        // unchanged; only actual source-PS output can detect this violation.
        if(mode==4 || mode==5)draw.pixelConstants[4*62]=colorBits(mode==4?2.f:-1.f);
        const char* why=nullptr;bool ok=false;
        {
            NativeEdram32Mutation mutation(owner);
            ok=executeNativeIndexedBlendDepthDraw(device,program,draw,indices,f.bound,f.textures,
                f.program.vertexIdentity,f.program.pixelIdentity,true,mutation,&why);
        }
        if(mode==3) {
            assert(!ok && why && std::strcmp(why,"single-coverage7e3-overlap-or-invalid-count")==0 && !owner.knownTiles());
            std::puts("INDEXED_BLEND_DEPTH_OVERLAP_GPU_PASS duplicated_mesh_refused=1 failed_owner_invalidated=1");
            continue;
        }
        if(mode==4 || mode==5) {
            assert(!ok && why && std::strcmp(why,"single-coverage7e3-source-alpha-range")==0 && !owner.knownTiles());
            std::printf("INDEXED_BLEND_DEPTH_SOURCE_ALPHA_GPU_PASS mode=%u original_constant_mutation=1 out_of_range_refused=1 failed_owner_invalidated=1\n",mode);
            continue;
        }
        if(!ok){std::printf("INDEXED_BLEND_DEPTH_GPU_REFUSED mode=%u stage=%s\n",mode,why);return 2;}
        const auto packed=owner.readColorWords(720,1280,1280,720);assert(packed && packed->size()==pixels);
        assert(owner.readDepthWords(0,1280,1280,720)==depth);
        assert(owner.readDepthWords(1500,80,80,16)==std::optional{std::vector<unsigned>(1280,0x12345678)});
        if(mode)assert(std::all_of(packed->begin(),packed->end(),[&](auto v){return v==seed;}));
        else {
            for(auto v:*packed)changed+=v!=seed;
            assert(changed>0);
            for(unsigned i=0;i<12;++i) {
                const auto at=4+16*i,x=indexedBlendBe(refs,at),y=indexedBlendBe(refs,at+4),want=indexedBlendBe(refs,at+8),got=packed->at(y*1280+x);
                for(unsigned c=0;c<4;++c) {
                    const unsigned shift=c==3?30:10*c,mask=c==3?3:1023;
                    const auto error=unsigned(std::abs(int((got>>shift)&mask)-int((want>>shift)&mask)));
                    if(c<3)maxRgbError=std::max(maxRgbError,error);
                    if(error>(c==3?0u:1u)) {
                        std::printf("INDEXED_BLEND_DEPTH_GPU_PIXEL_MISMATCH reference=%u channel=%u error=%u\n",i,c,error);return 3;
                    }
                }
            }
        }
    }
    std::printf("INDEXED_BLEND_DEPTH_PRODUCTION_GPU_PASS triangles=1120 references=12 max_rgb_code_error=%u alpha_exact=1 changed_pixels=%u depth_stencil_controls=4 overlap_refused=1 live_admission=0\n",maxRgbError,changed);
}
