#include "indexed_lod_fixture.h"
#include "native_indexed_lod_draw.h"
#include "process_limits.h"
#include <cstdio>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
int main(int argc,char** argv) {
    assert(argc==2 && initializeProbeProcess());std::setvbuf(stdout,nullptr,_IONBF,0);
    IndexedLodFixture f(argv[1]);NativeIndexedLodProgram program{f.positions,{f.vertexCode,f.pixelCode},33};
    ProbeDevice gpu;ShaderRequirements requirements;requirements.fragmentStoresAndAtomics=true;
    assert(gpu.inspect() && gpu.create(requirements));
    constexpr unsigned pixels=1280*720;unsigned seed=0;
    assert(packColor7e3({colorBits(.25f),colorBits(.5f),colorBits(1),colorBits(1)},seed));
    std::vector<unsigned> depth(pixels);std::vector<std::uint8_t> colors(pixels*4);
    for(unsigned p=0;p<pixels;++p){depth[p]=p&255;for(unsigned c=0;c<4;++c)colors[p*4+c]=seed>>(8*c);}
    const auto ownerSeed=[&] {NativeEdram32Bands owner;
        assert(owner.recordCompletedRawColorRows(720,1280,1280,720,colors,1));
        assert(owner.recordCompletedDepthWords(0,1280,1280,720,depth,2));return owner;};
    auto owner=ownerSeed();const char* why=nullptr;
    {NativeEdram32Mutation mutation(owner);
        if(!executeNativeIndexedLodDraw(gpu,program,f.draw,f.indices,f.bound,f.textures,
            f.positions.vertexIdentity,f.positions.pixelIdentity,true,mutation,&why)) {
            std::printf("INDEXED_LOD_GPU_REFUSED stage=%s\n",why?why:"unknown");return 2;}}
    const auto c=owner.readColorWords(720,1280,1280,720),d=owner.readDepthWords(0,1280,1280,720);assert(c && d);
    std::uint64_t hash=14695981039346656037ULL;unsigned changed=0;
    for(unsigned p=0;p<pixels;++p){changed+=d->at(p)!=depth[p];assert((d->at(p)&255)==(p&255));
        for(auto word:{c->at(p),d->at(p)}){hash^=word;hash*=1099511628211ULL;}}
    // Immutable v302 original shader+raster+physical component control, using
    // these exact captured inputs and authored seed. Not a retail image oracle.
    assert(hash==0x0A29C7BE5BF7E7FFULL && changed==4576);
    for(unsigned mode=0;mode<4;++mode) {
        owner=ownerSeed();auto bad=program;auto vertex=f.positions.vertexIdentity;
        if(mode==0)bad.abi=32;if(mode==1)vertex[0]^=1;
        if(mode==2)assert(owner.invalidate(0,1));
        {NativeEdram32Mutation mutation(owner);assert(!executeNativeIndexedLodDraw(gpu,bad,f.draw,f.indices,f.bound,f.textures,
            vertex,f.positions.pixelIdentity,mode!=3,mutation,&why));}
        assert(!owner.knownTiles());
    }
    std::puts("INDEXED_LOD_GPU_PASS production_executor=1 current_positions=1 textures=5 changed_pixels=4576 paired_hash=0a29c7be5bf7e7ff negatives=4 authored_owner=1 live_draw=0");
}
