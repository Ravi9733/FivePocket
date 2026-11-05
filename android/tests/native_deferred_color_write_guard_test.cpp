#include "native_deferred_color_write_guard.h"
#include "native_color_batch_fixture.h"
#include "native_post_output_swap_fixture.h"
#include <cstdio>
using namespace recomp::gpu;
int main(){
    static_assert(!std::is_copy_constructible_v<NativeDeferredColorWriteGuard>);
    static_assert(std::is_nothrow_move_constructible_v<NativeDeferredColorWriteGuard>);
    test::NativeColorBatchFixture fixture(0x800000,0x2000000,0x7000000,0x7000004,true,true);
    auto words=fixture.words;words.resize(words.size()-14);words.insert(words.end(),{0x2007,0,0x2318,0,0x231B,0});
    const auto command=NativeSwapCommand::make({3,0x3000+unsigned(words.size())*4+40,0x2000000,73});assert(command);
    std::vector<std::uint8_t> bytes;
    for(auto w:words)for(unsigned i=0;i<4;++i)bytes.push_back(w>>(24-i*8));
    const auto tail=test::postOutputSwapBytes(*command);bytes.insert(bytes.end(),tail.begin(),tail.end());
    const auto frame=NativeDeferredSwapSubmission::parse(*command,0x3000,bytes.data(),bytes.size());assert(frame);
    const Pm4PhysicalReader reader=[&](auto a,auto n)->std::optional<std::vector<std::uint8_t>>{
        for(unsigned i=0;i<8;++i)if(a==fixture.addresses[i] && n==fixture.raw[i].size())return fixture.raw[i];
        return std::nullopt;
    };
    const auto captured=NativeDeferredColorCapture::create(*frame,{},*fixture.nativeProgram,reader);assert(captured);
    const NativeColorMemoryControls controls{{0x1000,4096},{0x3000,unsigned(bytes.size())},{0x2000,4},{0x2004,4},76,79,
        {{0xC0013F00,0x3000,unsigned(bytes.size()/4)}}};
    const auto plan=NativeDeferredColorMemoryPlan::create(*captured,controls,{0x4000000,NativeSwapImagePlan::Capacity});assert(plan);
    std::map<unsigned,std::vector<std::uint8_t>> storage;
    const auto mapped=[&](NativePhysicalRange range){
        auto& buffer=storage[range.address];if(buffer.empty())buffer.resize(range.bytes+32,0xCD);
        assert(buffer.size()==range.bytes+32);return NativeColorMappedRange{range,range,buffer.data()+16,true};
    };
    NativeDeferredColorResolvedMemory memory;
    for(unsigned i=0;i<plan->inputCount();++i){
        memory.inputs[i]=mapped(plan->inputs()[i]);const auto raw=reader(plan->inputs()[i].address,plan->inputs()[i].bytes);assert(raw);
        std::memcpy(memory.inputs[i].bytes(),raw->data(),raw->size());
    }
    for(unsigned i=0;i<5;++i)memory.protectedRanges[i]=mapped(plan->protectedRanges()[i]);
    for(unsigned i=0;i<7;++i)memory.writes[i]=mapped(plan->writes()[i]);
    const auto put=[](std::uint8_t* p,unsigned value){for(unsigned i=0;i<4;++i)p[i]=value>>(24-i*8);};
    put(memory.protectedRanges[2].bytes(),76);
    for(unsigned i=0;i<3;++i)put(memory.protectedRanges[0].bytes()+(76+i)*4,controls.rootWords[i]);
    std::memcpy(memory.protectedRanges[1].bytes(),bytes.data(),bytes.size());
    const auto before=storage;
    auto guard=NativeDeferredColorWriteGuard::create(*captured,*plan,memory);assert(guard);
    assert(guard->snapshotBytes()>2*NativeSwapImagePlan::Capacity && guard->matches(*captured,memory));
    const auto& copyPlan=captured->copy().copy;
    std::vector<std::uint8_t> readback(copyPlan.capacity+32,0x5A);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){
        unsigned at=0;assert(colorCopyOffset(1280,720,x,y,at));put(readback.data()+16+at,0x12345678u^(y*1280+x));
    }
    const auto copy=NativeTiledCopyPublication::capture(copyPlan,readback.data(),readback.size());assert(copy);
    auto wrongPlan=copyPlan;wrongPlan.destinationBase+=0x400000;
    const auto wrong=NativeTiledCopyPublication::capture(wrongPlan,readback.data(),readback.size());assert(wrong);
    assert(!guard->publishCopy(*captured,memory,*wrong) && storage==before && !guard->published());
    unsigned stale=0,owners=0;
    const auto rejectByte=[&](std::uint8_t* value){
        *value^=1;const auto changed=storage;
        assert(!guard->matches(*captured,memory) && !guard->publishCopy(*captured,memory,*copy) && storage==changed && !guard->published());
        *value^=1;++stale;
    };
    for(unsigned i=0;i<plan->inputCount();++i){rejectByte(memory.inputs[i].bytes());rejectByte(memory.inputs[i].bytes()+memory.inputs[i].range.bytes-1);}
    for(const auto& m:memory.protectedRanges){rejectByte(m.bytes());rejectByte(m.bytes()+m.range.bytes/2);rejectByte(m.bytes()+m.range.bytes-1);}
    for(const auto& m:memory.writes){rejectByte(m.bytes());rejectByte(m.bytes()+m.range.bytes/2);rejectByte(m.bytes()+m.range.bytes-1);}
    for(unsigned section=0;section<3;++section){
        const auto count=section==0?plan->inputCount():(section==1?5u:7u);
        for(unsigned i=0;i<count;++i)for(unsigned fault=0;fault<4;++fault){
            auto bad=memory;auto& m=section==0?bad.inputs[i]:(section==1?bad.protectedRanges[i]:bad.writes[i]);
            if(fault==0)m.range.address^=4;if(fault==1)m.writable=!m.writable;
            if(fault==2)m.allocation.bytes+=4;if(fault==3)m.allocationBytes=nullptr;
            assert(!guard->publishCopy(*captured,bad,*copy) && storage==before && !guard->published());++owners;
        }
    }
    // Equivalent bytes in a different host owner may pass the generic mapping
    // check, but cannot replace an owner retained by this guard.
    auto relocated=storage.at(plan->inputs()[0].address);auto remapped=memory;
    remapped.inputs[0].allocationBytes=relocated.data()+16;
    assert(matchesNativeDeferredColorMemory(*captured,*plan,remapped));
    assert(!guard->publishCopy(*captured,remapped,*copy) && storage==before);++owners;
    auto invalid=memory;invalid.writes[0].allocationBytes=nullptr;
    assert(!NativeDeferredColorWriteGuard::create(*captured,*plan,invalid) && storage==before);
    memory.protectedRanges[2].bytes()[3]^=1;
    assert(!NativeDeferredColorWriteGuard::create(*captured,*plan,memory));
    memory.protectedRanges[2].bytes()[3]^=1;
    // A fresh valid recapture of altered texture bytes cannot replace the
    // inputs protected when this guard was created, even if PM4 is unchanged.
    fixture.raw[3][0]^=1;memory.inputs[3].bytes()[0]^=1;
    const auto newer=NativeDeferredColorCapture::create(*frame,{},*fixture.nativeProgram,reader);assert(newer);
    assert(matchesNativeDeferredColorMemory(*newer,*plan,memory));
    const auto changed=storage;
    assert(!guard->publishCopy(*newer,memory,*copy) && storage==changed && !guard->published());
    fixture.raw[3][0]^=1;memory.inputs[3].bytes()[0]^=1;
    auto moved=std::move(*guard);
    assert(!guard->matches(*captured,memory) && !guard->publishCopy(*captured,memory,*copy) && storage==before);
    assert(moved.matches(*captured,memory) && moved.publishCopy(*captured,memory,*copy) && moved.published());
    assert(!moved.matches(*captured,memory) && !moved.publishCopy(*captured,memory,*copy));
    auto expected=before;auto& image=expected.at(copyPlan.destinationBase);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){unsigned at=0;assert(colorCopyOffset(1280,720,x,y,at));
        std::memcpy(image.data()+16+at,readback.data()+16+at,4);}
    assert(storage==expected); // ALL padding, canaries, old image, sources, ring, tail, IRQ and identifier unchanged.
    assert(copy->matchesPublishedOwnedMemory(copyPlan.destinationBase,memory.writes[0].bytes(),copyPlan.capacity));
    std::printf("DEFERRED_COLOR_WRITE_GUARD: %u stale-byte refusals,%u owner refusals,2 capture refusals,1 changed-source recapture; active pixels only, one-shot, padding/tail/IRQ/RPTR unchanged; scratch only PASS\n",stale,owners);
}
