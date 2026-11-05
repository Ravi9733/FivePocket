#include "native_color_viewport_fixture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    const auto draw=test::colorViewportDraw();assert(draw.registers.size()==28);
    NativeColorViewportRequest result;
    assert(planNativeColorViewportRequest(draw,result) && result.drawOffset==100);
    assert((result.nativeViewport==std::array<float,6>{0,0,2048,1024,1,0}));
    assert((result.nativeScissor==std::array<std::uint32_t,4>{0,0,1280,720}));
    const NativeColorViewportRequest sentinel{73,{{-1,-2,-3,-4,-5,-6}},{{7,8,9,10}}};
    unsigned rejections=0;
    const auto reject=[&](const auto& bad) {
        result=sentinel;assert(!planNativeColorViewportRequest(bad,result));
        assert(result.drawOffset==sentinel.drawOffset && result.nativeViewport==sentinel.nativeViewport && result.nativeScissor==sentinel.nativeScissor);
        ++rejections;
    };
    for(const auto& pair:draw.registers) {
        auto bad=draw;bad.registers.erase(pair.first);reject(bad);
        for(unsigned bit=0;bit<32;++bit) {
            bad=draw;bad.registers[pair.first].bits.known^=1u<<bit;reject(bad);
            bad=draw;bad.registers[pair.first].bits.value^=1u<<bit;reject(bad);
        }
    }
    for(unsigned mutation=0;mutation<8;++mutation) {
        auto bad=draw;
        if(mutation==0)bad.offsetWords=0;if(mutation==1)bad.opcode=0x10;
        if(mutation==2)bad.indexCount=3;if(mutation==3)bad.primitive=8;
        if(mutation==4)bad.predicate.state.knownWords=0;
        if(mutation==5)bad.predicate.state.words[0]=0;
        if(mutation==6)bad.predicate.state.knownWords=16;
        if(mutation==7)bad.registers.clear();
        reject(bad);
    }
    auto allowed=draw;allowed.opcode=0x36;allowed.predicate.predicated=false;allowed.predicate.state={};
    allowed.registers[0x4000].bits={}; // Unrelated words are outside this selector.
    assert(planNativeColorViewportRequest(allowed,result));
    assert(validNativeColorCoordinateProfile(NativeColorCoordinateProfile::DiagnosticPositive));
    assert(validNativeColorCoordinateProfile(NativeColorCoordinateProfile::Padded1280W1));
    assert(!validNativeColorCoordinateProfile(static_cast<NativeColorCoordinateProfile>(2)));
    assert(!validNativeColorCoordinateProfile(static_cast<NativeColorCoordinateProfile>(UINT32_MAX)));
    std::printf("NATIVE_COLOR_VIEWPORT: %u rejection controls; 28 known words, output preserved, no GPU/guest work PASS\n",rejections);
}
