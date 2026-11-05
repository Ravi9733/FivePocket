#include "native_color_output_publication.h"
#include "native_color_batch_fixture.h"
#include "../../tools/vulkan_probe/native_color_scratch_memory.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    test::NativeColorBatchFixture fixture(0x800000,0x2000000,0x7000000,0x7000004);
    std::vector<std::uint8_t> raw;
    for(auto word:fixture.words)for(unsigned b=0;b<4;++b)raw.push_back(word>>(24-b*8));
    Pm4Snapshot child;Pm4Error error;assert(decodePm4(raw.data(),raw.size(),{},child,error));
    NativeColorMemoryControls controls{{0x1000,4096},{0x3000,static_cast<unsigned>(raw.size())},{0x2000,4},{0x2004,4},
        34,37,{{0xC0013F00,0x3000,static_cast<unsigned>(fixture.words.size())}}};
    auto capture=NativeColorBatchCapture::create(child,{},*fixture.nativeProgram,controls,
        [&](auto address,auto size)->std::optional<std::vector<std::uint8_t>>{
            for(unsigned i=0;i<fixture.raw.size();++i)
                if(address==fixture.addresses[i] && size==fixture.raw[i].size())return fixture.raw[i];
            return std::nullopt;});
    assert(capture);auto scratch=NativeColorScratchMemory::create(*capture);assert(scratch);
    const auto& memory=scratch->memory();const auto& plan=capture->copy().copy;
    std::vector<std::uint8_t> tiled(plan.capacity+32,0x5A),expected(plan.capacity);
    for(unsigned i=0;i<plan.capacity;++i)memory.outputs[0].bytes()[i]=expected[i]=i%251;
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x){unsigned at=0;assert(colorCopyOffset(1280,720,x,y,at));
        for(unsigned b=0;b<4;++b)tiled[16+at+b]=expected[at+b]=(x+y*7+b)%256;}
    auto copy=NativeTiledCopyPublication::capture(plan,tiled.data(),tiled.size());assert(copy);
    std::array<std::uint8_t,40> events;events.fill(0x5A);
    for(unsigned i=0;i<capture->events().staging.count;++i){const auto& w=capture->events().staging.words[i];
        std::memcpy(events.data()+w.byteOffset,w.bigEndianBytes.data(),4);}
    const auto snapshot=[&](){std::array<std::vector<std::uint8_t>,4> bytes;
        for(unsigned i=0;i<4;++i)bytes[i].assign(memory.outputs[i].bytes(),memory.outputs[i].bytes()+memory.outputs[i].range.bytes);
        return bytes;};
    const auto before=snapshot();unsigned rejects=0;
    const auto changed=[&](std::uint8_t* p){*p^=1;const auto beforeCall=snapshot();
        assert(!publishCapturedNativeColorOutputs(*capture,memory,*copy,events.data(),events.size()) && snapshot()==beforeCall);
        *p^=1;assert(snapshot()==before);++rejects;};
    for(unsigned i=0;i<capture->memory().inputCount;++i){changed(memory.inputs[i].bytes());changed(memory.inputs[i].bytes()+memory.inputs[i].range.bytes-1);}
    changed(memory.controls[1].bytes());changed(memory.controls[1].bytes()+controls.child.bytes-1);
    for(unsigned i=0;i<3;++i)changed(memory.controls[0].bytes()+(controls.previousWord+i)*4);
    for(unsigned i=0;i<4;++i)changed(memory.controls[2].bytes()+i);
    for(unsigned i=0;i<4;++i){auto bad=memory;bad.outputs[i].writable=false;
        assert(!publishCapturedNativeColorOutputs(*capture,bad,*copy,events.data(),40) && snapshot()==before);++rejects;
        bad=memory;bad.outputs[i].allocationBytes=memory.inputs[0].allocationBytes;
        assert(!publishCapturedNativeColorOutputs(*capture,bad,*copy,events.data(),40) && snapshot()==before);++rejects;}
    events[23]^=1;assert(!publishCapturedNativeColorOutputs(*capture,memory,*copy,events.data(),40) && snapshot()==before);events[23]^=1;++rejects;
    assert(!publishCapturedNativeColorOutputs(*capture,memory,*copy,nullptr,40) && snapshot()==before);++rejects;
    auto different=plan;different.swapRedBlue=!different.swapRedBlue;
    auto other=NativeTiledCopyPublication::capture(different,tiled.data(),tiled.size());assert(other);
    assert(!publishCapturedNativeColorOutputs(*capture,memory,*other,events.data(),40) && snapshot()==before);++rejects;
    // Synthetic CPU unit: publication into scratch is not simulated GPU success.
    assert(publishCapturedNativeColorOutputs(*capture,memory,*copy,events.data(),40));
    assert(std::equal(expected.begin(),expected.end(),memory.outputs[0].bytes()));
    assert(!std::memcmp(memory.outputs[1].bytes(),events.data()+16,4) && !std::memcmp(memory.outputs[2].bytes(),events.data()+20,4));
    assert(memory.outputs[3].bytes()[3]==37);
    const auto after=snapshot();assert(!publishCapturedNativeColorOutputs(*capture,memory,*copy,events.data(),40) && snapshot()==after);++rejects;
    memory.outputs[3].bytes()[3]=34;assert(matchesNativeColorBatchMemory(*capture,memory));memory.outputs[3].bytes()[3]=37;
    std::printf("NATIVE_COLOR_OUTPUT_PUBLICATION: %u failure-atomic rejections; copy/events/RPTR from one request, scratch only PASS\n",rejects);
}
