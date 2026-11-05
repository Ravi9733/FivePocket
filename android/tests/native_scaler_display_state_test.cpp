#include "native_scaler_display_state.h"
#include "native_identity_scaler_fixture.h"
#include "native_scaler_wrapper_fixture.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
int main() {
    const auto record=identityScalerRecord();
    const auto candidate=NativeIdentityScalerCandidate::inspect(identityScalerInputs(),record.data(),record.size());assert(candidate);
    const auto plan=NativeIdentityScalerPlan::make(*candidate);assert(plan);
    const auto command=NativeScalerCommand::make({1,43,0x64000000},*plan);assert(command);
    const auto bytes=scalerWrapperBytes(*command,0x20000,0x82004000,0x208);
    const auto wrapper=NativeScalerWrapper::parse(*command,0x10000,0x10114,bytes.data(),bytes.size());assert(wrapper);
    std::vector<std::uint8_t> input(1280*720*4+32,0xA5),out{1,2,3};
    for (unsigned y=0;y<720;++y) for (unsigned x=0;x<1280;++x) for (unsigned c=0;c<4;++c)
        input[16+(y*1280+x)*4+c]=(x*17+y*31+c*83)^(x>>8)^(y>>3);
    const auto original=input;
    NativeScalerDisplayState state;unsigned fences=0,controls=0;
    assert(!state.mapRgba(input.data()+16,input.size()-32,1280,720,out) && out==std::vector<std::uint8_t>({1,2,3}));
    const auto fence=[&] {++fences;return true;};
    for (unsigned i=15;i<22;++i) {
        for (unsigned j=0;j<26;++j) if(i!=j) {
            assert(!state.apply(wrapper->operations()[j],*command,fence) && state.nextOperation()==i);++controls;
        }
        auto bad=wrapper->operations()[i];bad.value^=1;
        assert(!state.apply(bad,*command,fence) && !state.apply(wrapper->operations()[i],*command,{}));
        assert(!state.configured());
        assert(state.apply(wrapper->operations()[i],*command,fence));
    }
    assert(state.configured() && fences==2 && state.nativeFences()==2 && controls==175);
    assert(!state.apply(wrapper->operations()[21],*command,fence));
    assert(state.mapRgba(input.data()+16,input.size()-32,1280,720,out));
    assert(out.size()==input.size()-32 && std::equal(out.begin(),out.end(),input.begin()+16) && input==original);
    const auto expected=out;
    assert(!state.mapRgba(nullptr,out.size(),1280,720,out));
    assert(!state.mapRgba(out.data(),out.size()-1,1280,720,out));
    assert(!state.mapRgba(out.data(),out.size(),720,1280,out));
    assert(out==expected && state.mapRgba(out.data(),out.size(),1280,720,out) && out==expected);
    for (unsigned failAt:{18u,21u}) for (bool throws:{false,true}) {
        NativeScalerDisplayState broken;unsigned calls=0;
        for (unsigned i=15;i<failAt;++i) assert(broken.apply(wrapper->operations()[i],*command,[]{return true;}));
        bool caught=false;
        try {assert(!broken.apply(wrapper->operations()[failAt],*command,[&]()->bool {
            ++calls;if (throws) throw std::runtime_error("native fence failure");return false;
        }));} catch (const std::runtime_error&) {caught=true;}
        assert(caught==throws && broken.failed() && !broken.configured() && calls==1);
        assert(!broken.apply(wrapper->operations()[failAt],*command,fence));
        assert(!broken.mapRgba(input.data()+16,input.size()-32,1280,720,out) && out==expected);
    }
    std::puts("NATIVE_SCALER_DISPLAY_STATE:175ordering controls,2required fences,4terminal failures,921600RGBA mappings/alias/canaries PASS; CPU fixture only");
}
