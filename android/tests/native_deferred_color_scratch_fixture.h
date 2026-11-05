#pragma once
#include "native_deferred_color_write_guard.h"
#include "native_color_batch_fixture.h"
#include "native_post_output_swap_fixture.h"

namespace recomp::gpu::test {
// Entirely CPU-owned scratch. No allocator lease, GPU, game memory or display.
struct NativeDeferredColorScratchFixture {
    NativeColorBatchFixture fixture{0x800000,0x2000000,0x7000000,0x7000004,true,true};
    std::optional<NativeDeferredColorCapture> capture;
    std::optional<NativeDeferredColorMemoryPlan> plan;
    NativeDeferredColorResolvedMemory memory;
    std::map<unsigned,std::vector<std::uint8_t>> storage;
    std::optional<NativeDeferredColorWriteGuard> guard;
    NativeDeferredColorScratchFixture(){
        auto words=fixture.words;words.resize(words.size()-14);words.insert(words.end(),{0x2007,0,0x2318,0,0x231B,0});
        const auto command=NativeSwapCommand::make({3,0x3000+unsigned(words.size())*4+40,0x2000000,73});assert(command);
        std::vector<std::uint8_t> bytes;
        for(auto w:words)for(unsigned i=0;i<4;++i)bytes.push_back(w>>(24-i*8));
        const auto tail=postOutputSwapBytes(*command);bytes.insert(bytes.end(),tail.begin(),tail.end());
        const auto frame=NativeDeferredSwapSubmission::parse(*command,0x3000,bytes.data(),bytes.size());assert(frame);
        const Pm4PhysicalReader reader=[&](auto a,auto n)->std::optional<std::vector<std::uint8_t>>{
            for(unsigned i=0;i<8;++i)if(a==fixture.addresses[i] && n==fixture.raw[i].size())return fixture.raw[i];
            return std::nullopt;
        };
        capture=NativeDeferredColorCapture::create(*frame,{},*fixture.nativeProgram,reader);assert(capture);
        const NativeColorMemoryControls controls{{0x1000,4096},{0x3000,unsigned(bytes.size())},{0x2000,4},{0x2004,4},76,79,
            {{0xC0013F00,0x3000,unsigned(bytes.size()/4)}}};
        plan=NativeDeferredColorMemoryPlan::create(*capture,controls,{0x4000000,NativeSwapImagePlan::Capacity});assert(plan);
        const auto mapped=[&](NativePhysicalRange range){
            auto& buffer=storage[range.address];if(buffer.empty())buffer.resize(range.bytes+32,0xCD);
            assert(buffer.size()==range.bytes+32);return NativeColorMappedRange{range,range,buffer.data()+16,true};
        };
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
        guard=NativeDeferredColorWriteGuard::create(*capture,*plan,memory);assert(guard);
    }
};
}
