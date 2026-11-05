#pragma once
#include "native_color_batch_capture.h"
#include "native_color_render_state_fixture.h"
#include "native_color_vertex_geometry_fixture.h"
#include "native_depth_clear_fixture.h"
#include <cassert>
namespace recomp::gpu::test {
// Synthetic command/source data ONLY. Native modules have inert framing, not
// executable instructions. Useful for registry/capture tests, never GPU tests.
struct NativeColorBatchFixture {
    std::vector<std::uint32_t> words;
    std::array<std::uint32_t,8> addresses{};
    std::array<Bytes,8> raw;
    std::optional<NativeColorChainProgram> nativeProgram;
    unsigned copyAddressWord=0;
    NativeColorBatchFixture(unsigned sourceBase,unsigned copyBase,unsigned event0,unsigned event1,bool constantZero=false,bool postOutput=false){
        addresses={{sourceBase,sourceBase+0x1000,sourceBase+0x2000,sourceBase+0x10000,
            sourceBase+0x100000,sourceBase+0x150000,sourceBase+0x1A0000,sourceBase+0x3000}};
        raw[0]=depthClearVertexSource();raw[1]=raw[2]=colorVertexSource();
        raw[3].resize(921600);raw[4].resize(276480);raw[5].resize(276480);raw[6].resize(4096);
        for(auto w:{0xBF000000u,0xBF000000u,0x449FF000u,0xBF000000u,0x449FF000u,0x4433E000u})
            for(unsigned b=0;b<4;++b)raw[7].push_back(w>>(24-b*8));
        const auto module=[](unsigned model){return std::vector<std::uint32_t>{0x07230203,0x10300,0,16,0,
            (2u<<16)|17,1,(5u<<16)|15,model,1,0x6E69616D,0};};
        NativeColorChainProgram::Translations translations;
        for(unsigned i=0;i<5;++i){const unsigned stage=i==2 || i==4?1:0;
            translations[i]={{stage,stage?Bytes(12,static_cast<std::uint8_t>(i)):i?colorVertexShader(i==1):depthClearVertexShader()},module(stage?4:0)};}
        NativeColorChainProgram::Support support;for(auto& code:support)code=module(5);
        nativeProgram=NativeColorChainProgram::create(translations,support,1,NativeColorCoordinateProfile::Padded1280W1);assert(nativeProgram);
        const auto append=[&](std::initializer_list<unsigned> w){words.insert(words.end(),w);};
        const auto shader=[&](unsigned stage,const Bytes& code){
            append({0xC0002B00u|((static_cast<unsigned>(code.size()/4)+1)<<16),stage,static_cast<unsigned>(code.size()/4)});
            for(unsigned i=0;i<code.size();i+=4)words.push_back((unsigned{code[i]}<<24)|(unsigned{code[i+1]}<<16)|(unsigned{code[i+2]}<<8)|code[i+3]);
        };
        const auto state=[&](const DrawRegisterCandidates& regs){for(const auto& r:regs)append({r.first,r.second.bits.value});};
        if(postOutput)append({0x5C8,0x20000,0xF01,0x200E});
        append({0x200D,0,0xC0003B00,0x300});
        auto depth=depthClearStatePlan();if(postOutput)depth.draws[0].registers[0x2182].bits.assign(0x10003);
        state(depth.draws[0].registers);
        append({0x4800,addresses[0]|3,0x4801,0x10000056});shader(0,translations[0].guest.bigEndianCode);shader(1,depth.shaders[1].bigEndianCode);
        append({0xC0003600,0x30088,0xA31,0x10000,0x10A2F,0,0x1000,0xC0043C00,3,0xA31,0,0x80000000,8});
        for(unsigned role=0;role<2;++role){
            state(colorRenderStateDraw(role).registers);
            append({0x48BE,addresses[1+role]|3,0x48BF,0x10000092});
            for(unsigned c:{8u,9u,10u,11u,36u})if(!role || c!=36)for(unsigned w=0;w<4;++w)append({0x4000+c*4+w,0});
            for(unsigned c:role?std::vector<unsigned>{16,62}:std::vector<unsigned>{32,33,34,35,255})
                for(unsigned w=0;w<4;++w)append({0x4400+c*4+w,0});
            for(unsigned i=0;i<(role?1u:3u);++i){
                const std::array<unsigned,6> descriptor=role?(constantZero?std::array<unsigned,6>{0x02024802,2,0,0x00281249,3,0x210}:
                    std::array<unsigned,6>{0x80400002,addresses[6]|0x86,0x3E01F,0xA80C14,3,0x210}):
                    std::array<unsigned,6>{i?0x06024802u:0x0A024802u,addresses[3+i]|2,i?0x002CE27Fu:0x0059E4FFu,0x00281400,3,0x210};
                for(unsigned w=0;w<6;++w)append({0x4800+(role?2:i)*6+w,descriptor[w]});
            }
            shader(0,translations[1+role*2].guest.bigEndianCode);shader(1,translations[2+role*2].guest.bigEndianCode);
            append({0xC0012200,0,0x40086});
        }
        append({0xC0003B00,0x100,0x5C8,0x20000});
        for(const auto& r:std::array<std::array<unsigned,2>,11>{{{{0x2208,6}},{{0x2081,0}},{{0x2082,0x2D00500}},{{0x2301,0}},
            {{0x2000,0x14000500}},{{0x2001,0xC02D0}},{{0x2318,0x100000}},{{0x231A,0x2D00500}},{{0x231B,0x1000300}},
            {{0x4800,addresses[7]|3}},{{0x4801,0x1000001A}}}})append({r[0],r[1]});
        append({0x2319,copyBase});copyAddressWord=static_cast<unsigned>(words.size()-1);
        const Words fetch{{1u<<19,0x3000u|(37u<<16)|0xB48,2}};
        shader(0,program({alloc(1),exec(1,2)|(1u<<16)},{fetch,alu(62)}));
        append({0xC0003600,0x30088,0xC0004600,6,0x5C8,0x20000,0xC0003B00,0x7FFF,0x5C8,0x20000,
            0xC0025800,3,event0|2,11,0xC0025800,3,event1|2,22});
        assert(words.size()>256 && words.size()<=4096);
    }
};
}
