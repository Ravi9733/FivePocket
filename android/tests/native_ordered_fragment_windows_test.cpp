#include "native_ordered_fragment_windows.h"
#include <cassert>
#include <cstdio>
#include <string_view>
using namespace recomp::gpu;
int main() {
    std::vector<unsigned> indices(72);for(unsigned i=0;i<72;++i)indices[i]=i+9;
    unsigned calls=0;std::array<unsigned,720> rowsSeen{};
    const auto good=[&](auto part,unsigned row,unsigned rows) {
        assert(part.size()==3 && part[0]==9+3*(calls/15));++calls;
        assert(rows && rows<=OrderedWindowRows && rows*1280<=OrderedFragment7e3MaxCapacity);
        if(calls<=15)for(unsigned y=row;y<row+rows;++y)++rowsSeen[y];
        return std::optional<NativeOrderedFragment7e3Batch>{{1,{{0,1280*row,{{.5f,.5f,.5f,.5f},.5f}}}}};
    };
    auto result=executeNativeOrderedTriangleWindows(indices,good);
    assert(result && calls==360 && result->primitiveCount==24 && result->fragments.size()==360);
    for(unsigned n:rowsSeen)assert(n==1);
    for(unsigned i=0;i<360;++i)assert(result->fragments[i].primitive==i/15);
    for(unsigned mode=0;mode<5;++mode) {
        calls=0;
        assert(!executeNativeOrderedTriangleWindows(indices,[&](auto part,unsigned row,unsigned rows) {
            auto b=good(part,row,rows);if(calls!=360)return b;
            if(mode==0)b.reset();
            if(mode==1)b->primitiveCount=2;
            if(mode==2)b->fragments[0].primitive=1;
            if(mode==3)b->fragments[0].pixel=0;
            if(mode==4)b->fragments.push_back(b->fragments[0]);
            return b;
        }));assert(calls==360);
    }
    const char* why=nullptr;calls=0;
    assert(!executeNativeOrderedTriangleWindows(indices,good,&why,2));
    assert(calls==3 && std::string_view(why)=="ordered-windows-mesh-budget");
    for(unsigned count:{0u,1u,71u,3*OrderedMeshMaxTriangles+3}) {
        std::vector<unsigned> invalid(count);calls=0;
        assert(!executeNativeOrderedTriangleWindows(invalid,good) && calls==0);
    }
    for(unsigned count:{3u,12u,36u,72u,192u,3*OrderedMeshMaxTriangles}) {
        std::vector<unsigned> mesh(count);unsigned seen=0;
        const auto batch=executeNativeOrderedTriangleWindows(mesh,[&](auto,unsigned,unsigned) {
            ++seen;return std::optional<NativeOrderedFragment7e3Batch>{{1,{}}};
        });assert(batch && batch->primitiveCount==count/3 && seen==count/3*15);
    }
    NativeEdram32Bands owner;std::vector<unsigned> depth(1280*720,91);
    std::vector<std::uint8_t> color(1280*720*4,0);
    assert(owner.recordCompletedDepthWords(0,1280,1280,720,depth,1));
    assert(owner.recordCompletedRawColorRows(720,1280,1280,720,color,2));calls=0;
    {NativeEdram32Mutation mutation(owner);
        assert(!executeNativeOrderedFragment7e3Physical(mutation,[&] {
            return executeNativeOrderedTriangleWindows(indices,[&](auto part,unsigned row,unsigned rows) {
                auto b=good(part,row,rows);if(calls==360)b.reset();return b;
            });
        }));}
    assert(calls==360 && !owner.knownTiles());
    std::puts("ORDERED_WINDOWS_PASS row_partition=720 triangles=24 windows=360 bounded_growth=1 late_failure_atomic=1 size_classes=6 no_admission=1");
}
