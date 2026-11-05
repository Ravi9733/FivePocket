#include "native_ordered_fragment7e3_physical.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <stdexcept>
using namespace recomp::gpu;
int main() {
    constexpr unsigned pixels=1280*720;
    unsigned original=0;assert(packColor7e3({colorBits(.25f),colorBits(.5f),colorBits(1),colorBits(1)},original));
    std::vector<std::uint8_t> bytes(pixels*4);
    std::vector<unsigned> depths(pixels);
    for(unsigned p=0;p<pixels;++p){depths[p]=p&255;for(unsigned c=0;c<4;++c)bytes[4*p+c]=original>>(8*c);}
    const auto seed=[&] {
        NativeEdram32Bands owner;
        assert(owner.recordCompletedRawColorRows(720,1280,1280,720,bytes,1));
        assert(owner.recordCompletedDepthWords(0,1280,1280,720,depths,2));
        assert(owner.recordCompletedDepthWords(1500,80,80,16,std::vector<unsigned>(1280,0xABCDEF12),3));
        return owner;
    };
    const auto makeBatch=[] {
        return NativeOrderedFragment7e3Batch{3,{
            {0,91,{{.5f,1,2,.5f},.75f}},
            {0,200,{{1,1,1,0},1}}, // Alpha failure must not write depth.
            {1,91,{{8,8,8,1},.5f}}, // Overlap behind the first write.
            {2,91,{{.25f,.5f,1,.5f},1}}}};
    };
    auto owner=seed();const auto unrelated=owner.readDepthWords(1500,80,80,16);
    {
        NativeEdram32Mutation mutation(owner);
        assert(executeNativeOrderedFragment7e3Physical(mutation,[&] {
            assert(!owner.knownTiles());return std::optional{makeBatch()};
        }));
    }
    const auto color=owner.readColorWords(720,1280,1280,720),depth=owner.readDepthWords(0,1280,1280,720);
    assert(color && depth && owner.readDepthWords(1500,80,80,16)==unrelated);
    unsigned expected=0;assert(packColor7e3({colorBits(.3125f),colorBits(.625f),colorBits(1.25f),colorBits(7.f/12.f)},expected));
    for(unsigned p=0;p<pixels;++p) {
        assert(color->at(p)==(p==91?expected:original));
        assert(depth->at(p)==(p==91?0xF000005Bu:depths[p]));
    }
    for(unsigned kind=0;kind<11;++kind) {
        owner=seed();unsigned called=0;const char* why=nullptr;
        if(kind==0)assert(owner.invalidate(720,1));
        if(kind==1)assert(owner.invalidate(0,1));
        {
            NativeEdram32Mutation mutation(owner);
            assert(!executeNativeOrderedFragment7e3Physical(mutation,[&]()->std::optional<NativeOrderedFragment7e3Batch> {
                ++called;auto b=makeBatch();
                if(kind==2)return std::nullopt;
                if(kind==3)b.primitiveCount=0;
                if(kind==4)b.primitiveCount=65536;
                if(kind==5)b.fragments[1]=b.fragments[0];
                if(kind==6)std::swap(b.fragments[0],b.fragments[1]);
                if(kind==7)b.fragments[0].pixel=pixels;
                if(kind==8)b.fragments[0].primitive=b.primitiveCount;
                if(kind==9)b.fragments.back().sample.depth=2;
                if(kind==10)b.fragments.back().sample.color[1]=std::numeric_limits<float>::infinity();
                return b;
            },&why));
        }
        assert(why && !owner.knownTiles() && called==(kind<2?0u:1u));
        assert(!owner.readColorWords(720,1280,1280,720) && !owner.readDepthWords(0,1280,1280,720));
    }
    owner=seed();
    try {NativeEdram32Mutation mutation(owner);
        executeNativeOrderedFragment7e3Physical(mutation,[]()->std::optional<NativeOrderedFragment7e3Batch>{throw std::runtime_error("fence failed");});
        assert(false);
    } catch(const std::runtime_error&) {assert(!owner.knownTiles());}
    // A completed empty raster preserves exact raw bytes, not a re-packed
    // float round-trip, and does not invent a clear.
    owner=seed();
    {NativeEdram32Mutation mutation(owner);assert(executeNativeOrderedFragment7e3Physical(mutation,[] {
        return std::optional{NativeOrderedFragment7e3Batch{4,{}}};}));}
    assert(owner.readDepthWords(0,1280,1280,720)==depths);
    const auto unchanged=owner.readColorWords(720,1280,1280,720);assert(unchanged);
    for(auto value:*unchanged)assert(value==original);
    std::puts("ORDERED_FRAGMENT_PHYSICAL_PASS paired_color_depth=1 overlapping_primitives=1 untouched_preserved=1 failures_invalidate=12 query_completion=0 live_draw=0");
}
