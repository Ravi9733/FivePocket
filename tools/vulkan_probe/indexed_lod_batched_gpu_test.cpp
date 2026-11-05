#include "indexed_lod_fixture.h"
#include "native_indexed_lod_draw.h"
#include "process_limits.h"
#include <cstdio>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
int main(int argc,char** argv) {
    assert((argc==2 || (argc==3 && std::strcmp(argv[2],"--windows")==0)) && initializeProbeProcess());std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto profile=NativeIndexedLodProfile::Indexed601;
    IndexedLodFixture f(argv[1],profile);assert(f.indices.size()==36 && f.draw.vertexCount==24);
    NativeIndexedLodProgram program{f.positions,{f.vertexCode,f.pixelCode},33,profile};
    if(argc==3)program.raster=NativeIndexedLodRaster::ClippedWindows;
    ProbeDevice gpu;ShaderRequirements requirements;requirements.fragmentStoresAndAtomics=true;
    assert(gpu.inspect() && gpu.create(requirements));const char* why=nullptr;
    const auto plan=planNativeIndexedLod(f.draw,f.indices,f.bound,f.textures,true,&why,profile);assert(plan);
    auto executor=NativeVertexClipExecutor::create(gpu,f.positions);assert(executor);
    const auto positions=executor->indexedPositions(f.draw);assert(positions && positions->size()==24);
    const auto expected=IndexedLodFixture::read(std::string(argv[1])+"/positions.bin");assert(expected.size()==384);
    for(unsigned i=0;i<24;++i)for(unsigned c=0;c<4;++c)
        assert(colorBits(positions->at(i)[c])==IndexedLodFixture::be(expected,i*16+c*4));
    std::array<std::unique_ptr<NativeTextureImage>,5> images;
    std::array<std::unique_ptr<NativeGeometrySampler>,5> samplers;
    std::array<VkDescriptorImageInfo,5> descriptors{};
    for(unsigned i=0;i<5;++i) {
        images[i]=NativeTextureImage::createUnpackedMips(gpu,plan->textures[i],f.textures[i].base,f.textures[i].mips);assert(images[i]);
        samplers[i]=NativeGeometrySampler::create(gpu,*images[i]);assert(samplers[i]);
        descriptors[i]={samplers[i]->sampler(),images[i]->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    const auto input=IndexedLodFixture::read(std::string(argv[1])+"/input.bin");
    const auto batch=executeNativeOrderedTriangleBatches(f.indices,[&](auto triangle) {
        return executeNativeOrderedFragmentRaster(gpu,program.fragments,input,triangle,plan->viewport,descriptors,
            OrderedFragment7e3MaxCapacity,&why);
    },&why);
    if(!batch){std::printf("INDEXED601_RASTER_REFUSED stage=%s\n",why?why:"unknown");return 2;}
    assert(batch->primitiveCount==12 && !batch->fragments.empty());
    constexpr unsigned pixels=1280*720;std::vector<std::uint8_t> covered(12*pixels);
    std::array<unsigned,12> counts{};std::vector<unsigned> coverage(pixels);
    struct Point {double x,y;};std::array<Point,24> xy{};
    for(unsigned i=0;i<24;++i) {const auto& p=positions->at(i);
        assert(p[3]>0 && p[2]>0 && p[2]<p[3]);xy[i]={640.0*p[0]/p[3]+640.5,-360.0*p[1]/p[3]+360.5};}
    // Framebuffer Y increases DOWN: visual CCW has negative algebraic cross.
    // Retained600 control: +2348/+2305 areas cull, -2325/-2263 render. Negate
    // cross so an admitted CCW face and its interior distances are positive.
    const auto edge=[](Point a,Point b,Point p){return (p.x-a.x)*(b.y-a.y)-(p.y-a.y)*(b.x-a.x);};
    const auto distances=[&](unsigned primitive,Point p) {
        std::array<double,3> d{};
        for(unsigned i=0;i<3;++i){const auto a=xy[f.indices[3*primitive+i]],b=xy[f.indices[3*primitive+(i+1)%3]];
            const double length=std::hypot(b.x-a.x,b.y-a.y);assert(length>0);d[i]=edge(a,b,p)/length;}
        return d;
    };
    for(const auto& r:batch->fragments) {
        assert(r.primitive<12 && r.pixel<pixels && !covered[r.primitive*pixels+r.pixel]);
        covered[r.primitive*pixels+r.pixel]=1;++counts[r.primitive];++coverage[r.pixel];
        const auto a=xy[f.indices[3*r.primitive]],b=xy[f.indices[3*r.primitive+1]],c=xy[f.indices[3*r.primitive+2]];
        assert(edge(a,b,c)>0); // independently checked CCW/back-cull orientation
        for(double d:distances(r.primitive,{double(r.pixel%1280)+.5,double(r.pixel/1280)+.5}))assert(d>=-1.0);
    }
    unsigned interiors=0;
    for(unsigned primitive=0;primitive<12;++primitive) {
        const auto a=xy[f.indices[3*primitive]],b=xy[f.indices[3*primitive+1]],c=xy[f.indices[3*primitive+2]];
        if(edge(a,b,c)<=0){assert(!counts[primitive]);continue;}
        for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
            const auto d=distances(primitive,{double(x)+.5,double(y)+.5});
            if(*std::min_element(d.begin(),d.end())>=1.0){++interiors;assert(covered[primitive*pixels+y*1280+x]);}
        }
    }
    assert(interiors>0);unsigned seed=0;
    assert(packColor7e3({colorBits(.25f),colorBits(.5f),colorBits(1),colorBits(1)},seed));
    std::vector<unsigned> referenceColor(pixels,seed),referenceDepth(pixels);std::vector<std::uint8_t> seedColor(pixels*4);
    for(unsigned p=0;p<pixels;++p){referenceDepth[p]=p&255;for(unsigned c=0;c<4;++c)seedColor[p*4+c]=seed>>(8*c);}
    const auto ownerSeed=[&] {NativeEdram32Bands owner;std::vector<unsigned> depth(pixels);
        for(unsigned p=0;p<pixels;++p)depth[p]=p&255;
        assert(owner.recordCompletedRawColorRows(720,1280,1280,720,seedColor,1));
        assert(owner.recordCompletedDepthWords(0,1280,1280,720,depth,2));return owner;};
    // The output policy is shared with production, not an independent Xbox
    // blend oracle. The independent checks above cover original positions,
    // front-face classification and every unambiguous interior sample.
    for(const auto& r:batch->fragments) {
        const auto value=mergeNativeFragment7e3Depth(r.sample,referenceColor[r.pixel],referenceDepth[r.pixel]);assert(value);
        referenceColor[r.pixel]=value->color;referenceDepth[r.pixel]=value->depthStencil;
    }
    auto owner=ownerSeed();
    {NativeEdram32Mutation mutation(owner);
        if(!executeNativeIndexedLodDraw(gpu,program,f.draw,f.indices,f.bound,f.textures,
            f.positions.vertexIdentity,f.positions.pixelIdentity,true,mutation,&why)) {
            std::printf("INDEXED601_PRODUCTION_REFUSED stage=%s\n",why?why:"unknown");return 3;}}
    const auto color=owner.readColorWords(720,1280,1280,720),depth=owner.readDepthWords(0,1280,1280,720);
    assert(color && depth && *color==referenceColor && *depth==referenceDepth);
    std::uint64_t hash=14695981039346656037ULL;unsigned changed=0,overlap=0;
    for(unsigned p=0;p<pixels;++p){changed+=depth->at(p)!=(p&255);overlap+=coverage[p]>1;assert((depth->at(p)&255)==(p&255));
        for(auto word:{color->at(p),depth->at(p)}){hash^=word;hash*=1099511628211ULL;}}
    // Actual shader/geometry capacity1 overflow may never publish an owner.
    const auto active=std::find_if(counts.begin(),counts.end(),[](unsigned n){return n>1;});assert(active!=counts.end());
    const unsigned primitive=active-counts.begin();owner=ownerSeed();
    {NativeEdram32Mutation mutation(owner);
        assert(!executeNativeOrderedFragment7e3Physical(mutation,[&] {
            return executeNativeOrderedFragmentRaster(gpu,program.fragments,input,
                std::span<const unsigned>(f.indices).subspan(3*primitive,3),plan->viewport,descriptors,1,&why);
        }));}
    assert(!owner.knownTiles());
    for(unsigned mode=0;mode<4;++mode) {
        owner=ownerSeed();auto bad=program;auto identity=f.positions.vertexIdentity;
        if(mode==0)bad.abi=32;
        if(mode==1)bad.profile=NativeIndexedLodProfile::Indexed600;
        if(mode==2)identity[0]^=1;
        if(mode==3)bad.fragments.pixel.clear();
        {NativeEdram32Mutation mutation(owner);assert(!executeNativeIndexedLodDraw(gpu,bad,f.draw,f.indices,f.bound,f.textures,
            identity,f.positions.pixelIdentity,true,mutation,&why));}
        assert(!owner.knownTiles());
        if(mode==3)assert(why && std::strcmp(why,"ordered-raster-program")==0);
    }
    std::printf("INDEXED601_BATCHED_GPU_PASS positions_exact=96 triangles=12 fragments=%zu interior_pixels=%u changed_pixels=%u overlap_pixels=%u paired_hash=%016llx full_owner_comparison=1 overflow_refused=1 authored_owner=1 live_draw=0\n",
        batch->fragments.size(),interiors,changed,overlap,static_cast<unsigned long long>(hash));
    std::printf("INDEXED601_PRIMITIVE_COUNTS");for(auto n:counts)std::printf(" %u",n);std::puts("");
    std::puts("INDEXED601_PRODUCTION_NEGATIVES_PASS cases=4 wrong_abi_profile_identity=1 raster_failure_stage_preserved=1 unknown_owner=1");
    if(argc==3)std::puts("INDEXED601_WINDOWED_RETAINED_PASS original_reference_hash=1 xenia_packing=1 production_connection=1");
}
