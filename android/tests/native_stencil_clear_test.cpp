#include "native_stencil_clear.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
static void put(std::vector<std::uint8_t>& b,unsigned at,unsigned v) {
    for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);
}
static Gta5ClearCapture fixture() {
    Gta5ClearCapture c;c.call.flags=0xA0;c.call.stencil=0;c.state.emplace(13864);c.rectangle.emplace(16);
    auto& b=*c.state;c.surfaces[4].object=0x10004000;c.surfaces[4].header.emplace(40);
    put(b,12832,c.surfaces[4].object);put(b,10368,0x14000500);put(b,10376,0x10000);
    put(*c.surfaces[4].header,24,0x14000500);put(*c.surfaces[4].header,28,0x10000);
    put(*c.surfaces[4].header,36,0x13FC1678);put(b,13032,0x44A00000);put(b,13036,0x44340000);
    put(b,12288,1);put(b,13060,1280);put(b,13064,384);put(*c.rectangle,8,1280);put(*c.rectangle,12,720);return c;
}
int main() {
    auto c=fixture();const auto plan=planNativeStencilClear(c);assert(plan && plan->bottom==384);
    constexpr unsigned count=1280*720;std::vector<unsigned> before(count);
    for(unsigned i=0;i<count;++i)before[i]=(i*0x10203u)^0x97A35BFFu;
    NativeEdram32Bands original;assert(original.recordCompletedDepthWords(0,1280,1280,720,before,1));
    std::vector<std::uint8_t> color(count*4,0xAB);assert(original.recordCompletedRawColorRows(720,1280,1280,720,color,2));
    for(unsigned reference:{0u,0x5Au,255u}) {
        c.call.stencil=reference;auto owner=original;
        {NativeEdram32Mutation m(owner);assert(executeNativeStencilClear(c,m,[]{return true;}));}
        auto after=owner.readDepthWords(0,1280,1280,720);assert(after);
        for(unsigned i=0;i<count;++i)assert((*after)[i]==(i<1280*384?((before[i]&0xFFFFFF00)|reference):before[i]));
        assert(owner.readColorWords(720,1280,1280,720)==original.readColorWords(720,1280,1280,720));
        assert(owner.nextSerial()==original.nextSerial()+1);
    }
    // Exercise signed intersection, non-tile-aligned edges, and viewport cap.
    put(*c.rectangle,0,UINT32_MAX);put(*c.rectangle,4,7);put(*c.rectangle,8,1300);
    put(*c.state,13052,17);put(*c.state,13060,1243);put(*c.state,13064,379);
    auto owner=original;{NativeEdram32Mutation m(owner);assert(executeNativeStencilClear(c,m,[]{return true;}));}
    auto after=owner.readDepthWords(0,1280,1280,720);assert(after);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){auto i=y*1280+x;
        assert((*after)[i]==((x>=17 && x<1243 && y>=7 && y<379)?((before[i]&0xFFFFFF00)|255):before[i]));}
    for(unsigned mode=0;mode<12;++mode) {
        auto bad=fixture();owner=original;
        if(mode==0)bad.call.flags=0xB0;if(mode==1)bad.call.stencil=256;
        if(mode==2)bad.surfaces[4].header.reset();if(mode==3)put(*bad.state,10376,0);
        if(mode==4)(*bad.state)[11068]=0x10;if(mode==5)put(*bad.state,13032,0x7FC00000);
        if(mode==6)put(*bad.rectangle,8,0);if(mode==7)bad.surfaces[4].object+=4;
        if(mode==8)assert(owner.invalidate(1,1));
        if(mode==10){(*bad.state)[11068]=0x20;bad.surfaces[4].tileCache=bad.surfaces[4].object;put(*bad.state,13120,bad.surfaces[4].object);}
        if(mode==11)put(*bad.surfaces[4].header,36,0);
        {NativeEdram32Mutation m(owner);assert(!executeNativeStencilClear(bad,m,[&]{return mode!=9;}));}
        assert(!owner.knownTiles());
    }
    assert(sameNativeStencilClearInputs(c,c));auto stale=c;(*stale.state)[123]^=1;
    assert(!sameNativeStencilClearInputs(c,stale));stale=c;stale.call.stencil^=1;assert(!sameNativeStencilClearInputs(c,stale));
    std::puts("NATIVE_STENCIL_CLEAR_PASS aspects_preserved=1 clipped_edges=1 unknown_stale_refused=1 authored_owner=1");
}
