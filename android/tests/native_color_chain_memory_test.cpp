#include "native_color_chain_memory.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    NativeColorChainBindings bindings;
    bindings.depth.source={0x100000,nullptr,84};bindings.pairs[0].vertices.source={0x110000,nullptr,144};
    bindings.pairs[1].vertices.source={0x120000,nullptr,144};
    for(unsigned i=0;i<3;++i){NativeColorTextureBinding t;t.source={0x200000+i*0x10000,nullptr,4096};bindings.pairs[0].textures.push_back(t);}
    NativeColorTextureBinding t;t.source={0x300000,nullptr,4096};bindings.pairs[1].textures.push_back(t);
    DrawCommandPlan plan;NativeColorSourceRanges ranges;
    assert(collectNativeColorSourceRanges(plan,bindings,ranges) && ranges.count==7);
    for(unsigned i=0;i<8;++i)plan.shaders.push_back({i,0,0x400000+i*0x10000,false,std::vector<std::uint8_t>(12)});
    for(unsigned i=0;i<32;++i)plan.constants.push_back({i,0x500000+i*0x10000,0x4000,std::vector<std::uint8_t>(4)});
    assert(collectNativeColorSourceRanges(plan,bindings,ranges) && ranges.count==47 && ranges.shaderRanges==8 && ranges.constantRanges==32);
    NativeColorCopyRequest copy;copy.vertexPhysicalAddress=0x800000;copy.drawOffset=70;
    assert(planColorCopy({0x14000500,0xC02D0,1280,720,false},0x100000,0x2000000,0x2D00500,0x1000300,copy.copy));
    NativeColorChainEventRequests events;events.cacheFlushOffset=80;events.packetOffsets={{85,89}};
    events.physicalAddresses={{0x900000,0x900004}};
    const NativeWordRequest words[]={{16,11,NativeWordPhase::AfterGraphics},{20,22,NativeWordPhase::AfterGraphics}};
    assert(planNativeWordUpdates(words,2,40,events.staging));
    NativeColorMemoryControls controls{{0x1000,4096},{0x3000,400},{0x2000,4},{0x2004,4},34,37,{{0xC0013F00,0x3000,100}}};
    NativeColorMemoryRequest result;
    assert(planNativeColorMemory(ranges,100,copy,events,controls,result) && result.inputCount==48 && result.outputs[0].bytes==3768320);
    const auto saved=result;unsigned rejects=0;
    const auto equal=[](const auto& a,const auto& b){for(unsigned i=0;i<a.size();++i)if(a[i].address!=b[i].address || a[i].bytes!=b[i].bytes)return false;return true;};
    const auto reject=[&](const auto& sources,const auto& c,const auto& e,const auto& ct,std::size_t count=100){
        result=saved;assert(!planNativeColorMemory(sources,count,c,e,ct,result));
        assert(result.inputCount==saved.inputCount && equal(result.inputs,saved.inputs) && equal(result.controls,saved.controls) && equal(result.outputs,saved.outputs));++rejects;
    };
    for(unsigned i=0;i<ranges.count;++i){
        for(unsigned mode=0;mode<4;++mode){auto bad=ranges;
            if(mode==0)bad.ranges[i].address|=1;if(mode==1)bad.ranges[i].bytes=0;
            if(mode==2)bad.ranges[i].address=0x20000000;if(mode==3)bad.ranges[i].bytes=0xFFFFFFFF;
            reject(bad,copy,events,controls);
        }
        for(const auto& forbidden:saved.controls){auto bad=ranges;bad.ranges[i]={forbidden.address,4};reject(bad,copy,events,controls);}
        for(const auto& forbidden:saved.outputs){auto bad=ranges;bad.ranges[i]={forbidden.address,4};reject(bad,copy,events,controls);}
    }
    // Every output/input/control edge, including copy's untouched padding.
    for(unsigned i=0;i<ranges.count;++i){auto badEvents=events;badEvents.physicalAddresses[0]=ranges.ranges[i].address;
        reject(ranges,copy,badEvents,controls);}
    auto padding=ranges;padding.ranges[0]={copy.copy.destinationBase+copy.copy.capacity-4,4};reject(padding,copy,events,controls);
    auto readOverlap=ranges;readOverlap.ranges[0]=readOverlap.ranges[1];assert(planNativeColorMemory(readOverlap,100,copy,events,controls,result));
    for(unsigned mode=0;mode<21;++mode){auto s=ranges;auto c=copy;auto e=events;auto ct=controls;
        if(mode==0)s.count=48;if(mode==1)s.count=6;if(mode==2)s.shaderRanges=9;if(mode==3)s.constantRanges=33;
        if(mode==4)c.copy.capacity-=4;if(mode==5)c.copy.touchedSpan-=4;if(mode==6)c.copy.pixelBytes-=4;
        if(mode==7)c.copy.swapRedBlue=false;if(mode==8)c.vertexPhysicalAddress=ct.ring.address;
        if(mode==9)e.physicalAddresses[1]=e.physicalAddresses[0];if(mode==10)e.staging.count=65;
        if(mode==11)e.staging.words[0].byteOffset=20;if(mode==12)e.staging.words[0].phase=NativeWordPhase::BeforeGraphics;
        if(mode==13)e.packetOffsets[1]=100;if(mode==14)e.cacheFlushOffset=e.packetOffsets[0];
        if(mode==15)ct.rootWords[0]^=1;if(mode==16)ct.rootWords[1]^=4;if(mode==17)ct.rootWords[2]++;
        if(mode==18)ct.nextWord++;if(mode==19)ct.child.address=ct.ring.address;if(mode==20)ct.identifier=ct.readPointer;
        reject(s,c,e,ct);
    }
    reject(ranges,copy,events,controls,0);reject(ranges,copy,events,controls,4097);
    for(const auto& forbidden:saved.outputs){auto c=copy;c.vertexPhysicalAddress=forbidden.address;reject(ranges,c,events,controls);}
    // Collection checks lengths, source counts, aggregate budgets and all physical
    // reads. Immediate shaders are already inside the separately protected child.
    const auto rejectCollection=[&](const auto& p,const auto& inputs){
        auto out=ranges;assert(!collectNativeColorSourceRanges(p,inputs,out));
        assert(out.count==ranges.count && out.shaderRanges==ranges.shaderRanges && out.constantRanges==ranges.constantRanges &&
            out.constantZeroTextures==ranges.constantZeroTextures && equal(out.ranges,ranges.ranges));++rejects;
    };
    auto literal=bindings;auto& zero=literal.pairs[1].textures[0];
    zero.slot=2;zero.source={};zero.sourceKind=NativeColorTextureBinding::SourceKind::ConstantZeroSwizzle;
    const std::array<std::uint32_t,6> zeroWords{0x02024802,2,0,0x00281249,3,0x210};
    for(unsigned i=0;i<6;++i)zero.descriptor[i]={zeroWords[i],0xFFFFFFFF};
    for(bool full:{false,true}){
        NativeColorSourceRanges zeroRanges;
        assert(collectNativeColorSourceRanges(full?plan:DrawCommandPlan{},literal,zeroRanges));
        assert(zeroRanges.count==(full?46u:6u) && zeroRanges.constantZeroTextures==1);
        assert(planNativeColorMemory(zeroRanges,100,copy,events,controls,result) && result.inputCount==(full?47u:7u));
        for(unsigned i=0;i<result.inputCount;++i)assert(result.inputs[i].address!=0 && result.inputs[i].address!=t.source.physicalAddress);
        for(unsigned mode=0;mode<4;++mode){auto bad=zeroRanges;
            if(mode==0)bad.constantZeroTextures=0;if(mode==1)bad.constantZeroTextures=2;
            if(mode==2)--bad.count;if(mode==3)++bad.count;reject(bad,copy,events,controls);
        }
    }
    for(unsigned mode=0;mode<7;++mode){auto bad=literal;auto& texture=bad.pairs[1].textures[0];
        if(mode==0)texture.source.physicalAddress=0x300000;
        if(mode==1)texture.source.size=4096;
        if(mode==2)texture.source.bytes=reinterpret_cast<const std::uint8_t*>(0x1000);
        if(mode==3)texture.sourceKind=NativeColorTextureBinding::SourceKind::GuestMemory;
        if(mode==4)texture.sourceKind=static_cast<NativeColorTextureBinding::SourceKind>(99);
        if(mode==5)texture.descriptor[3].knownMask^=1;
        if(mode==6)bad.pairs[0].textures[0].sourceKind=NativeColorTextureBinding::SourceKind::ConstantZeroSwizzle;
        rejectCollection(plan,bad);
    }
    std::puts("NATIVE_COLOR_CONSTANT_ZERO_MEMORY: exact six sources plus shaders/constants/copy; 15 metadata rejections PASS");
    for(unsigned mode=0;mode<12;++mode){auto p=plan;auto b=bindings;
        if(mode==0)p.shaders.resize(9);if(mode==1)p.constants.resize(33);
        if(mode==2)p.shaders[0].bigEndianCode.clear();if(mode==3)p.shaders[0].bigEndianCode.resize(4092);
        if(mode==4)p.shaders[0].physicalAddress|=1;if(mode==5)p.shaders[0].immediate=true;
        if(mode==6)p.constants[0].bigEndianWords.resize(8192);if(mode==7)p.constants[0].bigEndianWords.resize(3);
        if(mode==8)b.depth.source.size=0;if(mode==9)b.pairs[0].textures.clear();
        if(mode==10)b.pairs[1].textures[0].source.size=2*1024*1024;if(mode==11)b.depth.source.physicalAddress=0xFFFFFFFC;
        rejectCollection(p,b);
    }
    plan.shaders[0].immediate=true;plan.shaders[0].physicalAddress=0;
    assert(collectNativeColorSourceRanges(plan,bindings,ranges) && ranges.count==46 && ranges.shaderRanges==7);
    assert(validNativePhysicalRange({0x1FFFFFFC,4}) && !validNativePhysicalRange({0x1FFFFFFC,8}));
    assert(!nativePhysicalOverlap({4,4},{8,4}) && nativePhysicalOverlap({4,8},{8,4}));
    std::printf("NATIVE_COLOR_CHAIN_MEMORY: %u rejection controls; complete physical reads, control/output exclusions, no leases or writes PASS\n",rejects);
}
