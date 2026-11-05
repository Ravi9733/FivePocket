#pragma once
#include "native_depth_clear_request.h"
#include "xenos_copy_shader_fixture.h"
namespace recomp::gpu::test {
inline Bytes depthClearVertexShader(){
    const Words position{{(1u<<12)|(1u<<19),0xA88u|0x3000u|(57u<<16),7}};
    const Words color{{1u<<19,0x688u|0x3000u|(38u<<16)|(1u<<30),3u<<8}};
    return program({alloc(1),alloc(2),exec(2,4)|(5u<<16)},
        {position,color,alu(62,15,0,true,false,1),alu(0)});
}
inline Bytes depthClearVertexSource(){
    Bytes bytes(84);
    const auto put=[&](unsigned at,unsigned word){for(unsigned b=0;b<4;++b)bytes[at+b]=word>>(24-b*8);};
    for(unsigned i=0;i<3;++i){
        put(i*28,i?0x441FE000:0xBF000000);put(i*28+4,i==2?0x43B3C000:0xBF000000);
        put(i*28+8,0x3F400001);put(i*28+24,0x3F800000);
    }
    return bytes;
}
inline DrawCommandPlan depthClearStatePlan(){
    DrawCommandPlan plan;DrawCommandCandidate d;d.offsetWords=100;d.opcode=0x36;d.indexCount=3;d.primitive=8;
    d.predicate={{{{1,0,1,0}},15},true};d.shaders={{0,1}};
    plan.shaders.push_back({1,0,0,true,depthClearVertexShader()});
    plan.shaders.push_back({20,1,0,true,program({alloc(2),exec(1)},{alu(0)})});
    const auto assign=[&](unsigned index,unsigned value){d.registers[index].bits.assign(value);};
    assign(0x2000,0x0A020280);assign(0x2002,720);assign(0x200E,0);assign(0x200F,0x20002000);
    assign(0x2080,0);assign(0x2081,0);assign(0x2082,0x20002000);
    assign(0x2100,0xFFFF);assign(0x2101,0);assign(0x2102,0);assign(0x210D,255u<<16);
    assign(0x2180,0x10010001);assign(0x2181,0);assign(0x2182,0);assign(0x2183,0);assign(0x2184,0);
    assign(0x2200,0x8777);assign(0x2203,0);assign(0x2204,0x10000);assign(0x2205,0x10000);assign(0x2206,0x300);assign(0x2208,5);
    assign(0x2284,0);assign(0x2292,0);assign(0x2293,0);assign(0x2301,0xC003);assign(0x2302,4);assign(0x2312,0xFFFF);
    plan.draws.push_back(d);return plan;
}
}
