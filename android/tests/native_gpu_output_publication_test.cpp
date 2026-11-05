#include "native_gpu_output_publication.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    ColorCopyPlan copyPlan;
    assert(planColorCopy({0x14000500,0xC02D0,1280,720,false},0x100000,0x1F2D8000,0x2D00500,0x1000300,copyPlan));
    std::vector<std::uint8_t> tiled(copyPlan.capacity+32,0x5A),mirror(copyPlan.capacity+32),expected;
    for (std::size_t i=0;i<mirror.size();++i) mirror[i]=static_cast<std::uint8_t>(i%251);
    expected=mirror;
    for (unsigned y=0;y<720;++y) for (unsigned x=0;x<1280;++x) {
        std::uint32_t offset=0;assert(colorCopyOffset(1280,720,x,y,offset));
        for (unsigned lane=0;lane<4;++lane) tiled[16+offset+lane]=expected[16+offset+lane]=static_cast<std::uint8_t>((x+y*7+lane)%256);
    }
    auto captured=NativeTiledCopyPublication::capture(copyPlan,tiled.data(),tiled.size());assert(captured);
    const auto oldMirror=mirror,oldTiled=tiled;
    NativeWordUpdatePlan plan;
    const NativeWordRequest requests[]={{16,0x11111111,NativeWordPhase::BeforeGraphics},
        {16,0xA1B2C3D4,NativeWordPhase::AfterGraphics},{20,0x10203040,NativeWordPhase::AfterGraphics}};
    assert(planNativeWordUpdates(requests,3,40,plan));
    std::array<std::uint8_t,40> readback;readback.fill(0x5A);
    for (const auto& w:plan.words) if (w.byteOffset) std::memcpy(readback.data()+w.byteOffset,w.bigEndianBytes.data(),4);
    const auto oldReadback=readback;
    std::array<std::uint8_t,12> events;events.fill(0xCD);
    std::array<std::uint8_t,4> rptr{{0,0,0,34}};
    // Deliberately reverse staging order: each destination receives its OWN
    // audited last value, not readback[i*4] or host-endian guessed event words.
    const std::array<NativeOutputWord,2> words{{{20,{0x1000,events.data(),4}},{16,{0x2000,events.data()+4,4}}}};
    const NativeOutputSpan cursor{0x3000,rptr.data(),4},copy{copyPlan.destinationBase,mirror.data()+16,copyPlan.capacity};
    const auto unchanged=[&] {
        assert(mirror==oldMirror && tiled==oldTiled && readback==oldReadback);
        for (auto b:events) assert(b==0xCD);
        assert((rptr==std::array<std::uint8_t,4>{0,0,0,34}));
    };
    auto publish=[&](const auto& p,const auto* mapping,std::size_t count,const auto& rp,const auto& cp) {
        return publishNativeGpuOutputs(p,readback.data(),readback.size(),mapping,count,rp,34,37,&*captured,cp);
    };
    for (unsigned test=0;test<24;++test) {
        auto bad=words;auto rp=cursor,cp=copy;auto p=plan;
        const NativeOutputWord* mapping=bad.data();std::size_t count=2;
        switch (test) {
        case 0: count=1;break;
        case 1: count=65;break;
        case 2: mapping=nullptr;break;
        case 3: bad[1].stagingByteOffset=20;break;
        case 4: bad[1].stagingByteOffset=24;break;
        case 5: bad[1].target.physicalAddress=bad[0].target.physicalAddress;break;
        case 6: bad[1].target.bytes=bad[0].target.bytes;break;
        case 7: bad[1].target.bytes=bad[0].target.bytes+3;break;
        case 8: bad[1].target.physicalAddress=cursor.physicalAddress;break;
        case 9: bad[1].target.bytes=rptr.data();break;
        case 10: bad[1].target.physicalAddress=copy.physicalAddress;break;
        case 11: bad[1].target.physicalAddress=copy.physicalAddress+static_cast<std::uint32_t>(copy.size)-4;break;
        case 12: bad[1].target.bytes=copy.bytes+copy.size-4;break;
        case 13: rp.physicalAddress=copy.physicalAddress;break;
        case 14: rp.bytes=copy.bytes;break;
        case 15: cp.physicalAddress+=4;break;
        case 16: cp.size-=4;break;
        case 17: cp.bytes=nullptr;break;
        case 18: bad[1].target.size=8;break;
        case 19: bad[1].target.physicalAddress=0x20000000;break;
        case 20: bad[1].target.physicalAddress|=1;break;
        case 21: bad[1].target.bytes=readback.data()+24;break;
        case 22: p.words[2].phase=NativeWordPhase::BeforeGraphics;break;
        case 23: p.count=65;break;
        }
        assert(!publish(p,mapping,count,rp,cp));unchanged();
    }
    assert(!publishNativeGpuOutputs(plan,nullptr,40,words.data(),2,cursor,34,37,&*captured,copy));unchanged();
    assert(!publishNativeGpuOutputs(plan,readback.data(),39,words.data(),2,cursor,34,37,&*captured,copy));unchanged();
    for (auto next:{0u,33u,34u}) {
        assert(!publishNativeGpuOutputs(plan,readback.data(),40,words.data(),2,cursor,34,next,&*captured,copy));unchanged();
    }
    for (auto previous:{0u,33u,35u}) {
        assert(!publishNativeGpuOutputs(plan,readback.data(),40,words.data(),2,cursor,previous,37,&*captured,copy));unchanged();
    }
    // A late mismatching event leaves even the otherwise-valid large copy alone.
    readback[23]^=1;
    assert(!publish(plan,words.data(),2,cursor,copy));readback=oldReadback;unchanged();
    assert(!publishNativeGpuOutputs(plan,readback.data(),40,words.data(),2,cursor,34,37,nullptr,copy));unchanged();
    assert(publish(plan,words.data(),2,cursor,copy));
    assert(mirror==expected && readback==oldReadback && tiled==oldTiled);
    assert((events==std::array<std::uint8_t,12>{0x10,0x20,0x30,0x40,0xA1,0xB2,0xC3,0xD4,0xCD,0xCD,0xCD,0xCD}));
    assert((rptr==std::array<std::uint8_t,4>{0,0,0,37}));
    assert(!publish(plan,words.data(),2,cursor,copy)); // No replay of retired cursor.
    assert(mirror==expected && rptr[3]==37);
    // Existing no-copy production path and zero-word state-only batches remain supported.
    mirror=oldMirror;events.fill(0xCD);rptr[3]=34;
    assert(publishNativeGpuOutputs(plan,readback.data(),40,words.data(),2,cursor,34,37));
    assert(mirror==oldMirror && events[0]==0x10 && events[4]==0xA1 && rptr[3]==37);
    NativeWordUpdatePlan empty;assert(planNativeWordUpdates(nullptr,0,40,empty));
    assert(publishNativeGpuOutputs(empty,readback.data(),40,nullptr,0,cursor,37,40));
    assert(rptr[3]==40);
    assert(publishNativeGpuOutputs(empty,readback.data(),40,nullptr,0,cursor,40,43,&*captured,copy));
    assert(mirror==expected && rptr[3]==43);
    std::puts("NATIVE_GPU_OUTPUT_PUBLICATION_V1 copy_events_cursor=1 failure_atomic=1 scratch_only=1 fence_proved_by_helper=0");
}
