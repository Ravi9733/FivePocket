// Saved current inputs through the production-owned capture and state planner.
// No guest execution, rasterization, allocation ownership oracle or GPU claim.
#include "native_filter_weights_plan.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
using namespace recomp::gpu;
namespace {
std::vector<std::uint8_t> read(const std::string& path,unsigned bytes) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f && f.tellg()==bytes);f.seekg(0);
    std::vector<std::uint8_t> b(bytes);assert(f.read(reinterpret_cast<char*>(b.data()),bytes));return b;
}
unsigned word(const std::vector<std::uint8_t>& b,unsigned at) {
    assert(at+4<=b.size());return (unsigned{b[at]}<<24)|(unsigned{b[at+1]}<<16)|(unsigned{b[at+2]}<<8)|b[at+3];
}
void put(std::vector<std::uint8_t>& b,unsigned at,unsigned v) {
    assert(at+4<=b.size());for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-i*8);
}
}
int main(int argc,char** argv) {
    assert(argc==3);const std::string cap=argv[1],input=argv[2];std::setvbuf(stdout,nullptr,_IONBF,0);
    auto state=read(cap+"/device-state.bin",13860);
    const auto vertices=read(cap+"/vertices.bin",72),source=read(input+"/base.bin",3768320);
    const auto meta=read(input+"/capture-meta.bin",64);
    std::array<unsigned,6> pf{},vf{};for(unsigned i=0;i<6;++i){pf[i]=word(meta,4*i);vf[i]=word(meta,24+4*i);}
    const unsigned address=word(state,13848),device=word(meta,60),base=pf[1]&0xFFFFF000u;
    unsigned reads=0,zeroReads=0;bool missingPixels=false,shortPixels=false,missingVertexFetch=false;
    const Gta5ResourceReader heap=[&](unsigned a,unsigned n)->std::optional<std::vector<std::uint8_t>> {
        if(a==device && n==13860)return state;return std::nullopt;
    };
    const Gta5ResourceReader data=[&](unsigned a,unsigned n)->std::optional<std::vector<std::uint8_t>> {
        ++reads;if(!a){++zeroReads;return std::nullopt;}
        if(a==address && n==72)return vertices;
        if(a==base && n==source.size() && !missingPixels) {
            auto bytes=source;if(shortPixels)bytes.pop_back();return bytes;
        }
        return std::nullopt;
    };
    const Gta5TextureFetchReader submitted=[&](unsigned slot)->std::optional<std::array<unsigned,6>> {
        if(slot==5)return pf;if(slot==17 && !missingVertexFetch)return vf;return std::nullopt;
    };
    auto captured=captureNativeFilterWeightsDraw(device,8,3,24,heap,data,submitted);
    assert(captured && reads==2 && !zeroReads && captured->textures[0].source==source &&
        captured->textures[1].literalZero && captured->textures[1].source.empty());
    auto& draw=*captured;
    draw.scissorRegisters=std::array<unsigned,3>{word(meta,48),word(meta,52),word(meta,56)};
    Gta5BoundSurfaceCapture bound;
    for(unsigned i=0;i<5;++i)bound.surfaces[i].object=word(state,12816+4*i);
    bound.surfaces[0].header=read(cap+"/surface-0-argument.bin",40);
    const NativeFilterWeightsIdentity identity{read(cap+"/vertex-shader.bin",132),read(cap+"/pixel-shader.bin",708)};
    const char* why=nullptr;
    const auto plan=planNativeFilterWeightsDraw(draw,bound,identity,identity.vertex,identity.pixel,false,true,&why);
    if(!plan)std::printf("FILTER_WEIGHTS_PLAN_REFUSED %s\n",why);
    assert(plan && plan->target.format==3 && plan->target.width==640 && plan->target.height==360 &&
        plan->target.baseTile==720 && plan->target.tileCount==184 && plan->texture.fetch==pf && plan->textureIndex==0);
    unsigned negatives=0;
    const auto refuses=[&]{assert(!planNativeFilterWeightsDraw(draw,bound,identity,identity.vertex,identity.pixel,false,true));++negatives;};
    for(unsigned offset:{0u,4u,8u,16u,20u}) {
        const auto saved=word(draw.vertices,offset);
        for(unsigned bad:{0x7FC00000u,0x7F800000u,0x3E800000u}) {
            put(draw.vertices,offset,bad);refuses();
        }
        put(draw.vertices,offset,saved);
    }
    std::copy_n(draw.vertices.begin(),24,draw.vertices.begin()+24);refuses();draw.vertices=vertices;
    // Unread padding is not a shader input and is not required to be finite.
    put(draw.vertices,12,0x7FC00000u);assert(nativeFilterWeightsFullRectangle(draw));draw.vertices=vertices;
    for(const auto& r:NativeFilterWeightsRegisters) {
        const auto offset=gta5DrawRegisterOffset(r[0]);assert(offset);
        for(unsigned bit=0;bit<32;++bit){put(draw.deviceState,*offset,r[1]^(1u<<bit));refuses();}
        put(draw.deviceState,*offset,r[1]);
    }
    const auto change=[&](unsigned offset,unsigned value) {
        const auto old=word(draw.deviceState,offset);put(draw.deviceState,offset,value);refuses();put(draw.deviceState,offset,old);
    };
    change(12408,1);change(13072,draw.vertexShader^4);change(13068,0);
    draw.deviceState[11068]^=0x10;refuses();draw.deviceState[11068]^=0x10;
    for(unsigned i=1;i<5;++i) {
        put(draw.deviceState,12816+4*i,0x1000);bound.surfaces[i].object=0x1000;refuses();
        put(draw.deviceState,12816+4*i,0);bound.surfaces[i].object=0;
    }
    auto savedHeader=bound.surfaces[0].header;
    bound.surfaces[0].tileCacheHeader=savedHeader;bound.surfaces[0].header.reset();refuses();
    bound.surfaces[0].header=savedHeader;
    for(unsigned at:{24u,28u,36u}) {
        const auto old=word(*bound.surfaces[0].header,at);put(*bound.surfaces[0].header,at,old^0x10000);refuses();
        put(*bound.surfaces[0].header,at,old);
    }
    bound.surfaces[0].object^=4;refuses();bound.surfaces[0].object^=4;
    draw.primitive=4;refuses();draw.primitive=8;
    draw.vertexCount=6;refuses();draw.vertexCount=3;
    draw.stride=36;refuses();draw.stride=24;
    draw.vertices.pop_back();refuses();draw.vertices=vertices;
    const auto scissor=draw.scissorRegisters;draw.scissorRegisters.reset();refuses();draw.scissorRegisters=scissor;
    assert(!planNativeFilterWeightsDraw(draw,bound,identity,identity.vertex,identity.pixel,true,true));++negatives;
    assert(!planNativeFilterWeightsDraw(draw,bound,identity,identity.vertex,identity.pixel,false,false));++negatives;
    auto ps=identity.pixel;ps[0]^=1;
    assert(!planNativeFilterWeightsDraw(draw,bound,identity,identity.vertex,ps,false,true));++negatives;
    auto& pixel=draw.textures[0];auto& vertex=draw.textures[1];
    pixel.source.pop_back();refuses();pixel.source.push_back(source.back());
    pixel.literalZero=true;refuses();pixel.literalZero=false;
    vertex.literalZero=false;refuses();vertex.literalZero=true;
    vertex.source.push_back(0);refuses();vertex.source.clear();
    for(unsigned i=0;i<6;++i) {
        pixel.fetch[i]^=1;refuses();pixel.fetch[i]^=1;
        vertex.fetch[i]^=1;refuses();vertex.fetch[i]^=1;
    }
    vertex.slot=5;refuses();vertex.slot=17;
    std::swap(draw.textures[0],draw.textures[1]);
    const auto reversed=planNativeFilterWeightsDraw(draw,bound,identity,identity.vertex,identity.pixel,false,true);
    assert(reversed && reversed->textureIndex==1);std::swap(draw.textures[0],draw.textures[1]);
    const auto capture=[&]{return captureNativeFilterWeightsDraw(device,8,3,24,heap,data,submitted);};
    missingPixels=true;assert(!capture());++negatives;missingPixels=false;
    shortPixels=true;assert(!capture());++negatives;shortPixels=false;
    missingVertexFetch=true;assert(!capture());++negatives;missingVertexFetch=false;
    vf[3]^=2;assert(!capture());++negatives;vf[3]^=2;
    assert(!captureNativeFilterWeightsDraw(device,8,3,24,heap,data,{}));++negatives;
    // Existing generic capture is deliberately not widened by the opt-in path.
    assert(!captureGta5InlineDraw(device,8,3,24,{5,17},heap,data,submitted));++negatives;
    assert(!zeroReads && capture());
    std::printf("FILTER_WEIGHTS_CAPTURE_PLAN_PASS current_capture=1 owned_pixel_bytes=3768320 zero_address_reads=0 "
        "vertex_literal_only=1 target=640x360 ordinary7e3=1 query_idle_required=1 tile_cache_not_authority=1 "
        "negatives=%u rasterization=0 gpu=0 completion=0\n",negatives);
}
