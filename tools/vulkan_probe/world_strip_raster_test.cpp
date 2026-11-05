// Original captured inputs, AUTHORED physical destinations. The authored PS
// modes test clipping/output writes separately from the real sampled PS.
#ifndef RECOMP_PRIVATE_WORLD_STRIP_RASTER_PACK
#error Explicit private original and diagnostic modules required
#endif
#include RECOMP_PRIVATE_WORLD_STRIP_RASTER_PACK
#include "native_world_strip_capture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);const std::string folder=argv[1];std::setvbuf(stdout,nullptr,_IONBF,0);
    const auto read=[&](const std::string& name,unsigned size) {
        std::ifstream f(folder+"/"+name,std::ios::binary|std::ios::ate);assert(f && f.tellg()==size);
        f.seekg(0);std::vector<std::uint8_t> b(size);assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
    };
    const auto be=[](const auto& b,unsigned at){return (std::uint32_t{b[at]}<<24)|
        (std::uint32_t{b[at+1]}<<16)|(std::uint32_t{b[at+2]}<<8)|b[at+3];};
    const auto put=[](auto& b,unsigned at,unsigned w){for(unsigned c=0;c<4;++c)b[at+c]=w>>(8*c);};
    Gta5InlineDraw draw;draw.primitive=6;draw.vertexCount=4;draw.stride=36;
    draw.vertices=read("vertices.bin",144);draw.deviceState=read("device-state.bin",13860);
    draw.vertexShader=be(draw.deviceState,13072);draw.pixelShader=be(draw.deviceState,13068);
    assert(read("vertex-shader.bin",252)==worldStripOriginal.draw.vertexIdentity);
    assert(read("pixel-shader.bin",912)==worldStripOriginal.draw.pixelIdentity);
    const auto vc=read("vertex-constants.bin",4096),pc=read("pixel-constants.bin",4096),scissor=read("scissor.bin",12);
    for(unsigned i=0;i<1024;++i){draw.vertexConstants[i]=be(vc,4*i);draw.pixelConstants[i]=be(pc,4*i);}
    draw.scissorRegisters=std::array<unsigned,3>{be(scissor,0),be(scissor,4),be(scissor,8)};
    Gta5BoundSurfaceCapture bound;
    for(unsigned i=0;i<5;++i)bound.surfaces[i].object=be(draw.deviceState,12816+4*i);
    bound.surfaces[0].header=read("surface-0-argument.bin",40);bound.surfaces[4].header=read("surface-4-argument.bin",40);
    NativeWorldStripTextures textures;const auto meta=read("texture-meta.bin",144);
    constexpr unsigned slots[]{2,6,1,3};
    for(unsigned i=0;i<4;++i) {
        unsigned at=144;for(unsigned j=0;j<4;++j)if(be(meta,j*36)==slots[i])at=j*36;assert(at<144);
        auto& t=textures[i];for(unsigned j=0;j<6;++j)t.fetch[j]=be(meta,at+12+4*j);
        t.base=read("texture-"+std::to_string(slots[i])+".bin",be(meta,at+4));
        t.mips=read("texture-"+std::to_string(slots[i])+"-mips.bin",be(meta,at+8));
    }
    const auto owned=captureNativeWorldStripTextures([&](unsigned slot)->std::optional<std::array<unsigned,6>> {
        for(unsigned i=0;i<4;++i)if(slots[i]==slot)return textures[i].fetch;return std::nullopt;
    },[&](unsigned address,unsigned bytes)->std::optional<std::vector<std::uint8_t>> {
        for(const auto& t:textures) {
            const auto d=*XenosTextureDescription::decode(t.fetch);
            if(address==d.baseAddress && bytes==t.base.size())return t.base;
            if(address==d.mipAddress && !t.mips.empty() && bytes==t.mips.size())return t.mips;
        }
        return std::nullopt;
    });
    assert(owned);textures=*owned;
    constexpr unsigned pixels=1280*720;
    // Independent host homogeneous clipping reference:0 outside,1 deep inside,
    // 2 within two pixels of a clip/triangle boundary (not a precision oracle).
    const auto coverage=read("coverage-reference.bin",pixels);
    ProbeDevice device;ShaderRequirements requirements;assert(device.inspect() && device.create(requirements));
    unsigned sentinel=0;assert(packColor7e3({colorBits(.125f),colorBits(.5f),colorBits(2.f),colorBits(1.f/3.f)},sentinel));
    unsigned authored=0;assert(packColor7e3({colorBits(5),colorBits(.25f),colorBits(-1),colorBits(1.5f)},authored));
    for(unsigned mode=0;mode<4;++mode) {
        auto program=worldStripOriginal;
        // Never used in the installable pack: explicit test PS overrides.
        if(mode==1)program.draw.pixelCode=worldStripAuthoredColor;
        if(mode==2)program.draw.pixelCode=worldStripAuthoredKill;
        std::vector<std::uint8_t> colors(pixels*4);std::vector<unsigned> depth(pixels);
        for(unsigned i=0;i<pixels;++i) {
            put(colors,4*i,sentinel);
            depth[i]=((mode==3?0u:i%3==0?0u:i%3==1?0x400u:0x401u)<<8)|0xA5u;
        }
        NativeEdram32Bands owner;assert(owner.recordCompletedRawColorRows(720,1280,1280,720,colors,1));
        assert(owner.recordCompletedDepthWords(0,1280,1280,720,depth,2));
        assert(owner.recordCompletedRawColorRows(1500,80,80,16,std::vector<std::uint8_t>(5120,0xAC),3));
        const auto unrelated=owner.readColorWords(1500,80,80,16);const char* why=nullptr;
        {NativeEdram32Mutation mutation(owner);
        const bool success=executeNativeWorldStripDraw(device,program,draw,bound,textures,true,mutation,&why);
        if(!success)std::printf("WORLD_STRIP_RASTER_REFUSED mode=%u stage=%s\n",mode,why?why:"unknown");
        assert(success);}
        const auto result=owner.readColorWords(720,1280,1280,720),afterDepth=owner.readDepthWords(0,1280,1280,720);
        assert(result && afterDepth && owner.readColorWords(1500,80,80,16)==unrelated);
        unsigned changed=0,checked=0,passing=0;
        for(unsigned i=0;i<pixels;++i) {
            const bool didChange=(*result)[i]!=sentinel || (*afterDepth)[i]!=depth[i];changed+=didChange;
            if(mode!=3 && i%3==2)assert(!didChange); // Depth failure cannot alter ANY aspect.
            if(mode==2 && i%1280<640)assert(!didChange); // Discard also preserves all aspects.
            if(!mode || mode==3 || coverage[i]==2)continue;
            const bool expect=coverage[i]==1 && i%3!=2 && (mode!=2 || i%1280>=640);
            if((*result)[i]!=(expect?authored:sentinel) || (*afterDepth)[i]!=(expect?0x40000u:depth[i]))
                std::printf("WORLD_STRIP_COVERAGE_MISMATCH mode=%u pixel=%u coverage=%u color=%08x depth=%08x expect=%u\n",
                    mode,i,coverage[i],(*result)[i],(*afterDepth)[i],expect);
            assert((*result)[i]==(expect?authored:sentinel));assert((*afterDepth)[i]==(expect?0x40000u:depth[i]));
            ++checked;passing+=expect;
        }
        assert(changed>0);if(mode==1 || mode==2)assert(checked>800000 && passing>1000);
        if(mode==3) {
            const auto reference=read("pixel-reference.bin",4+100*24);assert(be(reference,0)==100);
            unsigned maximumCodeError=0,exact=0,mismatch=0;
            for(unsigned i=0;i<100;++i) {
                const auto at=4+i*24,x=be(reference,at),y=be(reference,at+4);assert(x<1280 && y<720);
                std::array<unsigned,4> rgba{};for(unsigned c=0;c<4;++c)rgba[c]=be(reference,at+8+c*4);
                unsigned expected=0;assert(packColor7e3(rgba,expected));
                const auto actual=(*result)[y*1280+x];unsigned error=0;
                for(unsigned c=0;c<3;++c) {
                    const auto a=(actual>>(10*c))&1023,e=(expected>>(10*c))&1023;
                    error=std::max(error,a>e?a-e:e-a);
                }
                maximumCodeError=std::max(maximumCodeError,error);exact+=actual==expected;
                // A predeclared one-storage-code envelope for independent
                // ideal filtering/interpolation vs native approximations, NOT
                // Xbox bit equivalence. Alpha and depth/stencil stay exact.
                const bool match=error<=1 && (actual>>30)==(expected>>30) && (*afterDepth)[y*1280+x]==0x40000;
                if(!match)std::printf("WORLD_STRIP_PIXEL_MISMATCH x=%u y=%u actual=%08x expected=%08x code_error=%u depth=%08x\n",
                    x,y,actual,expected,error,(*afterDepth)[y*1280+x]);
                mismatch+=!match;
            }
            std::printf("WORLD_STRIP_PIXEL_REFERENCE compared=100 exact=%u max_code_error=%u allowed_code_error=1 mismatches=%u\n",exact,maximumCodeError,mismatch);
            assert(mismatch==0);
        }
        std::printf("WORLD_STRIP_RASTER_MODE_PASS mode=%u changed=%u checked=%u passing=%u physical_published=1\n",mode,changed,checked,passing);
    }
    std::puts("WORLD_STRIP_RASTER_GPU_PASS modes=4 original_ps_reference_pixels=100 authored_owner=1 installation=0 gameplay=0");
}
