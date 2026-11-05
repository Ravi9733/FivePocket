#include "native_depth_clear_fixture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    unsigned rejects=0;auto plan=test::depthClearStatePlan();assert(plan.draws[0].registers.size()==28);
    NativeDepthClearStateRequest state;assert(planNativeDepthClearState(plan,0,state));
    assert(state.drawOffset==100 && state.width==640 && state.height==360 && state.samples==4 && state.sampleMask==15 && !state.stencilReference);
    const auto reject=[&](const auto& bad,unsigned index=0){
        state={1,2,3,4,5,6};assert(!planNativeDepthClearState(bad,index,state));
        assert(state.drawOffset==1 && state.width==2 && state.height==3 && state.samples==4 && state.sampleMask==5 && state.stencilReference==6);++rejects;
    };
    for(const auto& pair:plan.draws[0].registers){
        auto bad=plan;bad.draws[0].registers.erase(pair.first);reject(bad);
        for(unsigned bit=0;bit<32;++bit){
            bad=plan;bad.draws[0].registers[pair.first].bits.known^=1u<<bit;reject(bad);
            bad=plan;bad.draws[0].registers[pair.first].bits.value^=1u<<bit;reject(bad);
        }
    }
    reject(plan,1);reject(plan,UINT32_MAX);
    for(unsigned mutation=0;mutation<13;++mutation){auto bad=plan;auto& d=bad.draws[0];
        if(mutation==0)d.offsetWords=0;if(mutation==1)d.opcode=0x10;if(mutation==2)d.indexCount=4;if(mutation==3)d.primitive=6;
        if(mutation==4)d.predicate.state.knownWords=0;if(mutation==5)d.predicate.state.words[0]=0;
        if(mutation==6)d.shaders[1]=UINT32_MAX;if(mutation==7)bad.shaders[1].stage=0;
        if(mutation==8)bad.shaders[1].offsetWords=d.offsetWords;if(mutation==9)bad.shaders[1].bigEndianCode.clear();
        if(mutation==10)bad.shaders[1].bigEndianCode=test::program({test::alloc(2),test::exec(1)},{test::alu(0,15,0,true,false,1)});
        if(mutation==11)bad.shaders[1].bigEndianCode=test::program({test::alloc(2),test::exec(1)},{test::alu(0,15,15)});
        if(mutation==12)bad.shaders.resize(9);reject(bad);
    }
    auto allowed=plan;allowed.draws[0].registers[0x210F].bits={}; // Disabled viewport scale is not a reset assumption.
    allowed.draws[0].opcode=0x22;assert(planNativeDepthClearState(allowed,0,state));
    assert(state.interpolatorControl==0);
    auto inherited=plan;inherited.draws[0].registers[0x2182].bits.assign(0x10003);
    assert(planNativeDepthClearState(inherited,0,state) && state.interpolatorControl==0x10003);
    for(unsigned bit=0;bit<32;++bit)for(unsigned mode=0;mode<2;++mode){auto bad=inherited;
        if(mode)bad.draws[0].registers[0x2182].bits.value^=1u<<bit;
        else bad.draws[0].registers[0x2182].bits.known^=1u<<bit;reject(bad);}
    auto source=test::depthClearVertexSource();NativeColorShaderIdentity shader{0,test::depthClearVertexShader()};
    NativeColorVertexBinding binding;binding.slot=0;
    binding.descriptor={NativeInputWord{0x1003,0xFFFFFFFF},NativeInputWord{0x10000056,0xFFFFFFFF}};
    binding.source={0x1000,source.data(),source.size()};NativeDepthClearGeometry geometry;
    assert(planNativeDepthClearGeometry(shader,binding,geometry));
    assert(geometry.depthBits==0x3F400001 && geometry.executedInstructions==4);
    assert((geometry.uniformColorBits==std::array<std::uint32_t,4>{0,0,0,0x3F800000}));
    const auto rejectGeometry=[&](const auto& badShader,const auto& badBinding){
        geometry={17,18,{{19,20,21,22}}};assert(!planNativeDepthClearGeometry(badShader,badBinding,geometry));
        assert(geometry.depthBits==17 && geometry.executedInstructions==18 && (geometry.uniformColorBits==std::array<std::uint32_t,4>{19,20,21,22}));++rejects;
    };
    for(unsigned byte=0;byte<source.size();++byte)for(unsigned bit=0;bit<8;++bit){
        source[byte]^=1u<<bit;rejectGeometry(shader,binding);source[byte]^=1u<<bit;
    }
    for(unsigned mutation=0;mutation<7;++mutation){auto badShader=shader;auto badBinding=binding;
        if(mutation==0)badShader.stage=1;if(mutation==1)badShader.bigEndianCode.clear();
        if(mutation==2)badBinding.slot=95;if(mutation==3)badBinding.source.bytes=nullptr;
        if(mutation==4)--badBinding.source.size;if(mutation==5)badBinding.descriptor[0].knownMask=0;
        if(mutation==6)++badBinding.source.physicalAddress;rejectGeometry(badShader,badBinding);
    }
    for(unsigned i=0;i<3;++i)source[i*28+12]=0x80; // Uniform negative zero is conservatively rejected.
    rejectGeometry(shader,binding);
    for(unsigned i=0;i<3;++i){source[i*28+12]=0x41;source[i*28+13]=0x80;} // Uniform color 16, unused in native depth pass.
    assert(planNativeDepthClearGeometry(shader,binding,geometry) && geometry.uniformColorBits[0]==0x41800000);
    std::printf("NATIVE_DEPTH_CLEAR_REQUEST: %u rejection controls; known state, passthrough pixel, whole canonical depth rectangle PASS\n",rejects);
    std::puts("NATIVE_DEPTH_INHERITED_INTERPOLATOR: exact 0/0x10003 requests,64 alternate-state bit refusals,uniform-depth geometry remains required PASS");
}
