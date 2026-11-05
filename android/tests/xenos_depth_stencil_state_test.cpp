#include "xenos_depth_stencil_state.h"
#include <cassert>
#include <initializer_list>

int main() {
    using namespace recomp::gpu;
    const auto state=decodeXenosDepthStencilState(0x00700263,0x0000FFFF,0x00FFFF00);
    assert(state && state->depthEnable && !state->depthWrite && state->depthCompare==6);
    assert(state->stencilEnable && !state->backfaceEnable && state->front==state->back);
    assert(state->front.compare==2 && state->front.reference==255 && state->front.readMask==255 && !state->front.writeMask);
    for(bool back:{false,true}) {
        const auto result=evaluateXenosDepthStencil(*state,1,0,255,back);
        assert(result.colorPass && !result.depthWritten && result.stencil==255);
        assert(!evaluateXenosDepthStencil(*state,1,0,254,back).colorPass);
    }
    for(unsigned value=0;value<256;++value) {
        assert(applyXenosStencilOperation(0,37,value)==value);
        assert(applyXenosStencilOperation(1,37,value)==0);
        assert(applyXenosStencilOperation(2,37,value)==37);
        assert(applyXenosStencilOperation(3,37,value)==(value<255?value+1:255));
        assert(applyXenosStencilOperation(4,37,value)==(value?value-1:0));
        assert(applyXenosStencilOperation(5,37,value)==255-value);
        assert(applyXenosStencilOperation(6,37,value)==(value+1)%256);
        assert(applyXenosStencilOperation(7,37,value)==(value+255)%256);
    }
    const bool less[]{false,true,false,true,false,true,false,true};
    const bool equal[]{false,false,true,true,false,false,true,true};
    const bool greater[]{false,false,false,false,true,true,true,true};
    for(unsigned f=0;f<8;++f) {
        assert(xenosCompare(f,1,2)==less[f]);assert(xenosCompare(f,2,2)==equal[f]);assert(xenosCompare(f,3,2)==greater[f]);
    }
    // Separate back state, z-pass/z-fail/stencil-fail selection, masked writes.
    for(unsigned operation=0;operation<8;++operation)for(unsigned mask:{0u,0x0Fu,0xF0u,255u}) {
        auto s=*state;s.depthWrite=true;s.depthCompare=1;s.backfaceEnable=true;
        s.front={7,2,operation,5,37,255,mask};s.back={0,2,0,0,73,255,255};
        const auto pass=evaluateXenosDepthStencil(s,.25,.5,15,false);
        assert(pass.colorPass && pass.depthWritten);
        assert(pass.stencil==((15&~mask)|(applyXenosStencilOperation(operation,37,15)&mask)));
        const auto fail=evaluateXenosDepthStencil(s,.75,.5,15,false);
        assert(!fail.colorPass && !fail.depthWritten && fail.stencil==((15&~mask)|(240&mask)));
        const auto back=evaluateXenosDepthStencil(s,.25,.5,15,true);
        assert(!back.colorPass && !back.depthWritten && back.stencil==73);
    }
    assert(!decodeXenosDepthStencilState(8,0,0));
    assert(!decodeXenosDepthStencilState(0,0x01000000,0));
    assert(!decodeXenosDepthStencilState(0,0,0x01000000));
}
