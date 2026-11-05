#include "native_color_batch_capture.h"
#include "native_color_batch_fixture.h"
#include "../../tools/vulkan_probe/native_color_scratch_memory.h"
#include <cassert>
#include <cstdio>
#include <type_traits>
using namespace recomp::gpu;
int main(){
    static_assert(!std::is_copy_constructible_v<NativeColorBatchCapture> && !std::is_default_constructible_v<NativeColorBatchCapture>);
    static_assert(std::is_nothrow_move_constructible_v<NativeColorBatchCapture> && std::is_nothrow_move_assignable_v<NativeColorBatchCapture>);
    const auto module=[](unsigned model){return std::vector<std::uint32_t>{0x07230203,0x10300,0,16,0,(2u<<16)|17,1,(5u<<16)|15,model,1,0x6E69616D,0};};
    NativeColorChainProgram::Translations translations;
    for(unsigned i=0;i<5;++i){const unsigned stage=i==2 || i==4?1:0;translations[i]={{stage,std::vector<std::uint8_t>(12)},module(stage?4:0)};}
    NativeColorChainProgram::Support support;for(auto& s:support)s=module(5);
    const auto program=NativeColorChainProgram::create(translations,support,1,NativeColorCoordinateProfile::Padded1280W1);
    const auto diagnostic=NativeColorChainProgram::create(translations,support);assert(program && diagnostic);
    std::vector<std::uint8_t> raw;
    // Include a syntactically valid draw: a shader-only stream is rejected by
    // the whole-stream preflight before any physical reader is called.
    for(unsigned w:{0xC0012700u,0x400000u,3u,0xC0003600u,0x00030088u})for(unsigned b=0;b<4;++b)raw.push_back(w>>(24-b*8));
    Pm4Snapshot child;Pm4Error error;assert(decodePm4(raw.data(),raw.size(),{},child,error));
    NativeColorMemoryControls controls{{0x1000,4096},{0x3000,20},{0x2000,4},{0x2004,4},34,37,{{0xC0013F00,0x3000,5}}};
    assert(validNativeColorMemoryControls(controls,5));
    unsigned reads=0,rejections=0;
    const Pm4PhysicalReader reader=[&](auto,auto)->std::optional<std::vector<std::uint8_t>>{++reads;return std::nullopt;};
    for(unsigned mode=0;mode<16;++mode){auto bad=controls;
        if(mode==0)bad.ring.address|=1;if(mode==1)bad.ring.bytes=0;if(mode==2)bad.ring.bytes=0x20000000;
        if(mode==3)bad.child.bytes=16;if(mode==4)bad.child.address=0x20000000;
        if(mode==5)bad.readPointer.bytes=8;if(mode==6)bad.identifier.bytes=8;if(mode==7)bad.identifier=bad.readPointer;
        if(mode==8)bad.nextWord++;if(mode==9)bad.previousWord=UINT32_MAX;if(mode==10)bad.rootWords[0]^=1;
        if(mode==11)bad.rootWords[1]^=4;if(mode==12)bad.rootWords[2]++;
        if(mode==13){bad.child.address=bad.ring.address;bad.rootWords[1]=bad.child.address;}
        if(mode==14)bad.readPointer.address=bad.ring.address;if(mode==15)bad.identifier.address=bad.child.address;
        assert(!NativeColorBatchCapture::create(child,{},*program,bad,reader) && !reads);++rejections;
    }
    assert(!NativeColorBatchCapture::create(child,{},*diagnostic,controls,reader) && !reads);++rejections;
    assert(!NativeColorBatchCapture::create(child,{},*program,controls,{}) && !reads);++rejections;
    DrawBinState invalid;invalid.knownWords=16;
    assert(!NativeColorBatchCapture::create(child,invalid,*program,controls,reader) && !reads);++rejections;
    // Now valid metadata reaches the source reader; failure is a clean rejection.
    assert(!NativeColorBatchCapture::create(child,{},*program,controls,reader) && reads==1);++rejections;
    const Pm4PhysicalReader throwing=[](auto,auto)->std::optional<std::vector<std::uint8_t>>{throw 1;};
    assert(!NativeColorBatchCapture::create(child,{},*program,controls,throwing));++rejections;
    std::printf("NATIVE_COLOR_BATCH_CAPTURE: %u rejection controls; root before reads, move-only, no live owner/execution claim PASS\n",rejections);
    for(bool postOutput:{false,true})for(bool constantZero:{false,true}){
        test::NativeColorBatchFixture fixture(0x800000,0x2000000,0x7000000,0x7000004,constantZero,postOutput);
        std::vector<std::uint8_t> rawBatch;
        for(auto word:fixture.words)for(unsigned b=0;b<4;++b)rawBatch.push_back(word>>(24-b*8));
        Pm4Snapshot snapshot;assert(decodePm4(rawBatch.data(),rawBatch.size(),{},snapshot,error));
        auto batchControls=controls;batchControls.child.bytes=static_cast<unsigned>(rawBatch.size());
        batchControls.rootWords[2]=static_cast<unsigned>(fixture.words.size());
        unsigned batchReads=0;
        auto batch=NativeColorBatchCapture::create(snapshot,{},*fixture.nativeProgram,batchControls,
            [&](auto address,auto size)->std::optional<std::vector<std::uint8_t>>{
                ++batchReads;assert(address!=0 && (!constantZero || address!=fixture.addresses[6]));
                for(unsigned i=0;i<8;++i)if(address==fixture.addresses[i] && size==fixture.raw[i].size())return fixture.raw[i];
                return std::nullopt;
            },postOutput?NativeColorBatchPhase::PostOutput:NativeColorBatchPhase::Startup);
        assert(batch && batchReads==(constantZero?7u:8u) && batch->memory().inputCount==batchReads);
        assert(batch->color().sourceRanges().constantZeroTextures==unsigned(constantZero));
        assert(batch->color().depthState().interpolatorControl==(postOutput?0x10003u:0u));
        assert(batch->synchronization().phase==(postOutput?NativeColorBatchPhase::PostOutput:NativeColorBatchPhase::Startup));
        // Neither profile silently absorbs the other's leading wait/setup.
        assert(!NativeColorBatchCapture::create(snapshot,{},*fixture.nativeProgram,batchControls,
            [](auto,auto)->std::optional<std::vector<std::uint8_t>>{assert(false);return std::nullopt;},
            postOutput?NativeColorBatchPhase::Startup:NativeColorBatchPhase::PostOutput));
        auto scratch=NativeColorScratchMemory::create(*batch);assert(scratch);
        assert(matchesNativeColorBatchMemory(*batch,scratch->memory()));
        for(unsigned i=0;i<batchReads;++i){
            auto* bytes=scratch->memory().inputs[i].bytes();bytes[0]^=1;
            assert(!matchesNativeColorBatchMemory(*batch,scratch->memory()));bytes[0]^=1;
            assert(matchesNativeColorBatchMemory(*batch,scratch->memory()));
        }
        auto moved=std::move(*batch);assert(matchesNativeColorBatchMemory(moved,scratch->memory()));
        std::printf("NATIVE_COLOR_BATCH_CONSTANT_ZERO: enabled=%u exact_reads=%u immutable_sources_and_move=PASS phase=%u\n",constantZero,batchReads,unsigned(postOutput));
    }
}
