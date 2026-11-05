#include "native_display_gamma.h"
#include "native_display_gamma_fixture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    NativeDisplayGammaRamp::Packed packed{};
    for (unsigned i=0;i<256;++i) {
        const auto r=(i*1023+127)/255,g=((255-i)*1023+127)/255,b=(i*i*1023+32512)/65025;
        packed[i]=(r<<20)|(g<<10)|b;
    }
    const auto ramp=NativeDisplayGammaRamp::make(packed);assert(ramp);
    for (unsigned i=0;i<256;++i) for (unsigned bit=30;bit<32;++bit) {
        auto bad=packed;bad[i]|=1u<<bit;assert(!NativeDisplayGammaRamp::make(bad));
    }
    for (unsigned i=0;i<256;++i) for (unsigned j=0;j<256;++j) {
        const auto p=ramp->pixel({static_cast<std::uint8_t>(i),static_cast<std::uint8_t>(j),static_cast<std::uint8_t>(i^j),static_cast<std::uint8_t>(i+j)});
        assert(p[0]==i && p[1]==255-j && p[3]==static_cast<std::uint8_t>(i+j));
        const auto expected=((packed[i^j]&1023)*255+511)/1023;assert(p[2]==expected);
    }
    std::vector<std::uint8_t> input(1280*720*4+32,0xA5),output{1,2,3};
    for (unsigned i=16;i<input.size()-16;++i) input[i]=i^(i>>8);
    const auto original=input;
    assert(ramp->mapRgba(input.data()+16,input.size()-32,output) && input==original);
    for (unsigned i=0;i<output.size();i+=4) {
        const auto p=ramp->pixel({input[i+16],input[i+17],input[i+18],input[i+19]});
        for(unsigned c=0;c<4;++c)assert(output[i+c]==p[c]);
    }
    const auto first=output;assert(ramp->mapRgba(output.data(),output.size(),output));
    std::vector<std::uint8_t> twice;assert(ramp->mapRgba(first.data(),first.size(),twice) && twice==output);
    for (const auto size:{0u,1u,3u,1280*720*4+4u}) assert(!ramp->mapRgba(input.data(),size,output) && output==twice);
    assert(!ramp->mapRgba(nullptr,4,output) && output==twice);
    const auto bytes=test::gammaWrapperBytes(packed);
    const auto plan=NativeDisplayGammaWrapper::parse(0x10000,bytes.data(),bytes.size(),0x100000);assert(plan);
    assert(plan->eventPhysical(0)==0x20000 && plan->eventValue(1)==0xABCD &&
        plan->matches(0x10000,bytes.data(),bytes.size()) && plan->frontbuffer()==0x100000);
    unsigned controls=0;
    // Changing one sample also contradicts its paired canonical COND_WRITE
    // reference. Arbitrary legal ramps are accepted when both agree.
    for (unsigned i=0;i<bytes.size();++i) for(unsigned bit=0;bit<8;++bit) {
        auto bad=bytes;bad[i]^=1u<<bit;
        assert(!plan->matches(0x10000,bad.data(),bad.size()));
        const unsigned word=i/4;
        if (word==2333 || word==2334 || word==2337 || word==2338) continue; // Variable owned event addresses/values.
        assert(!NativeDisplayGammaWrapper::parse(0x10000,bad.data(),bad.size(),0x100000));++controls;
    }
    for(unsigned size:{0u,1u,9352u,9360u})assert(!NativeDisplayGammaWrapper::parse(0x10000,bytes.data(),size,0x100000));
    assert(!NativeDisplayGammaWrapper::parse(0x10001,bytes.data(),bytes.size(),0x100000));
    assert(!NativeDisplayGammaWrapper::parse(0x10000,bytes.data(),bytes.size(),0x100004));
    assert(controls==74720);
    std::puts("NATIVE_DISPLAY_GAMMA:74720packet bits,512reserved bits,65536channel cases,921600pixels/alpha/alias/canaries PASS; pure plan, no execution");
}
