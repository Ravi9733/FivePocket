#include "native_deferred_color_memory.h"
#include "native_color_batch_fixture.h"
#include "native_post_output_swap_fixture.h"
#include <cstdio>
#include <sys/mman.h>
using namespace recomp::gpu;
int main(){
    static_assert(!std::is_default_constructible_v<NativeDeferredColorMemoryPlan>);
    static_assert(!std::is_convertible_v<NativeDeferredColorMemoryPlan,NativeColorMemoryRequest>);
    unsigned physicalRejects=0,mappingRejects=0,staleRejects=0;
    for(bool constantZero:{false,true}){
        test::NativeColorBatchFixture fixture(0x800000,0x2000000,0x7000000,0x7000004,constantZero,true);
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
        const NativePhysicalRange old{0x4000000,NativeSwapImagePlan::Capacity};
        const auto request=NativeDeferredColorMemoryPlan::create(*captured,controls,old);assert(request);
        assert(request->inputCount()==(constantZero?7u:8u) && request->writes()[5].bytes==32);
        assert(request->controls().child.bytes==bytes.size());
        const auto reject=[&](const auto& c,NativePhysicalRange scanout){
            assert(!NativeDeferredColorMemoryPlan::create(*captured,c,scanout));++physicalRejects;};
        for(unsigned mode=0;mode<18;++mode){auto bad=controls;auto previous=old;
            if(mode==0)bad.rootWords[0]^=1;if(mode==1)bad.rootWords[1]^=4;if(mode==2)bad.rootWords[2]++;
            if(mode==3)bad.child.bytes-=4;if(mode==4)bad.previousWord-=3;if(mode==5)bad.nextWord++;
            if(mode==6)bad.identifier=bad.readPointer;if(mode==7)bad.readPointer.address=0x8000;
            if(mode==8)bad.identifier.address=0x30010;if(mode==9)bad.ring.address=old.address;
            if(mode==10)bad.ring.address=fixture.addresses[0];if(mode==11)bad.readPointer.address=fixture.addresses[7];
            if(mode==12)previous.address=0x2000000;if(mode==13)previous.address=0x2000000+NativeSwapImagePlan::Capacity-4096;
            if(mode==14)previous.address=0x8000;if(mode==15)previous.bytes-=4;
            if(mode==16)previous.address=0x1FFFF000;if(mode==17)previous.address=fixture.addresses[0];
            reject(bad,previous);
        }
        for(unsigned i=0;i<request->inputCount();++i){
            auto bad=controls;bad.identifier.address=request->inputs()[i].address;reject(bad,old);
            bad=controls;bad.readPointer.address=request->inputs()[i].address+request->inputs()[i].bytes-4;reject(bad,old);
        }
        for(const auto target:request->writes()){
            if(target.address==controls.readPointer.address)continue;
            auto bad=controls;bad.identifier.address=target.address;reject(bad,old);
        }
        // Real inaccessible mappings prove the matching routine does not read
        // either image (including tiled padding). Metadata-only is intentional.
        auto* imagePages=static_cast<std::uint8_t*>(mmap(nullptr,2*NativeSwapImagePlan::Capacity,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0));
        assert(imagePages!=MAP_FAILED);
        std::map<unsigned,std::vector<std::uint8_t>> storage;
        const auto mapped=[&](NativePhysicalRange range){
            if(range.address==old.address)return NativeColorMappedRange{range,range,imagePages,true};
            if(range.address==0x2000000)return NativeColorMappedRange{range,range,imagePages+NativeSwapImagePlan::Capacity,true};
            auto& buffer=storage[range.address];if(buffer.empty())buffer.resize(range.bytes);
            assert(buffer.size()==range.bytes);return NativeColorMappedRange{range,range,buffer.data(),true};
        };
        NativeDeferredColorResolvedMemory memory;
        for(unsigned i=0;i<request->inputCount();++i){
            memory.inputs[i]=mapped(request->inputs()[i]);const auto raw=reader(request->inputs()[i].address,request->inputs()[i].bytes);assert(raw);
            std::memcpy(memory.inputs[i].bytes(),raw->data(),raw->size());
        }
        for(unsigned i=0;i<5;++i)memory.protectedRanges[i]=mapped(request->protectedRanges()[i]);
        for(unsigned i=0;i<7;++i)memory.writes[i]=mapped(request->writes()[i]);
        const auto put=[](std::uint8_t* p,unsigned value){for(unsigned i=0;i<4;++i)p[i]=value>>(24-i*8);};
        put(memory.protectedRanges[2].bytes(),76);
        for(unsigned i=0;i<3;++i)put(memory.protectedRanges[0].bytes()+(76+i)*4,controls.rootWords[i]);
        std::memcpy(memory.protectedRanges[1].bytes(),bytes.data(),bytes.size());
        assert(validateNativeDeferredColorResolvedMemory(*request,memory));
        assert(matchesNativeDeferredColorMemory(*captured,*request,memory));
        const auto badMapping=[&](const auto& bad){assert(!validateNativeDeferredColorResolvedMemory(*request,bad));++mappingRejects;};
        for(unsigned section=0;section<3;++section){
            const auto size=section==0?request->inputCount():(section==1?5u:7u);
            for(unsigned i=0;i<size;++i)for(unsigned fault=0;fault<6;++fault){
                auto bad=memory;auto& range=section==0?bad.inputs[i]:(section==1?bad.protectedRanges[i]:bad.writes[i]);
                if(fault==0)range.allocationBytes=nullptr;if(fault==1)range.range.address^=4;
                if(fault==2)range.allocation.bytes-=4;if(fault==3)range.allocation.address|=1;
                // MAX-4 plus a four-byte extent does not overflow; a metadata
                // validator cannot infer OS mapping validity from that value.
                if(fault==4)range.allocationBytes=reinterpret_cast<std::uint8_t*>(UINTPTR_MAX-2);
                if(fault==5)range.allocationBytes=memory.inputs[0].allocationBytes;
                if(fault==5 && section==0 && i==0)continue;
                badMapping(bad);
            }
        }
        for(unsigned i=0;i<7;++i){auto bad=memory;bad.writes[i].writable=false;badMapping(bad);}
        auto disagreement=memory;disagreement.protectedRanges[2].allocationBytes=memory.protectedRanges[3].allocationBytes;badMapping(disagreement);
        const auto stale=[&](std::uint8_t* byte){
            *byte^=1;assert(!matchesNativeDeferredColorMemory(*captured,*request,memory));*byte^=1;++staleRejects;
        };
        for(unsigned i=0;i<request->inputCount();++i){stale(memory.inputs[i].bytes());stale(memory.inputs[i].bytes()+memory.inputs[i].range.bytes-1);}
        for(unsigned i=0;i<12;++i)stale(memory.protectedRanges[0].bytes()+76*4+i);
        for(unsigned i=0;i<4;++i)stale(memory.protectedRanges[2].bytes()+i);
        // Every byte of the combined child, including the opaque ticket and
        // tail, is tied to the exact original capture; no prefix-only compare.
        for(unsigned i=0;i<bytes.size();++i)stale(memory.protectedRanges[1].bytes()+i);
        memory.protectedRanges[3].bytes()[0]^=1;memory.writes[5].bytes()[0]^=1;
        assert(matchesNativeDeferredColorMemory(*captured,*request,memory)); // No identifier/IRQ state claim.
        assert(munmap(imagePages,2*NativeSwapImagePlan::Capacity)==0);
    }
    std::printf("DEFERRED_COLOR_MEMORY: %u physical refusals,%u mapping refusals,%u stale-byte refusals; PROT_NONE images unread,whole-child tied,no live lease or writes PASS\n",
        physicalRejects,mappingRejects,staleRejects);
}
