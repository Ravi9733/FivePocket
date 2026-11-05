#include "indexed_lod_fixture.h"
#include "native_indexed_lod_draw.h"
#include "native_ordered_fragment_windows.h"
#include "raw10_resolve_connection_fixture.h"
#include "stencil_clear_connection_fixture.h"
#include "sampled_stencil_connection_fixture.h"
#include "process_limits.h"
#include <cstdio>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
namespace {
using H=std::array<double,4>;
struct P {double x,y,z;};
// Independent homogeneous Sutherland-Hodgman reference, not production clipping.
// Vulkan clips before perspective divide; NoPerspective outputs remain linear
// in framebuffer coordinates (spec: vertexpostproc, Clipping Shader Outputs).
std::vector<P> clipped(const std::array<H,3>& triangle) {
    std::vector<H> polygon(triangle.begin(),triangle.end());
    for(unsigned plane=0;plane<6;++plane) {
        const auto distance=[&](const H& p){switch(plane){
            case 0:return p[3]+p[0];case 1:return p[3]-p[0];
            case 2:return p[3]+p[1];case 3:return p[3]-p[1];
            case 4:return p[2];default:return p[3]-p[2];}};
        std::vector<H> next;
        if(polygon.empty())break;
        auto a=polygon.back();double da=distance(a);
        for(const auto& b:polygon) {
            const double db=distance(b);
            if((da>=0)!=(db>=0)) {
                const double t=da/(da-db);H p{};
                for(unsigned c=0;c<4;++c)p[c]=a[c]+t*(b[c]-a[c]);
                next.push_back(p);
            }
            if(db>=0)next.push_back(b);a=b;da=db;
        }
        polygon=std::move(next);
    }
    std::vector<P> result;
    for(const auto& p:polygon){assert(p[3]>0);result.push_back({1024*(p[0]/p[3]+1),512*(p[1]/p[3]+1),1-p[2]/p[3]});}
    return result;
}
double edge(P a,P b,P p){return(p.x-a.x)*(b.y-a.y)-(p.y-a.y)*(b.x-a.x);}
double area(const std::vector<P>& p) {
    double result=0;for(unsigned i=1;i+1<p.size();++i)result+=edge(p[0],p[i],p[i+1]);return result;
}
std::vector<std::array<double,3>> edgePlanes(const std::vector<P>& polygon) {
    std::vector<std::array<double,3>> result;
    for(unsigned i=0;i<polygon.size();++i){const auto a=polygon[i],b=polygon[(i+1)%polygon.size()];
        const double dx=b.x-a.x,dy=b.y-a.y,length=std::hypot(dx,dy);
        if(length>1e-12)result.push_back({dy/length,-dx/length,(a.y*dx-a.x*dy)/length});}
    return result;
}
double minimumDistance(const std::vector<std::array<double,3>>& planes,P p) {
    double result=1e100;for(const auto& e:planes)result=std::min(result,e[0]*p.x+e[1]*p.y+e[2]);return result;
}
double depth(const std::vector<P>& polygon,P p) {
    for(unsigned i=1;i+1<polygon.size();++i) {
        const auto a=polygon[0],b=polygon[i],c=polygon[i+1];const double d=edge(a,b,c);
        if(std::abs(d)<1e-8)continue;
        return(edge(b,c,p)*a.z+edge(c,a,p)*b.z+edge(a,b,p)*c.z)/d;
    }
    assert(false);return 0;
}
}
int main(int argc,char** argv) {
    assert((argc>=2 && argc<=6) && initializeProbeProcess());std::setvbuf(stdout,nullptr,_IONBF,0);
    IndexedLodFixture f(argv[1],NativeIndexedLodProfile::Indexed601);
    assert(f.draw.vertexCount==40 && f.indices.size()==72);const char* why=nullptr;
    assert(!planNativeIndexedLod(f.draw,f.indices,f.bound,f.textures,true,&why,NativeIndexedLodProfile::Indexed601));
    assert(std::strcmp(why,"indexed-lod-extent")==0); // live admission STILL refuses
    // A subset establishes exact state/texture ingredients only. It does not
    // admit the full draw or bypass its finite-positive-W production guard.
    const auto plan=planNativeIndexedLod(f.draw,std::span<const unsigned>(f.indices).first(3),
        f.bound,f.textures,true,&why,NativeIndexedLodProfile::Indexed601);assert(plan);
    ProbeDevice gpu;ShaderRequirements requirements;requirements.fragmentStoresAndAtomics=true;
    assert(gpu.inspect() && gpu.create(requirements));
    auto positionExecutor=NativeVertexClipExecutor::create(gpu,f.positions);assert(positionExecutor);
    const auto positions=positionExecutor->indexedPositions(f.draw);assert(positions && positions->size()==40);
    const auto expected=IndexedLodFixture::read(std::string(argv[1])+"/positions.bin");assert(expected.size()==640);
    unsigned negativeW=0;std::array<H,40> native{};
    for(unsigned i=0;i<40;++i) {
        const auto& p=positions->at(i);negativeW+=p[3]<0;
        for(unsigned c=0;c<4;++c){assert(colorBits(p[c])==IndexedLodFixture::be(expected,16*i+4*c));assert(std::isfinite(p[c]));}
        assert(p[3]!=0);
        native[i]={double(p[0])*plan->viewport[0]+double(p[3])*plan->viewport[2],
            double(p[1])*plan->viewport[1]+double(p[3])*plan->viewport[3],p[2],p[3]};
    }
    assert(negativeW>0);
    std::array<std::vector<P>,24> polygons;
    for(unsigned i=0;i<24;++i)polygons[i]=clipped({native[f.indices[3*i]],native[f.indices[3*i+1]],native[f.indices[3*i+2]]});
    std::array<std::vector<std::array<double,3>>,24> planes;
    for(unsigned i=0;i<24;++i)planes[i]=edgePlanes(polygons[i]);
    std::array<std::unique_ptr<NativeTextureImage>,5> images;
    std::array<std::unique_ptr<NativeGeometrySampler>,5> samplers;
    std::array<VkDescriptorImageInfo,5> descriptors{};
    for(unsigned i=0;i<5;++i){images[i]=NativeTextureImage::createUnpackedMips(gpu,plan->textures[i],f.textures[i].base,f.textures[i].mips);assert(images[i]);
        samplers[i]=NativeGeometrySampler::create(gpu,*images[i]);assert(samplers[i]);
        descriptors[i]={samplers[i]->sampler(),images[i]->view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};}
    const auto input=IndexedLodFixture::read(std::string(argv[1])+"/input.bin");
    NativeOrderedFragmentRasterProgram program{f.vertexCode,f.pixelCode};
    auto executor=NativeOrderedFragmentRasterExecutor::create(gpu,program,input,
        std::span<const unsigned>(f.indices).first(3),plan->viewport,descriptors,OrderedFragment7e3MaxCapacity,&why);assert(executor);
    unsigned calls=0;
    const auto batch=executeNativeOrderedTriangleWindows(f.indices,[&](auto triangle,unsigned row,unsigned rows) {
        ++calls;auto b=executor->execute(triangle,{{0,int(row)},{1280,rows}},&why);
        if(!b)std::printf("INDEXED602_BAND_REFUSED call=%u row=%u stage=%s\n",calls,row,why?why:"unknown");return b;
    },&why);
    if(!batch){std::printf("INDEXED602_WINDOWS_REFUSED stage=%s\n",why?why:"unknown");return 2;}
    assert(calls==360 && batch->primitiveCount==24 && !batch->fragments.empty());
    constexpr unsigned pixels=1280*720;std::vector<std::uint8_t> covered(24*pixels);
    std::array<unsigned,24> counts{};std::vector<unsigned> coverage(pixels);double maximumDepthError=0;
    for(const auto& r:batch->fragments) {
        assert(r.primitive<24 && r.pixel<pixels && !covered[r.primitive*pixels+r.pixel]);
        covered[r.primitive*pixels+r.pixel]=1;++counts[r.primitive];++coverage[r.pixel];
        const auto& polygon=polygons[r.primitive];assert(polygon.size()>=3 && area(polygon)>0);
        const P point{double(r.pixel%1280)+.5,double(r.pixel/1280)+.5,0};
        assert(minimumDistance(planes[r.primitive],point)>=-1.0);
        const double error=std::abs(depth(polygon,point)-r.sample.depth);maximumDepthError=std::max(maximumDepthError,error);
        assert(error<=2e-6); // predeclared CPU-double vs GPU-float clipping envelope
    }
    unsigned interiors=0;
    for(unsigned i=0;i<24;++i) {
        const auto& polygon=polygons[i];if(polygon.size()<3 || area(polygon)<=0){assert(!counts[i]);continue;}
        for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x)
            if(minimumDistance(planes[i],{double(x)+.5,double(y)+.5,0})>=1.0){++interiors;assert(covered[i*pixels+y*1280+x]);}
    }
    assert(interiors>0);
    std::printf("INDEXED602_CLIP_DEPTH_PASS positions_exact=160 negative_w=%u windows=%u fragments=%zu interiors=%u max_depth_error=%.12g live_admission=0\n",
        negativeW,calls,batch->fragments.size(),interiors,maximumDepthError);
    std::printf("INDEXED602_PRIMITIVE_COUNTS");for(unsigned n:counts)std::printf(" %u",n);std::puts("");
    unsigned seed=0;assert(packColor7e3({colorBits(.25f),colorBits(.5f),colorBits(1),colorBits(1)},seed));
    std::vector<unsigned> color(pixels,seed),z(pixels);std::vector<std::uint8_t> bytes(pixels*4);
    for(unsigned p=0;p<pixels;++p){z[p]=p&255;for(unsigned c=0;c<4;++c)bytes[4*p+c]=seed>>(8*c);}
    NativeEdram32Bands owner;assert(owner.recordCompletedRawColorRows(720,1280,1280,720,bytes,1));
    assert(owner.recordCompletedDepthWords(0,1280,1280,720,z,2));
    const auto originalOwner=owner;
    for(const auto& f:batch->fragments){const auto r=mergeNativeFragment7e3Depth(f.sample,color[f.pixel],z[f.pixel],Color7e3Rounding::XeniaShiftRound);
        if(!r){std::printf("INDEXED602_MERGE_REFUSED primitive=%u pixel=%u rgba=%08x,%08x,%08x,%08x depth=%08x destination=%08x depth_stencil=%08x\n",
            f.primitive,f.pixel,colorBits(f.sample.color[0]),colorBits(f.sample.color[1]),colorBits(f.sample.color[2]),colorBits(f.sample.color[3]),
            colorBits(f.sample.depth),color[f.pixel],z[f.pixel]);return 3;}
        color[f.pixel]=r->color;z[f.pixel]=r->depthStencil;}
    {NativeEdram32Mutation mutation(owner);assert(executeNativeOrderedFragment7e3Physical(mutation,[&]{return batch;},nullptr,Color7e3Rounding::XeniaShiftRound));}
    assert(owner.readColorWords(720,1280,1280,720)==color && owner.readDepthWords(0,1280,1280,720)==z);
    NativeIndexedLodProgram productionProgram{f.positions,{f.vertexCode,f.pixelCode},33,
        NativeIndexedLodProfile::Indexed601,NativeIndexedLodRaster::ClippedWindows};
    auto productionOwner=originalOwner;
    {NativeEdram32Mutation mutation(productionOwner);
        if(!executeNativeIndexedLodDraw(gpu,productionProgram,f.draw,f.indices,f.bound,f.textures,
            f.positions.vertexIdentity,f.positions.pixelIdentity,true,mutation,&why)) {
            std::printf("INDEXED602_PRODUCTION_REFUSED stage=%s\n",why?why:"unknown");return 4;}}
    assert(productionOwner.readColorWords(720,1280,1280,720)==color &&
        productionOwner.readDepthWords(0,1280,1280,720)==z);
    if(argc>=3)checkRaw10ResolveConnection(readRaw10ResolveConnection(argv[2]),productionOwner,color);
    if(argc>=4)checkStencilClearConnection(Color8ClearExecutionFixture(argv[3]).capture,productionOwner,z,color);
    const auto rejected=argc==6?std::make_optional<PostSampledStencilFixture>(argv[5]):std::nullopt;
    if(argc>=5)checkSampledStencilConnection(gpu,SampledStencilFixture(argv[4]),
        Color8ClearExecutionFixture(argv[3]).capture,productionOwner,color,rejected?&*rejected:nullptr);
    for(unsigned mode=0;mode<6;++mode) {
        auto bad=productionProgram;auto identity=f.positions.vertexIdentity;productionOwner=originalOwner;
        if(mode==0)bad.abi=32;
        if(mode==1)bad.raster=NativeIndexedLodRaster::LegacyBatches;
        if(mode==2)identity[0]^=1;
        if(mode==3)bad.fragments.pixel.clear();
        if(mode==5)assert(productionOwner.invalidate(720,1));
        {NativeEdram32Mutation mutation(productionOwner);
            assert(!executeNativeIndexedLodDraw(gpu,bad,f.draw,f.indices,f.bound,f.textures,
                identity,f.positions.pixelIdentity,mode!=4,mutation,&why));}
        assert(!productionOwner.knownTiles());
        if(mode==3)assert(why && std::strcmp(why,"ordered-raster-program")==0);
    }
    std::uint64_t hash=14695981039346656037ULL;unsigned overlap=0;
    for(unsigned p=0;p<pixels;++p){overlap+=coverage[p]>1;for(auto value:{color[p],z[p]}){hash^=value;hash*=1099511628211ULL;}}
    assert(!executor->execute(std::span<const unsigned>(f.indices).first(3),{{0,720},{1280,1}},&why));
    assert(std::strcmp(why,"ordered-raster-window")==0);
    std::printf("INDEXED602_WINDOWED_GPU_PASS positions_exact=160 negative_w=%u triangles=24 windows=%u fragments=%zu interiors=%u overlap_pixels=%u max_depth_error=%.12g paired_hash=%016llx shared_blend_reference=1 authored_owner=1 live_admission=0\n",
        negativeW,calls,batch->fragments.size(),interiors,overlap,maximumDepthError,static_cast<unsigned long long>(hash));
    std::printf("INDEXED602_PRIMITIVE_COUNTS");for(unsigned n:counts)std::printf(" %u",n);std::puts("");
    std::puts("INDEXED602_PRODUCTION_PASS current_positions=1 exact_resources=1 paired_full_owner_match=1 negatives=6 query_refused=1 legacy_guard_retained=1 live_draw=0");
}
