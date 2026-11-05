// Host-only saved input -> state/resource plan. No GPU or guest completion.
#include "native_screen_strip_plan.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string_view>
#ifndef RECOMP_PRIVATE_STRIP_POSITION_PACK
#error Exact private shader identities are required
#endif
#include RECOMP_PRIVATE_STRIP_POSITION_PACK
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);const std::string folder=argv[1];std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto read=[&](const char* name,unsigned size) {
        std::ifstream f(folder+"/"+name,std::ios::binary|std::ios::ate);assert(f && f.tellg()==size);
        f.seekg(0);std::vector<std::uint8_t> b(size);assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
    };
    const auto be=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto put=[](auto& b,unsigned at,unsigned word){for(unsigned i=0;i<4;++i)b[at+i]=word>>(24-8*i);};
    Gta5InlineDraw draw;draw.primitive=6;draw.vertexCount=100;draw.stride=8;
    draw.vertices=read("vertices.bin",800);draw.deviceState=read("device-state.bin",13860);
    draw.vertexShader=be(draw.deviceState,13072);draw.pixelShader=be(draw.deviceState,13068);
    const auto meta=read("strip-meta.bin",36);
    draw.scissorRegisters=std::array<std::uint32_t,3>{be(meta,0),be(meta,4),be(meta,8)};
    Gta5InlineDraw::Texture texture;texture.slot=3;
    for(unsigned i=0;i<6;++i)texture.fetch[i]=be(meta,12+i*4);
    texture.source=read("texture-3.bin",3768320);draw.textures={texture};
    Gta5BoundSurfaceCapture bindings;
    for(unsigned i=0;i<5;++i)bindings.surfaces[i].object=be(draw.deviceState,12816+i*4);
    bindings.surfaces[0].header=read("surface-0-argument.bin",40);
    bindings.surfaces[4].header=read("surface-4-argument.bin",40);
    NativeScreenStripIdentity identity{screenStripPositions.vertexIdentity,screenStripPositions.pixelIdentity};
    const auto vs=read("vertex-shader.bin",300),ps=read("pixel-shader.bin",492);
    const char* why=nullptr;
    const auto plan=planNativeScreenStrip(draw,bindings,identity,vs,ps,false,true,&why);
    if(!plan)std::printf("STRIP_PLAN_REFUSED stage=%s\n",why);
    assert(plan && plan->expandedVertices==294 && plan->colorBase==720 && plan->depthBase==0 &&
        plan->textureIndex==0 && plan->texture.format==23 && plan->width==1280 && plan->height==720);
    unsigned negatives=0;
    const auto refuses=[&](const auto& d,const auto& b) {
        assert(!planNativeScreenStrip(d,b,identity,vs,ps,false,true));++negatives;
    };
    for(unsigned field=0;field<9;++field) {
        auto d=draw;
        if(field==0)d.primitive=4;
        if(field==1)d.vertexCount=2;
        if(field==2)d.vertexCount=32769;
        if(field==3)d.stride=12;
        if(field==4)d.vertices.pop_back();
        if(field==5)d.deviceState.pop_back();
        if(field==6)d.vertexShader^=4;
        if(field==7)d.pixelShader=0;
        if(field==8)d.scissorRegisters.reset();
        refuses(d,bindings);
    }
    for(unsigned r:{0x2000u,0x2001u,0x2002u,0x2104u,0x210Du,0x2113u,0x2114u,
        0x2180u,0x2181u,0x2182u,0x2183u,0x2184u,0x2200u,0x2201u,0x2202u,0x2203u,
        0x2204u,0x2205u,0x2206u,0x2293u,0x2301u,0x2302u,0x231Cu,0x2324u,0x2325u}) {
        for(unsigned bit=0;bit<32;++bit) {
            auto d=draw;put(d.deviceState,*gta5DrawRegisterOffset(r),*gta5DrawRegister(d,r)^(1u<<bit));
            refuses(d,bindings);
        }
    }
    for(unsigned i:{0u,4u}) {
        auto b=bindings;b.surfaces[i].header.reset();refuses(draw,b);
        b=bindings;b.surfaces[i].object^=4;refuses(draw,b);
        b=bindings;put(*b.surfaces[i].header,28,be(*b.surfaces[i].header,28)^0x10000);refuses(draw,b);
    }
    {auto d=draw;put(d.deviceState,12408,0);refuses(d,bindings);}
    {auto d=draw;d.textures.clear();refuses(d,bindings);}
    {auto d=draw;d.textures.push_back(d.textures[0]);refuses(d,bindings);}
    {auto d=draw;d.textures[0].source.pop_back();refuses(d,bindings);}
    for(unsigned i=0;i<6;++i) {
        auto d=draw;d.textures[0].fetch[i]^=i==1?1u:(i==3?1u<<19:1u);refuses(d,bindings);
    }
    assert(!planNativeScreenStrip(draw,bindings,identity,vs,ps,true,true));++negatives;
    assert(!planNativeScreenStrip(draw,bindings,identity,vs,ps,false,false));++negatives;
    auto wrong=ps;wrong[0]^=1;assert(!planNativeScreenStrip(draw,bindings,identity,vs,wrong,false,true));++negatives;
    // The actual point-sampled format23 decoder, including every texel and
    // unmodified source storage. This is not GPU sampling or a live owner.
    const auto decoded=decodeXenosDepth24FloatTexture(plan->texture,texture.source);
    assert(decoded && decoded->size()==1280*720*4);
    const auto rawPlan=planXenosDepth24FloatStorage(plan->texture);assert(rawPlan);
    const auto raw=linearizeXenosTexture2D(rawPlan->description,texture.source);assert(raw);
    unsigned nonzero=0;float minimum=2,maximum=0;
    for(unsigned i=0;i<1280*720;++i) {
        const auto at=i*4;const unsigned bits=unsigned((*raw)[at])|(unsigned((*raw)[at+1])<<8)|
            (unsigned((*raw)[at+2])<<16)|(unsigned((*raw)[at+3])<<24);
        const unsigned code=bits>>8,exponent=code>>20,mantissa=code&0xFFFFF;
        // Independent mathematical decode, without the production decoder.
        const float expected=exponent?float(1.+double(mantissa)/1048576.)*std::ldexp(1.f,int(exponent)-15):
            float(mantissa)*0x1p-34f;
        float got;std::memcpy(&got,decoded->data()+at,4);assert(got==expected && std::isfinite(got));
        nonzero+=got!=0;minimum=std::min(minimum,got);maximum=std::max(maximum,got);
    }
    for(unsigned count:{3u,4u,5u,100u,32768u}) {
        const auto n=nativeScreenStripExpandedCount(count,8,std::uint64_t{count}*8);assert(n && *n==(count-2)*3);
        for(unsigned i=0;i<*n;i+=3) {
            const unsigned last=2+i/3;
            assert(nativeScreenStripIndex(count,i)==last);
            assert(nativeScreenStripIndex(count,i+1)==(last%2?last-1:last-2));
            assert(nativeScreenStripIndex(count,i+2)==(last%2?last-2:last-1));
        }
        assert(!nativeScreenStripIndex(count,*n));
    }
    std::printf("SCREEN_STRIP_CAPTURE_PLAN_PASS vertices=100 expanded=294 negatives=%u depth_texels=921600 "
        "nonzero_depth=%u minimum=%.9g maximum=%.9g gpu=0 native_execution=0 gameplay=0\n",negatives,nonzero,minimum,maximum);
}
