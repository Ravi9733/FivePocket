#include "native_tiled_copy_publication.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    ColorCopyPlan plan;
    assert(planColorCopy({0x14000500,0xC02D0,1280,720,false},0x100000,0x1F2D8000,0x2D00500,0x1000300,plan));
    std::vector<std::uint8_t> readback(plan.capacity+32,0x5A),mask(plan.capacity/4),destination(plan.capacity+32),expected;
    for (std::size_t i=0;i<destination.size();++i) destination[i]=static_cast<std::uint8_t>(i%251);
    expected=destination;
    for (unsigned y=0;y<720;++y) for (unsigned x=0;x<1280;++x) {
        std::uint32_t offset=0;assert(colorCopyOffset(1280,720,x,y,offset));mask[offset/4]=1;
        for (unsigned c=0;c<4;++c) readback[16+offset+c]=expected[16+offset+c]=static_cast<std::uint8_t>((x*7+y*13+c)%256);
    }
    const auto original=readback,before=destination;
    auto captured=NativeTiledCopyPublication::capture(plan,readback.data(),readback.size());assert(captured);
    assert(captured->capacity()==plan.capacity && captured->pixelBytes()==3686400);
    assert(captured->matchesPlan(plan));unsigned planRejections=0;
    for(unsigned field=0;field<8;++field)for(unsigned bit=0;bit<32;++bit){
        auto changed=plan;std::uint32_t* fields[]={&changed.source.surface,&changed.source.info,
            &changed.source.width,&changed.source.height,&changed.destinationBase,&changed.capacity,&changed.touchedSpan,&changed.pixelBytes};
        *fields[field]^=1u<<bit;assert(!captured->matchesPlan(changed));++planRejections;
    }
    auto changed=plan;changed.source.depth=!changed.source.depth;assert(!captured->matchesPlan(changed));++planRejections;
    changed=plan;changed.swapRedBlue=!changed.swapRedBlue;assert(!captured->matchesPlan(changed));++planRejections;
    ColorCopyPlan other;
    assert(planColorCopy({0x14000500,0xC02D1,1280,720,false},0x100000,plan.destinationBase,0x2D00500,0x300,other));
    assert(other.capacity==plan.capacity && other.pixelBytes==plan.pixelBytes && !captured->matchesPlan(other));
    auto otherCopy=NativeTiledCopyPublication::capture(other,readback.data(),readback.size());
    assert(otherCopy && otherCopy->matchesPlan(other) && !otherCopy->matchesPlan(plan));
    // Reject invalid publication targets without partial output; then prove
    // the snapshot survives changes to its original mapped/staging input.
    assert(!captured->publishToOwnedMemory(plan.destinationBase+4,destination.data()+16,plan.capacity));
    assert(!captured->publishToOwnedMemory(plan.destinationBase,destination.data()+16,plan.capacity-4));
    assert(!captured->publishToOwnedMemory(plan.destinationBase,nullptr,plan.capacity));
    assert(destination==before && readback==original);
    assert(!captured->matchesPublishedOwnedMemory(plan.destinationBase,destination.data()+16,plan.capacity));
    std::fill(readback.begin(),readback.end(),0xCD);
    assert(captured->publishToOwnedMemory(plan.destinationBase,destination.data()+16,plan.capacity));
    assert(destination==expected);
    assert(captured->matchesPublishedOwnedMemory(plan.destinationBase,destination.data()+16,plan.capacity));
    assert(!captured->matchesPublishedOwnedMemory(plan.destinationBase+4,destination.data()+16,plan.capacity));
    assert(!captured->matchesPublishedOwnedMemory(plan.destinationBase,destination.data()+16,plan.capacity-4));
    assert(!captured->matchesPublishedOwnedMemory(plan.destinationBase,nullptr,plan.capacity));
    unsigned pixelRejections=0;
    for(unsigned y:{0u,31u,32u,359u,719u})for(unsigned x:{0u,31u,32u,639u,1279u})for(unsigned lane=0;lane<4;++lane){
        std::uint32_t offset=0;assert(colorCopyOffset(1280,720,x,y,offset));destination[16+offset+lane]^=1;
        assert(!captured->matchesPublishedOwnedMemory(plan.destinationBase,destination.data()+16,plan.capacity));
        destination[16+offset+lane]^=1;++pixelRejections;
    }
    unsigned padding=0;for (auto word:mask) if (!word) padding+=4;
    assert(padding==81920);
    for (unsigned at:{0u,15u,plan.capacity+16,plan.capacity+31}) {
        auto bad=original;bad[at]^=1;
        assert(!NativeTiledCopyPublication::capture(plan,bad.data(),bad.size()));
    }
    unsigned firstHole=0;while(mask[firstHole]) ++firstHole;
    destination[16+firstHole*4]^=1;
    assert(captured->matchesPublishedOwnedMemory(plan.destinationBase,destination.data()+16,plan.capacity));
    destination[16+firstHole*4]^=1;
    auto bad=original;bad[16+firstHole*4]^=1;
    assert(!NativeTiledCopyPublication::capture(plan,bad.data(),bad.size()));
    assert(!NativeTiledCopyPublication::capture(plan,original.data(),original.size()-4));
    assert(!NativeTiledCopyPublication::capture(plan,nullptr,original.size()));
    for (unsigned field=0;field<4;++field) {
        auto invalid=plan;
        if(field==0)++invalid.capacity;else if(field==1)++invalid.pixelBytes;
        else if(field==2)++invalid.touchedSpan;else invalid.destinationBase|=4;
        assert(!NativeTiledCopyPublication::capture(invalid,original.data(),original.size()));
    }
    assert(destination==expected);
    auto moved=std::move(*captured);
    assert(!captured->matchesPlan(plan) && moved.matchesPlan(plan));
    assert(!captured->matchesPublishedOwnedMemory(plan.destinationBase,destination.data()+16,plan.capacity));
    std::printf("NATIVE_PUBLISHED_COPY_CONTINUITY: %u active-lane controls, padding ignored; no allocation lease PASS\n",pixelRejections);
    std::printf("NATIVE_TILED_REQUEST_IDENTITY: %u field-bit rejections; equal capacity is not matching layout PASS\n",planRejections);
    std::puts("NATIVE_TILED_PUBLICATION_V1 active_bytes=3686400 preserved_padding=81920 owned_snapshot=1 guest_memory_written=0");
}
