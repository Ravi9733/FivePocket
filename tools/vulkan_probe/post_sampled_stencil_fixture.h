#pragma once
#include "native_post_sampled_stencil_rejection.h"
#include "sampled_stencil_fixture.h"

namespace recomp::gpu::test {
struct PostSampledStencilFixture {
    Gta5InlineDraw draw;Gta5BoundSurfaceCapture bindings;NativePostSampledStencilIdentity identity;
    explicit PostSampledStencilFixture(const std::string& path) {
        const auto read=[&](const char* name){return SampledStencilFixture::read(path+"/"+name);};
        const auto word=SampledStencilFixture::word<std::vector<std::uint8_t>>;
        draw.primitive=8;draw.vertexCount=3;draw.stride=24;
        draw.deviceState=read("device-state.bin");assert(draw.deviceState.size()==13860);
        draw.vertices=read("vertices.bin");assert(draw.vertices.size()==72);
        draw.vertexShader=word(draw.deviceState,13072);draw.pixelShader=word(draw.deviceState,13068);
        const auto scissor=read("scissor.bin");assert(scissor.size()==12);
        draw.scissorRegisters=std::array<unsigned,3>{word(scissor,0),word(scissor,4),word(scissor,8)};
        identity.vertex=read("vertex-shader.bin");identity.pixel=read("pixel-shader.bin");
        assert(identity.vertex.size()==96 && identity.pixel.size()==324);
        for(unsigned i=0;i<5;++i){auto& s=bindings.surfaces[i];s.object=word(draw.deviceState,12816+4*i);
            if(s.object)s.header=SampledStencilFixture::read(path+"/surface-"+std::to_string(i)+"-argument.bin");}
    }
};
inline void checkPostSampledStencilConnection(const PostSampledStencilFixture& f,
        const NativeEdram32Bands& producer,unsigned expectedPassing) {
    auto owner=producer;const auto serial=owner.nextSerial();
    const auto color=owner.readColorWords(720,1280,1280,720),depth=owner.readDepthWords(0,1280,1280,720);
    assert(color && depth);
    NativePostSampledStencilProof proof;const char* why=nullptr;
    const auto prove=[&](const auto& current,bool idle=true){return proveNativePostSampledStencilRejection(
        f.draw,f.bindings,f.identity,f.identity.vertex,f.identity.pixel,false,idle,current,proof,&why);};
    const bool result=prove(owner);
    if(result!=(expectedPassing==0))std::printf("POST_SAMPLED_CONNECTION_REFUSED expected_passing=%u stage=%s\n",expectedPassing,why);
    const auto scissor=nativeTranslatedScissor(f.draw);assert(scissor);
    assert(result==(expectedPassing==0) && proof.examined==(*scissor)[2]*(*scissor)[3] && proof.passing==expectedPassing);
    assert(!prove(owner,false));
    {auto unknown=owner;assert(unknown.invalidate(383,1));assert(!prove(unknown));}
    assert(owner.nextSerial()==serial && owner.readColorWords(720,1280,1280,720)==color &&
           owner.readDepthWords(0,1280,1280,720)==depth);
    if(!expectedPassing) {
        bool display=false;
        {NativeEdram32Mutation mutation(owner);
            assert(finishNativeReadOnlyDraw(prove(mutation.candidate()),mutation));}
        assert(!display && owner.nextSerial()==serial && owner.readColorWords(720,1280,1280,720)==color &&
               owner.readDepthWords(0,1280,1280,720)==depth);
    }
}
}
