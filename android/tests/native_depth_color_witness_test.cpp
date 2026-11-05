#include "native_depth_color_witness.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    const std::array<unsigned,4> full{15,15,15,15},replace{0x10001,0x10001,0x10001,0x10001};
    XenosDepthStencilState producer;producer.depthEnable=producer.depthWrite=true;producer.depthCompare=6;
    auto consumer=producer;consumer.depthWrite=false;
    NativeDepthColorWitness w;assert(!w.permits(0,15,consumer));
    w.cleared(1,1,0x10,0);assert(w.permits(0,15,consumer) && !w.permits(1,15,consumer));
    w.drew(0,producer,15,full,replace);assert(w.permits(0,15,consumer));
    auto masks=full;masks[1]=7;w.drew(0,producer,15,masks,replace);
    assert(w.permits(0,13,consumer) && !w.permits(0,2,consumer));
    w.cleared(1,0,0x10,0);assert(!w.permits(0,2,consumer)); // Partial zero clear cannot repair entire tile.
    w.cleared(1,1,0x10,0);assert(w.permits(0,15,consumer));
    w.drew(0,consumer,0,{},replace);assert(w.permits(0,15,consumer)); // No depth writes.
    w.cleared(1,0,1,1);assert(w.permits(0,15,consumer)); // Color-only changes never invalidate.
    w.cleared(1,0,0x10,.5f);assert(!w.permits(0,1,consumer));
    w.cleared(1,1,0x10,0);w.carried(0,1);assert(w.permits(1,15,consumer));
    w.carried(2,1);assert(!w.permits(1,1,consumer)); // Undefined source cannot gain proof through carry.
    for(unsigned compare=0;compare<8;++compare) {
        auto d=consumer;d.depthCompare=compare;assert(w.permits(0,15,d)==(compare==6));
    }
    auto d=consumer;d.stencilEnable=true;assert(!w.permits(0,1,d));
    assert(!w.permits(0,1,producer));d=consumer;d.depthEnable=false;assert(!w.permits(0,1,d));
    assert(!w.permits(0,0,consumer) && !w.permits(32,1,consumer) && !w.permits(0,16,consumer));
    for(unsigned slot=0;slot<4;++slot)for(unsigned mask=0;mask<16;++mask)for(unsigned output=0;output<16;++output) {
        NativeDepthColorWitness proof;proof.cleared(1,1,0x10,0);auto m=full;m[slot]=mask;
        proof.drew(0,producer,output,m,replace);
        assert(proof.permits(0,1u<<slot,consumer)==(mask==15 && (output&(1u<<slot))));
    }
    // Connected state oracle: partial rasterization, discarded fragments,
    // depth pass/fail, masked color/depth writers and partial/full depth clears.
    unsigned rng=793;unsigned observations=0;
    for(unsigned trial=0;trial<64;++trial) {
        NativeDepthColorWitness proof;std::array<float,8> depth{};std::array<unsigned,8> valid{};
        proof.cleared(1,1,0x10,0);
        for(unsigned step=0;step<128;++step) {
            const auto next=[&](){rng=rng*1664525u+1013904223u;return rng;};
            if(step%19==0) {
                const bool fullClear=next()&1;const float z=next()&1?0.f:.25f;
                proof.cleared(1,fullClear?1:0,0x10,z);
                for(unsigned i=0;i<(fullClear?8u:4u);++i)depth[i]=z;
            } else {
                auto state=producer;state.depthCompare=next()%8;state.depthWrite=next()&1;
                const unsigned outputs=next()&15;auto m=full;for(auto&v:m)v=next()&15;
                for(unsigned i=0;i<8;++i) {
                    if(next()&1)continue; // Not covered / discarded.
                    const float z=float(next()%5)/4;const auto result=evaluateXenosDepthStencil(state,z,depth[i],0,false);
                    if(result.colorPass)for(unsigned s=0;s<4;++s)if(outputs&(1u<<s))valid[i]|=m[s]<<(4*s);
                    if(result.depthWritten)depth[i]=z;
                }
                proof.drew(0,state,outputs,m,replace);
            }
            for(unsigned s=0;s<4;++s)if(proof.permits(0,1u<<s,consumer))for(unsigned i=0;i<8;++i) {
                assert(depth[i]==0 || ((valid[i]>>(4*s))&15)==15);++observations;
            }
        }
    }
    std::printf("DEPTH_COLOR_WITNESS_HOST_PASS connected_observations=%u gameplay=0\n",observations);
}
