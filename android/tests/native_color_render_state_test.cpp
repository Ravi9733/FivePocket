#include "native_color_render_state_fixture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    unsigned rejections=0;
    for(unsigned role=0;role<2;++role){
        const auto draw=test::colorRenderStateDraw(role);assert(draw.registers.size()==42);
        NativeColorRenderStateRequest result;
        assert(planNativeColorRenderState(draw,role,result));
        assert(result.role==role && result.edramBaseTile==720 && result.colorFormat==12 && result.samples==1 && result.colorMask==15);
        assert((result.rgbBlend==std::array<unsigned,3>{6,7,0}) && result.alphaBlend==result.rgbBlend);
        assert(result.interpolatorMask==3 && result.centerMask==3 && result.vertexRegisters==4 && result.pixelRegisters==(role?3u:4u));
        const NativeColorRenderStateRequest sentinel{{73,{{1,2,3,4,5,6}},{{7,8,9,10}}},91,92,93,94,95,{{11,12,13}},{{14,15,16}},96,97,98,99};
        const auto reject=[&](const auto& bad,unsigned index){
            result=sentinel;assert(!planNativeColorRenderState(bad,index,result));
            assert(result.viewport.drawOffset==73 && result.viewport.nativeViewport==sentinel.viewport.nativeViewport && result.viewport.nativeScissor==sentinel.viewport.nativeScissor);
            assert(result.role==91 && result.edramBaseTile==92 && result.colorFormat==93 && result.samples==94 && result.colorMask==95);
            assert(result.rgbBlend==sentinel.rgbBlend && result.alphaBlend==sentinel.alphaBlend);
            assert(result.interpolatorMask==96 && result.centerMask==97 && result.vertexRegisters==98 && result.pixelRegisters==99);++rejections;
        };
        for(const auto& pair:draw.registers){
            auto bad=draw;bad.registers.erase(pair.first);reject(bad,role);
            for(unsigned bit=0;bit<32;++bit){
                bad=draw;bad.registers[pair.first].bits.known^=1u<<bit;reject(bad,role);
                bad=draw;bad.registers[pair.first].bits.value^=1u<<bit;reject(bad,role);
            }
        }
        reject(draw,2);reject(draw,UINT32_MAX);reject(draw,role^1);
        auto bad=draw;bad.predicate.state.knownWords=0;reject(bad,role);
        bad=draw;bad.predicate.state.words[0]=0;reject(bad,role);
        bad=draw;bad.indexCount=3;reject(bad,role);
        bad=draw;bad.primitive=8;reject(bad,role);
        bad=draw;bad.opcode=0x10;reject(bad,role);
        bad=draw;bad.offsetWords=0;reject(bad,role);
        auto allowed=draw;allowed.registers[0x210D].bits={}; // Disabled stencil does not consume its reference.
        allowed.registers[0x2105].bits={}; // No constant blend factors.
        allowed.registers[0x4800].bits={}; // Resources belong to a different gate.
        allowed.opcode=0x36;allowed.predicate.predicated=false;allowed.predicate.state={};
        assert(planNativeColorRenderState(allowed,role,result));
    }
    std::printf("NATIVE_COLOR_RENDER_STATE: %u rejection controls; 42 known words, two roles, output preserved PASS\n",rejections);
}
