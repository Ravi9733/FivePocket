#include "native_color_pending_registers.h"
#include "native_color_batch_fixture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;

int main(){
    test::NativeColorBatchFixture fixture(0x800000,0x2000000,0x7000000,0x7000004);
    // Constant-load source independent of the fixture's texture/vertex spans.
    fixture.words.insert(fixture.words.begin(),{0x2010,0x87654313});
    // After the color PS constants (which also write47FE/47FF), before events.
    fixture.words.insert(fixture.words.end()-8,{0xC0022F00,0xC00000,0x7FE,2});
    const std::vector<std::uint8_t> constants{1,2,3,4,0xAB,0xCD,0xEF,0};
    const auto create=[&](){
        std::vector<std::uint8_t> raw;
        for(auto word:fixture.words)for(unsigned b=0;b<4;++b)raw.push_back(word>>(24-b*8));
        Pm4Snapshot child;Pm4Error error;assert(decodePm4(raw.data(),raw.size(),{},child,error));
        NativeColorMemoryControls controls{{0x1000,4096},{0x3000,static_cast<unsigned>(raw.size())},
            {0x2000,4},{0x2004,4},34,37,{{0xC0013F00,0x3000,static_cast<unsigned>(fixture.words.size())}}};
        auto capture=NativeColorBatchCapture::create(child,{},*fixture.nativeProgram,controls,
            [&](auto address,auto size)->std::optional<std::vector<std::uint8_t>>{
                if(address==0xC00000 && size==constants.size())return constants;
                for(unsigned i=0;i<fixture.raw.size();++i)
                    if(address==fixture.addresses[i] && size==fixture.raw[i].size())return fixture.raw[i];
                return std::nullopt;
            });
        assert(capture);return capture;
    };
    auto capture=create();
    NativeRegisterJournal prior{{0x2010,{0x87654321,0xFFFFFFFF}},{0x3000,{0xAB00,0xFF00}},
        {0x4800,{0xFFFFFF,0xFFFFFF}},{0x5C8,{0x20000,0xFFFFFFFF}},{0xA31,{0,0xFFFFFFFF}},
        {0xA2F,{0x1000,0xFFFFFFFF}},{0xA30,{0x2000,0xFFFFFFFF}},{0x21F9,{3,0xFFFFFFFF}},
        {0xE00,{1,0xFFFFFFFF}}};
    const auto original=prior;NativeRegisterJournal staged{{99,{77,0xFFFFFFFF}}};
    assert(prepareNativeColorRegisters(*capture,prior,staged) && prior==original);
    assert(staged.at(0x2010)==std::make_pair(0x87654313u,0xFFFFFFFFu));
    assert(staged.at(0x200E)==std::make_pair(0u,0xFFFFFFFFu));
    assert(staged.at(0x3000)==prior.at(0x3000) && staged.at(0xE00)==prior.at(0xE00));
    assert(staged.at(0x47FE)==std::make_pair(0x01020304u,0xFFFFFFFFu));
    assert(staged.at(0x47FF)==std::make_pair(0xABCDEF00u,0xFFFFFFFFu));
    for(unsigned index:{0x5C8u,0xA2Fu,0xA30u,0xA31u,0x21F9u})assert(!staged.count(index));
    assert(staged.at(0x4800).first==(fixture.addresses[7]|3));
    // Exhaust every prior known/value combination for eight bits: assigned
    // registers replace those bits, unassigned registers preserve them exactly.
    unsigned cases=0;
    for(unsigned known=0;known<256;++known)for(unsigned value=0;value<256;++value)if(!(value&~known)){
        NativeRegisterJournal input{{0x2010,{value,known}},{0x3000,{value,known}}},output;
        assert(prepareNativeColorRegisters(*capture,input,output));
        assert(output.at(0x2010)==std::make_pair(0x87654313u,0xFFFFFFFFu));
        assert(output.at(0x3000)==std::make_pair(value,known));++cases;
    }
    unsigned rejects=0;
    for(unsigned index:{0x8000u,0xFFFFFFFFu}){
        auto invalid=prior;invalid[index]={0,0};auto output=staged;
        assert(!prepareNativeColorRegisters(*capture,invalid,output) && output==staged);++rejects;
    }
    for(unsigned bit=0;bit<32;++bit){
        auto invalid=prior;invalid[0x3000]={1u<<bit,~(1u<<bit)};auto output=staged;
        assert(!prepareNativeColorRegisters(*capture,invalid,output) && output==staged);++rejects;
    }
    auto owned=std::move(*capture);auto output=staged;
    assert(!prepareNativeColorRegisters(*capture,prior,output) && output==staged);++rejects;
    assert(prepareNativeColorRegisters(owned,prior,prior) && prior==staged); // aliased input/output
    std::printf("NATIVE_COLOR_PENDING_REGISTERS: %u prior-knowledge cases, %u rejections; sync/event registers unknown, no commit PASS\n",cases,rejects);
}
