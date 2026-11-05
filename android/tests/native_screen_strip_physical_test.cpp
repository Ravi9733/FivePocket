// Authored ownership/geometry contract only. GPU coverage is a separate probe.
#include "native_screen_strip_physical.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
int main() {
    std::vector<std::array<float,4>> grid;
    for(unsigned row=0;row<5;++row) {
        const auto vertex=[&](unsigned col,unsigned r){return std::array<float,4>{-1.f+col*.25f,1.f-r*.4f,.5f,1.f};};
        grid.push_back(vertex(0,row));
        for(unsigned col=0;col<9;++col){grid.push_back(vertex(col,row));grid.push_back(vertex(col,row+1));}
        grid.push_back(vertex(8,row+1));
    }
    assert(nativeScreenStripSingleCoverage(grid));unsigned negatives=0;
    for(unsigned at=0;at<100;++at)for(unsigned lane=0;lane<4;++lane) {
        auto bad=grid;bad[at][lane]=std::numeric_limits<float>::quiet_NaN();
        assert(!nativeScreenStripSingleCoverage(bad));++negatives;
    }
    for(unsigned kind=0;kind<9;++kind) {
        auto bad=grid;
        if(kind==0)bad.pop_back();if(kind==1)bad[0][0]+=.001f;
        if(kind==2)bad[19][1]-=.001f;if(kind==3)bad[44][0]=0;
        if(kind==4)bad[39][1]=-.2f;if(kind==5)bad[37][3]=2;
        if(kind==6)bad[11][2]=-1;if(kind==7)bad[11][2]=1.001f;
        if(kind==8)bad[21][2]=.6f; // Seams must be identical, including Z.
        assert(!nativeScreenStripSingleCoverage(bad));++negatives;
    }
    // Reordered columns/rows, despite equal seams, cannot pass the grid proof.
    auto folded=grid;for(auto& v:folded)if(v[0]==-.75f)v[0]=.75f;
    assert(!nativeScreenStripSingleCoverage(folded));++negatives;
    folded=grid;for(auto& v:folded)if(v[1]==grid[2][1])v[1]=-.8f;
    assert(!nativeScreenStripSingleCoverage(folded));++negatives;
    for(unsigned code=0;code<1024;++code) {
        std::uint32_t bits=0,back=0;assert(unpack7e3(code,bits) && pack7e3Checked(bits,back) && back==code);
    }
    constexpr unsigned pixels=1280*720;
    NativeScreenStripPlan plan;plan.width=plan.pitch=1280;plan.height=720;
    plan.colorBase=720;plan.depthBase=0;plan.colorObject=0x1000;plan.depthObject=0x2000;plan.expandedVertices=294;
    std::vector<std::uint8_t> colors(pixels*4);std::vector<std::uint32_t> depth(pixels);
    const auto put=[](auto& bytes,unsigned at,unsigned v){for(unsigned c=0;c<4;++c)bytes[at+c]=v>>(8*c);};
    for(unsigned i=0;i<pixels;++i) {
        // Every RGB code and alpha value, including negative/positive source
        // destination-independent outputs in the simulated consumer below.
        put(colors,4*i,(i%1024)*0x100401u|((i%4)<<30));
        depth[i]=((i*17u)&0xFFFFFF00u)|(i&255);
    }
    const auto seed=[&]{NativeEdram32Bands owner;
        assert(owner.recordCompletedRawColorRows(720,1280,1280,720,colors,1));
        assert(owner.recordCompletedDepthWords(0,1280,1280,720,depth,2));
        assert(owner.recordCompletedRawColorRows(1500,80,80,16,std::vector<std::uint8_t>(5120,0xA5),3));
        return owner;};
    auto owner=seed();const auto original=owner.readColorWords(720,1280,1280,720);
    const auto unrelated=owner.readColorWords(1500,80,80,16);
    const auto originalSerial=owner.nextSerial();const char* why=nullptr;
    {
        NativeEdram32Mutation mutation(owner);
        assert(executeNativeScreenStripPhysical(mutation,plan,grid,[&](const auto& rgba,const auto& words) {
            assert(!owner.knownTiles() && words==depth);
            return std::optional{NativeScreenStripReadback{rgba,words}};
        },&why));
    }
    assert(owner.readColorWords(720,1280,1280,720)==original && owner.readDepthWords(0,1280,1280,720)==std::optional{depth});
    assert(owner.readColorWords(1500,80,80,16)==unrelated && owner.nextSerial()==originalSerial+1);
    for(unsigned failure=0;failure<13;++failure) {
        owner=seed();auto p=plan;auto positions=grid;unsigned calls=0;
        if(failure==0)assert(owner.invalidate(721,1));
        if(failure==1)assert(owner.invalidate(0,1));
        if(failure==2)p.depthBase=720;
        if(failure==3)positions[0][0]=0;
        {
            NativeEdram32Mutation mutation(owner);
            assert(!executeNativeScreenStripPhysical(mutation,p,positions,[&](const auto& rgba,const auto& words)
                    ->std::optional<NativeScreenStripReadback> {
                ++calls;NativeScreenStripReadback result{rgba,words};
                if(failure==4)return std::nullopt;
                if(failure==5)result.rgba.pop_back();
                if(failure==6)result.depthStencil[0]^=0x100u;
                if(failure==7)result.depthStencil[0]^=1;
                if(failure==8)result.rgba[3]=.5f;
                if(failure==9)result.rgba[4]=std::numeric_limits<float>::infinity();
                if(failure==10)result.rgba[4]=std::numeric_limits<float>::quiet_NaN();
                if(failure==11)result.rgba[3]=-0.f; // Bit preservation, not equality alone.
                if(failure==12)result.depthStencil.clear();
                return result;
            },&why));
        }
        assert(!owner.knownTiles() && calls==(failure>=4?1u:0u));++negatives;
    }
    owner=seed();
    {
        NativeEdram32Mutation mutation(owner);
        assert(executeNativeScreenStripPhysical(mutation,plan,grid,[](const auto& rgba,const auto& words) {
            auto changed=rgba;for(unsigned i=0;i<pixels;++i){changed[4*i]=-1.f;changed[4*i+1]=64.f;changed[4*i+2]=.5f;}
            return std::optional{NativeScreenStripReadback{changed,words}};
        }));
    }
    const auto result=owner.readColorWords(720,1280,1280,720);assert(result);
    unsigned half=0;assert(pack7e3Checked(colorBits(.5f),half));
    for(unsigned i=0;i<pixels;++i)assert((*result)[i]==(((i%4)<<30)|(1023<<10)|(half<<20)));
    assert(owner.readDepthWords(0,1280,1280,720)==std::optional{depth} && owner.readColorWords(1500,80,80,16)==unrelated);
    std::printf("SCREEN_STRIP_PHYSICAL_HOST_PASS pixels=%u roundtrip_codes=1024 negatives=%u gpu=0 gameplay=0\n",pixels,negatives);
}
