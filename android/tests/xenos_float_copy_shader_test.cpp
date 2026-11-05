#include "xenos_copy_shader.h"
#include "xenos_copy_shader_fixture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
static Words fetch(unsigned format=37,unsigned swizzle=0xB48,unsigned stride=2,unsigned offset=0,
        bool mini=false,unsigned dst=0,unsigned src=0) {
    return {{(src<<5)|(dst<<12)|(1<<19),swizzle|0x3000|(format<<16)|(unsigned(mini)<<30),(offset<<8)|stride}};
}
int main() {
    const auto basic=program({alloc(1),exec(1,2)|(1u<<16)},{fetch(),alu(62)});
    FloatCopyShaderProof proof;CopyShaderError error;
    assert(analyzeFloatCopyShader(basic.data(),basic.size(),proof,error));
    assert(proof.fetchCount==1 && proof.executedCount==2 && proof.exported.count()==1 && proof.exported[62]);
    assert((proof.exports[62]==CopyShaderProof::Vector{{CopyShaderProof::Fetch,CopyShaderProof::Fetch+1,CopyShaderProof::One,CopyShaderProof::One}}));
    assert(proof.fetches[0].indexExpression==CopyShaderProof::Initial && proof.initialComponentsRead.count()==1);
    CopyShaderProof noFetch;assert(!analyzeCopyShader(basic.data(),basic.size(),CopyShaderStage::Vertex,noFetch,error));
    const auto saved=proof;unsigned rejects=0;
    const auto reject=[&](const Bytes& bad,CopyShaderFailure failure) {
        assert(!analyzeFloatCopyShader(bad.data(),bad.size(),proof,error) && error.failure==failure);
        assert(proof.exports==saved.exports && proof.fetchCount==saved.fetchCount && proof.byteCount==saved.byteCount);++rejects;
    };
    for (unsigned bit:{0u,11u,18u,19u}) {auto bad=fetch();bad[0]^=1u<<bit;
        reject(program({alloc(1),exec(1,2)|(1u<<16)},{bad,alu(62)}),CopyShaderFailure::UnsupportedFetch);}
    for (unsigned bit:{14u,15u,22u,24u,31u}) {auto bad=fetch();bad[1]^=1u<<bit;
        reject(program({alloc(1),exec(1,2)|(1u<<16)},{bad,alu(62)}),CopyShaderFailure::UnsupportedFetch);}
    for (const auto& bad:{fetch(6),fetch(37,0xB48,0),fetch(37,0xB48,2,0,true),fetch(37,0xB48,2,0x400000)})
        reject(program({alloc(1),exec(1,2)|(1u<<16)},{bad,alu(62)}),CopyShaderFailure::UnsupportedFetch);
    // Cache-hint bits are accepted only by explicit float analysis, not the old
    // no-fetch consumer. Their presence does not execute or complete a cache op.
    auto hints=program({alloc(1),exec(1,2)|(1u<<16)|(std::uint64_t{63}<<28)},{fetch(),alu(62)});
    assert(analyzeFloatCopyShader(hints.data(),hints.size(),proof,error));
    assert(!analyzeCopyShader(hints.data(),hints.size(),CopyShaderStage::Vertex,noFetch,error));
    // Full fetch overwrites r0.x, mini MUST retain the saved original index;
    // the following full fetch sees the newly overwritten symbolic expression.
    const auto mini=program({alloc(1),exec(1,4)|(0x15u<<16)},
        {fetch(),fetch(37,0xB48,255,2,true,1,63),fetch(37,0xB48,2,0,false,2,0),alu(62,15,0,true,false,1)});
    assert(analyzeFloatCopyShader(mini.data(),mini.size(),proof,error) && proof.fetchCount==3);
    assert(proof.fetches[1].indexExpression==CopyShaderProof::Initial && proof.fetches[1].layout.strideWords==2);
    assert(proof.fetches[2].indexExpression==CopyShaderProof::Fetch);
    // KEEP lanes preserve earlier literal/symbolic state; ALU swizzles snapshot
    // all source lanes simultaneously, including fetched rather than initial bits.
    unsigned swizzles=0;
    for (unsigned swizzle=0;swizzle<256;++swizzle) {
        const auto data=program({alloc(1),exec(1,3)|(1u<<16)},
            {fetch(38,0x688,4),alu(0,15,0,false,false,0,swizzle),alu(62)});
        assert(analyzeFloatCopyShader(data.data(),data.size(),proof,error));
        for (unsigned lane=0;lane<4;++lane) assert(proof.exports[62][lane]==CopyShaderProof::Fetch+(((swizzle>>(2*lane))+lane)&3));
        ++swizzles;
    }
    const auto keep=program({alloc(1),exec(1,2)|(1u<<16)},{fetch(37,0xFFF),alu(62)});
    assert(analyzeFloatCopyShader(keep.data(),keep.size(),proof,error));
    for (unsigned i=0;i<4;++i) assert(proof.exports[62][i]==CopyShaderProof::Initial+i);
    // At most 32 executed fetches, even when EXEC rereads the same body.
    std::vector<std::uint64_t> controls{alloc(1)};
    for (unsigned i=0;i<5;++i) controls.push_back(exec(4,6,1)|(0x555u<<16));
    controls.push_back(exec(4,3)|(5u<<16));
    std::vector<Words> body{fetch(37,0xB48,2,0,false,1),fetch(37,0xB48,2,0,false,1),alu(62,15,0,true,false,1),fetch(37,0xB48,2,0,false,1),fetch(37,0xB48,2,0,false,1),fetch(37,0xB48,2,0,false,1)};
    // First five controls mark every instruction as fetch, so use an all-fetch
    // shared body and a separate final ALU outside those repeated ranges.
    body.assign(6,fetch(37,0xB48,2,0,false,1));body.push_back(alu(62,15,0,true,false,1));
    controls.back()=exec(8,3)|(5u<<16); // Two final fetches at8/9, ALU at10.
    auto boundary=program(controls,body);
    assert(analyzeFloatCopyShader(boundary.data(),boundary.size(),proof,error) && proof.fetchCount==32);
    controls.back()=exec(7,4)|(0x15u<<16);boundary=program(controls,body);
    assert(!analyzeFloatCopyShader(boundary.data(),boundary.size(),proof,error) && error.failure==CopyShaderFailure::FetchLimit);++rejects;
    // 10,000 deterministic hostile instruction streams under the opt-in parser.
    std::uint32_t seed=0xAB1234EF;
    for (unsigned n=0;n<10000;++n) {Bytes data((n%340+1)*12);
        for (auto& b:data) {seed=seed*1664525u+1013904223u;b=seed>>24;}
        (void)analyzeFloatCopyShader(data.data(),data.size(),proof,error);
    }
    std::printf("FLOAT_COPY_SHADER: %u explicit rejections, %u fetched ALU swizzles, 10000 hostile streams PASS\n",rejects,swizzles);
}
