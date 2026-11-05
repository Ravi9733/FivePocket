#include "gta5_color8_clear_plan.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
using Bytes=std::vector<std::uint8_t>;
void word(Bytes& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);}
unsigned bits(float f){unsigned u;std::memcpy(&u,&f,4);return u;}
Gta5ClearCapture fixture(unsigned samples=2,unsigned format=1,unsigned width=128,unsigned height=128,unsigned pitch=160) {
    Gta5ClearCapture c;c.call.flags=0x3F;c.state=Bytes(13864);c.rectangle=Bytes(16);c.color=Bytes(16);
    c.surfaces[0].object=0x10274D10;c.surfaces[0].header=Bytes(40);
    const unsigned info=0x02000000|((samples==4?2:samples==2?1:0)<<16)|pitch,color=format<<16;
    word(*c.state,12816,c.surfaces[0].object);word(*c.state,10368,info);word(*c.state,10372,color);
    word(*c.surfaces[0].header,24,info);word(*c.surfaces[0].header,28,color);
    word(*c.surfaces[0].header,36,((width-1)<<18)|((height-1)<<3));
    word(*c.state,13032,bits(float(width)));word(*c.state,13036,bits(float(height)));
    word(*c.rectangle,8,width);word(*c.rectangle,12,height);word(*c.color,12,bits(1));
    word(*c.state,12288,1);word(*c.state,13060,width);word(*c.state,13064,height);
    return c;
}
int main() {
    for(unsigned samples:{1u,2u,4u})for(unsigned format:{0u,1u}) {
        auto c=fixture(samples,format);auto a=auditGta5Color8Clear(c);assert(!a.issues);
        assert(a.plan.samples==samples && a.plan.format==format && a.effectiveFlags==15);
        assert(a.plan.width==128 && a.plan.height==128 && a.plan.tileCount==16*samples);
        assert((a.plan.color==std::array<std::uint8_t,4>{0,0,0,255}));
    }
    auto c=fixture();const auto baseline=c;
    c.state.reset();c.color.reset();c.rectangle.reset();c.surfaces[0].header.reset();
    const auto missing=auditGta5Color8Clear(c);
    assert((missing.issues&(ClearStateMissing|ClearColorUnsupported|ClearRectangleMissing|ClearHeaderMissing))==
        (ClearStateMissing|ClearColorUnsupported|ClearRectangleMissing|ClearHeaderMissing));
    c=baseline;word(*c.state,12832,0x10001000);c.surfaces[4].object=0x10001000;
    assert(auditGta5Color8Clear(c).issues&ClearDepthStencilUnsupported);
    c.call.flags=15;assert(!auditGta5Color8Clear(c).issues); // Unrequested depth is not silently cleared.
    c=baseline;word(*c.state,12820,0x10002000);c.surfaces[1].object=0x10002000;
    assert(auditGta5Color8Clear(c).issues&ClearMultipleTargets);
    c=baseline;c.state->at(11068)=0x10;assert(auditGta5Color8Clear(c).issues&ClearBinned);
    c.state->at(11068)=0x20;assert(!auditGta5Color8Clear(c).issues);
    word(*c.state,13104,c.surfaces[0].object);c.surfaces[0].tileCache=c.surfaces[0].object;
    assert(auditGta5Color8Clear(c).issues&ClearBinned);
    c=baseline;word(*c.state,11068,0x42400);assert(!auditGta5Color8Clear(c).issues);
    c=baseline;word(*c.state,13052,1);assert(auditGta5Color8Clear(c).issues&ClearPartial);
    c=baseline;word(*c.rectangle,8,127);assert(auditGta5Color8Clear(c).issues&ClearPartial);
    c=baseline;word(*c.rectangle,0,UINT32_MAX);assert(!auditGta5Color8Clear(c).issues);
    c=baseline;word(*c.state,13024,bits(.75f));assert(!auditGta5Color8Clear(c).issues); // fctiwz truncation.
    for(float bad:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),9000.f}) {
        c=baseline;word(*c.state,13024,bits(bad));assert(auditGta5Color8Clear(c).issues&ClearViewportUnsupported);
        c=baseline;word(*c.color,0,bits(bad));assert(auditGta5Color8Clear(c).issues&ClearColorUnsupported);
    }
    c=baseline;c.call.flags|=64;assert(auditGta5Color8Clear(c).issues&ClearFlagsUnsupported);
    c=baseline;word(*c.surfaces[0].header,24,0x20300A0);assert(auditGta5Color8Clear(c).issues&ClearSurfaceUnsupported);
    c=baseline;word(*c.surfaces[0].header,28,0x1007FF);assert(auditGta5Color8Clear(c).issues&ClearEdramRange);
    c=baseline;c.surfaces[0].header->pop_back();assert(auditGta5Color8Clear(c).issues&ClearHeaderMissing);
    c=baseline;c.surfaces[0].object+=4;assert(auditGta5Color8Clear(c).issues&ClearBindingMismatch);
    c=baseline;c.surfaces[4].object=0x10274D50;c.surfaces[4].header=*c.surfaces[0].header;
    word(*c.state,12832,0x10274D50);word(*c.state,10376,0x10020);word(*c.surfaces[4].header,28,0x10020);
    c.call.depthBits=0;c.call.stencil=255;
    auto coupled=auditGta5Color8Clear(c);assert(!coupled.issues && coupled.plan.depth);
    assert(coupled.plan.depth->value.encoded==0 && coupled.plan.depth->value.stencil==255);
    assert(coupled.plan.depth->tileCount==32 && coupled.plan.depth->baseTile==32);
    assert(color8ClearIntersects(coupled.plan,0,1) && color8ClearIntersects(coupled.plan,32,1));
    assert(!color8ClearIntersects(coupled.plan,64,1) && !color8ClearIntersects(coupled.plan,0,0));
    auto overlapping=coupled.plan;overlapping.baseTile=64;overlapping.depth->baseTile=16;
    assert(color8ClearPlansOverlap(coupled.plan,overlapping));overlapping.depth->baseTile=96;
    assert(!color8ClearPlansOverlap(coupled.plan,overlapping));
    const auto full=c;
    c.call.flags=0x1F;assert(auditGta5Color8Clear(c).issues&ClearDepthStencilUnsupported);
    c=full;c.call.stencil=256;assert(auditGta5Color8Clear(c).issues&ClearDepthValueUnsupported);
    c=full;double noncanonical=.1;std::memcpy(&c.call.depthBits,&noncanonical,8);
    assert(auditGta5Color8Clear(c).issues&ClearDepthValueUnsupported);
    c=full;word(*c.surfaces[4].header,28,0x10010);word(*c.state,10376,0x10010);
    assert(auditGta5Color8Clear(c).issues&ClearDepthColorAlias);
    c=full;c.surfaces[4].header->pop_back();assert(auditGta5Color8Clear(c).issues&ClearDepthHeaderMissing);
    c=full;word(*c.surfaces[4].header,36,(63<<18)|(127<<3));assert(auditGta5Color8Clear(c).issues&ClearDepthExtentMismatch);
    std::puts("COLOR8_CLEAR_PLAN_PASS samples1/2/4,UNORM/PWL,dimensions,clipping,depth flags,binning,multiple independent issues");
}
