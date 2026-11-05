#include "native_ordered_triangle_batches.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    std::vector<unsigned> indices(36);for(unsigned i=0;i<36;++i)indices[i]=i+17;
    unsigned calls=0;
    const auto make=[&](std::span<const unsigned> part) {
        assert(part.size()==3 && part[0]==17+3*calls && part[2]==19+3*calls);++calls;
        return std::optional<NativeOrderedFragment7e3Batch>{{1,{{0,91,{{.5f,.5f,.5f,.5f},.5f}}}}};
    };
    const auto result=executeNativeOrderedTriangleBatches(indices,make);
    assert(result && result->primitiveCount==12 && result->fragments.size()==12 && calls==12);
    for(unsigned i=0;i<12;++i)assert(result->fragments[i].primitive==i && result->fragments[i].pixel==91);
    for(unsigned mode=0;mode<5;++mode) {
        calls=0;
        const auto broken=executeNativeOrderedTriangleBatches(indices,[&](auto part) {
            auto b=make(part);if(calls!=12)return b;
            if(mode==0)return std::optional<NativeOrderedFragment7e3Batch>{};
            if(mode==1)b->primitiveCount=2;
            if(mode==2)b->fragments[0].primitive=1;
            if(mode==3)b->fragments[0].pixel=1280*720;
            if(mode==4)b->fragments.push_back(b->fragments[0]);
            return b;
        });assert(!broken && calls==12);
    }
    for(unsigned count:{0u,1u,35u,37u,39u}) {
        std::vector<unsigned> bad(count);calls=0;
        assert(!executeNativeOrderedTriangleBatches(bad,make) && !calls);
    }
    NativeEdram32Bands owner;std::vector<unsigned> depth(1280*720,91);
    std::vector<std::uint8_t> color(1280*720*4,0);
    assert(owner.recordCompletedDepthWords(0,1280,1280,720,depth,1));
    assert(owner.recordCompletedRawColorRows(720,1280,1280,720,color,2));
    calls=0;
    {NativeEdram32Mutation mutation(owner);
        assert(!executeNativeOrderedFragment7e3Physical(mutation,[&] {
            return executeNativeOrderedTriangleBatches(indices,[&](auto part) {
                auto b=make(part);if(calls==12)b.reset();return b;
            });
        }));}
    assert(calls==12 && !owner.knownTiles());
    std::puts("ORDERED_TRIANGLE_BATCHES_PASS triangles=12 exact_indices=1 remapped_order=1 negatives=10 late_failure_invalidates=1 partial_publication=0");
}
