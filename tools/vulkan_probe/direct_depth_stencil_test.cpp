// Authored producer/consumer test. No game shader or live owner claim.
#include "device.h"
#include <cassert>
static unsigned pipelineCount=0;
static VkResult inspectDirect(VkDevice d,VkPipelineCache cache,uint32_t count,
        const VkGraphicsPipelineCreateInfo* infos,const VkAllocationCallbacks* allocator,VkPipeline* pipelines) {
    assert(count==1 && infos[0].stageCount==1 && infos[0].pStages[0].stage==VK_SHADER_STAGE_VERTEX_BIT);
    assert(infos[0].pColorBlendState->attachmentCount==0);
    assert(infos[0].pDepthStencilState->depthWriteEnable==VK_FALSE);
    ++pipelineCount;return vkCreateGraphicsPipelines(d,cache,count,infos,allocator,pipelines);
}
#define vkCreateGraphicsPipelines inspectDirect
#include "native_direct_depth_target.h"
#include "native_edram32_bands.h"
#undef vkCreateGraphicsPipelines
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);std::setvbuf(stdout,nullptr,_IONBF,0);
    std::ifstream f(argv[1],std::ios::binary|std::ios::ate);assert(f);
    const auto size=f.tellg();assert(size>0 && size<65536 && size%4==0);f.seekg(0);
    NativeTranslatedDrawProgram p;p.abiVersion=25;p.directTriangles=true;p.stride=36;p.textureSlot=UINT32_MAX;
    p.vertexIdentity={'A','U','T','H'};p.vertexCode.resize(size/4);
    assert(f.read(reinterpret_cast<char*>(p.vertexCode.data()),size));
    ProbeDevice d;ShaderRequirements requirements;assert(d.inspect() && d.create(requirements));
    NativeStencilOnlyPlan plan;plan.colorObject=0x1000;plan.depthObject=0x2000;
    plan.width=plan.pitch=160;plan.height=32;plan.depthBase=100;
    plan.minDepth=1;plan.maxDepth=0;
    plan.state=*decodeXenosDepthStencilState(0x00720213,0x00FFFF20,0x00FFFF00);
    assert(plan.state.depthCompare==1); // 0x13 bits4..6 = strict LESS, not LEQUAL.
    std::vector<std::uint32_t> raw(160*32);
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<160;++x) {
        // Includes zero, denormals, the largest20e4 encoding, and exact
        // depth-test ties. D32 import must preserve all bits on readback.
        const std::array<std::uint32_t,8> z{0,1,0xFFFFF,0x100000,0xE00000,0xE80000,0xF00000,0xFFFFFF};
        raw[y*160+x]=(z[x%8]<<8)|((x+y)%3?32u:37u);
    }
    assert(!NativeDirectDepthTarget::create(d,plan,{}));
    auto bad=plan;bad.height=1030;assert(!NativeDirectDepthTarget::create(d,bad,raw));
    auto target=NativeDirectDepthTarget::create(d,plan,raw);assert(target && target->readWords()==raw);
    std::vector<std::uint8_t> input(8208+3*36);
    const auto put=[&](unsigned at,std::uint32_t v){for(unsigned b=0;b<4;++b)input[at+b]=v>>(24-8*b);};
    put(0,3);const std::array<std::array<float,2>,3> xy{{{-1,-1},{3,-1},{-1,3}}};
    const auto set=[&](unsigned at,float v){std::uint32_t bits;std::memcpy(&bits,&v,4);put(at,bits);};
    for(unsigned i=0;i<3;++i){set(8208+i*36,xy[i][0]);set(8212+i*36,xy[i][1]);set(8216+i*36,.25f);}
    auto invalid=input;invalid[3]=4;
    assert(!target->draw(p,invalid,3,{1,1,0,0},{32,4,64,16}) && target->readWords()==raw);
    assert(!target->draw(p,input,3,{1,1,0,0},{159,4,64,16}) && target->readWords()==raw);
    NativeEdram32Bands physical;
    assert(physical.recordCompletedDepthWords(100,160,160,32,raw,1));
    {NativeEdram32Mutation mutation(physical);
     assert(executeNativeEdram32Stencil(mutation,100,160,160,32,[&](const auto& before) {
        assert(before==raw && !physical.knownTiles());
        assert(target->draw(p,input,3,{1,1,0,0},{32,4,64,16}));return target->readWords();
     }));}
    const auto result=physical.readDepthWords(100,160,160,32);assert(result);
    unsigned changed=0;
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<160;++x) {
        const auto old=raw[y*160+x];
        // Real captured LESS/reverse-Z semantics: incoming .25 -> .75;
        // equality fails too. The earlier .5-only fixture did not test ties.
        const bool fail=x>=32 && x<96 && y>=4 && y<20 && (old&255)==32 && .75f>=*decodeXenosFloat24(old>>8);
        const auto expected=fail?old&0xFFFFFF00:old;
        changed+=fail;
        if((*result)[y*160+x]!=expected)std::printf("DIRECT_DEPTH_MISMATCH x=%u y=%u before=%08x actual=%08x expected=%08x\n",x,y,old,(*result)[y*160+x],expected);
        assert((*result)[y*160+x]==expected);
    }
    assert(changed && pipelineCount==1);
    std::puts("DIRECT_STENCIL_TRANSACTION_GPU_PASS completed_output_rejoins_physical_owner=1 depth_preservation_enforced=1 authored=1 gameplay=0");
    std::printf("DIRECT_DEPTH_STENCIL_GPU_PASS raw_words=5120 depth_exact=1 stencil_exact=1 changed=%u colour_attachments=0 no_clear=1 authored=1 gameplay=0\n",changed);
}
