#include "xenos_geometry_vertex_fetch.h"
#include "xenos_vertex_arithmetic.h"
#include "xenos_copy_shader_fixture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
namespace {
Words fetch(unsigned format,unsigned destination=0,bool mini=false,unsigned offset=0) {
    return {(1u<<19)|(destination<<12),(format<<16)|(format==7?0x1000:0x3000)|
        (format==31?0xB08u:0x688u)|(unsigned(mini)<<30),(offset<<8)|(mini?0u:2u)};
}
void put(Bytes& data,unsigned at,std::uint32_t w){for(unsigned i=0;i<4;++i)data.at(at+i)=w>>(24-8*i);}
bool same(const KnownVertexVector& a,const KnownVertexVector& b) {
    return a.known==b.known && std::memcmp(a.values.data(),b.values.data(),16)==0;
}
}
int main() {
    assert(std::fesetround(FE_TONEAREST)==0);
    Bytes data(256*1024);GeometryVertexFetch plan;KnownVertexVector output;
    const KnownVertexVector sentinel{{11,12,13,14},15};
    assert(decodeGeometryVertexFetch(fetch(7),{},plan));
    for(unsigned lane=0;lane<4;++lane) {
        const auto width=lane==3?2u:10u,shift=lane==3?30u:lane*10;
        for(unsigned raw=0;raw<(1u<<width);++raw) {
            put(data,0,raw<<shift);assert(readGeometryVertexFetch(plan,data.data(),8,0,{},output));
            const int value=raw<(1u<<(width-1))?int(raw):int(raw)-int(1u<<width);
            volatile float reference=float(value)*(lane==3?1.f:1.f/511.f);
            for(unsigned i=0;i<4;++i)assert(output.values[i]==(i==lane?std::max(-1.f,float(reference)):0.f));
        }
    }
    assert(decodeGeometryVertexFetch(fetch(31),{},plan));
    for(unsigned half=0;half<65536;++half)for(unsigned lane=0;lane<2;++lane) {
        put(data,0,half<<(16*lane));output=sentinel;
        const bool valid=((half>>10)&31)!=31;
        assert(readGeometryVertexFetch(plan,data.data(),8,0,{},output)==valid);
        if(!valid){assert(same(output,sentinel));continue;}
        // Independent host compiler half conversion as a finite-domain oracle.
        static_assert(sizeof(_Float16)==2);const auto bits=static_cast<std::uint16_t>(half);
        _Float16 reference;std::memcpy(&reference,&bits,2);
        const auto expected=static_cast<float>(reference);
        assert(vertexFloatBits(output.values[lane])==vertexFloatBits(expected));
        assert(output.values[1-lane]==0 && output.values[2]==0 && output.values[3]==1);
    }
    for(unsigned format:{7u,31u})for(const auto& [word,bit]:std::array<std::pair<unsigned,unsigned>,16>{{
        {0,0},{0,11},{0,18},{0,19},{1,12},{1,13},{1,14},{1,15},{1,22},{1,24},{1,25},{1,29},{1,31},{2,30},{2,31},{2,1}}}) {
        auto bad=fetch(format);bad[word]^=1u<<bit;const auto old=plan;
        assert(!decodeGeometryVertexFetch(bad,{},plan));
        assert(plan.format==old.format && plan.layout.swizzle==old.layout.swizzle);
    }
    assert(decodeGeometryVertexFetch(fetch(32),{},plan));
    for(unsigned lane=0;lane<4;++lane)for(unsigned half:{0u,1u,0x3C00u,0x7BFFu,0x8000u,0xFBFFu,0x7C00u,0xFFFFu}) {
        put(data,0,0);put(data,4,0);put(data,(lane/2)*4,half<<((lane%2)*16));output=sentinel;
        const bool valid=(half&0x7C00)!=0x7C00;
        assert(readGeometryVertexFetch(plan,data.data(),8,0,{},output)==valid);
        if(!valid){assert(same(output,sentinel));continue;}
        const auto bits=static_cast<std::uint16_t>(half);_Float16 reference;std::memcpy(&reference,&bits,2);
        assert(vertexFloatBits(output.values[lane])==vertexFloatBits(static_cast<float>(reference)));
    }
    output=sentinel;assert(!readGeometryVertexFetch(plan,data.data(),4,0,{},output));assert(same(output,sentinel));
    const auto half4Code=program({alloc(1),alloc(2),exec(2,3)|(1u<<16)},
        {fetch(32),alu(62),alu(0)});
    put(data,0,0xC0003C00);put(data,4,0x80000001);
    VertexArithmeticInputs half4Input;half4Input.vertices=data.data();half4Input.vertexBytes=8;
    VertexArithmeticResult half4Result;VertexArithmeticError half4Error;
    assert(!evaluateVertexArithmetic(half4Code.data(),half4Code.size(),half4Input,0,half4Result,half4Error));
    half4Input.packedGeometry=true;
    assert(evaluateVertexArithmetic(half4Code.data(),half4Code.size(),half4Input,0,half4Result,half4Error));
    assert((half4Result.exports[62].values==std::array<float,4>{1,-2,0x1p-24f,-0.f}));
    assert(vertexFloatBits(half4Result.exports[62].values[3])==0x80000000);
    assert(decodeGeometryVertexFetch(fetch(7),{},plan));
    auto mini=fetch(31,1,true,1);mini[0]|=(63u<<5)|(1u<<11)|(3u<<25);
    GeometryVertexFetch second;assert(!decodeGeometryVertexFetch(mini,{},second));
    assert(decodeGeometryVertexFetch(mini,plan.layout,second));
    assert(second.layout.sourceRegister==0 && second.layout.strideWords==2);
    put(data,8192,511|(512<<10)|(1u<<30));put(data,8196,0xC0003C00);
    assert(readGeometryVertexFetch(second,data.data(),data.size(),1024,{},output));
    assert((output.values==std::array<float,4>{1,-2,0,1}));
    for(unsigned size:{0u,3u,8199u,256u*1024+4}) {
        output=sentinel;assert(!readGeometryVertexFetch(second,data.data(),size,1024,{},output));
        assert(same(output,sentinel));
    }
    output=sentinel;assert(!readGeometryVertexFetch(second,nullptr,8,0,{},output));assert(same(output,sentinel));
    assert(!readGeometryVertexFetch(second,data.data(),data.size(),0x1000000,{},output));
    second.layout.swizzle[0]=6;assert(!readGeometryVertexFetch(second,data.data(),data.size(),0,{},output));
    const auto code=program({alloc(1),alloc(2),exec(2,4)|(5u<<16)},
        {fetch(7),fetch(31,1,true,1),alu(62),alu(0,15,0,true,false,1)});
    VertexArithmeticInputs input;input.vertices=data.data();input.vertexBytes=data.size();
    VertexArithmeticResult result;VertexArithmeticError error;
    assert(!evaluateVertexArithmetic(code.data(),code.size(),input,1024,result,error));
    input.packedGeometry=true;
    assert(evaluateVertexArithmetic(code.data(),code.size(),input,1024,result,error));
    assert((result.exports[62].values==std::array<float,4>{1,-1,0,1}));
    assert((result.exports[0].values==std::array<float,4>{1,-2,0,1}));
    assert(result.fetchCount==2 && result.exports[62].known==15 && result.exports[0].known==15);
    input.vertexBytes=8;input.packedGeometry=false;
    assert(!evaluateVertexArithmetic(code.data(),code.size(),input,0,result,error));
    input.packedGeometry=true;put(data,4,0x7C003C00);
    const auto old=result;
    assert(!evaluateVertexArithmetic(code.data(),code.size(),input,0,result,error));
    assert(result.exports[62].values==old.exports[62].values && result.exports[0].values==old.exports[0].values);
    std::puts("GEOMETRY_VERTEX_FETCH_PASS snorm_patterns=3076 finite_half_domain=1 connected_exports=1 default_unchanged=1 gpu_executed=0");
}
