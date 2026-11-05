#include "xenos_unorm8_vertex_fetch.h"
#include "xenos_float_vertex_fetch.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
using namespace recomp::gpu;
namespace {
std::array<std::uint32_t,3> instruction(unsigned swizzle=0x688) {
    return {1u<<19, (6u<<16)|swizzle, 1};
}
void put(std::array<std::uint8_t,4096>& b,unsigned offset,std::uint32_t value) {
    for(unsigned i=0;i<4;++i)b[offset+i]=static_cast<std::uint8_t>(value>>(24-8*i));
}
bool same(const KnownVertexVector& a,const KnownVertexVector& b) {
    return a.known==b.known && std::memcmp(a.values.data(),b.values.data(),16)==0;
}
}
int main(){
    std::array<std::uint8_t,4096> bytes{};
    Unorm8VertexFetch plan; assert(decodeUnorm8VertexFetch(instruction(),{},plan));
    FloatVertexFetch floatPlan;assert(!decodeFloatVertexFetch(instruction(),{},floatPlan));
    KnownVertexVector sentinel{{11,12,13,14},15};unsigned guards=0;
    for(unsigned lane=0;lane<4;++lane)for(unsigned value=0;value<256;++value){
        put(bytes,0,value<<(lane*8));KnownVertexVector result;
        assert(readUnorm8VertexFetch(plan,bytes.data(),4,0,{},result));
        assert(result.known==15);
        for(unsigned i=0;i<4;++i)assert(result.values[i]==(i==lane?float(value)*(1.0f/255.0f):0.0f));
    }
    put(bytes,0,0x4080FF01);
    for(unsigned swizzle=0;swizzle<4096;++swizzle){
        bool valid=true;for(unsigned i=0;i<4;++i)valid&=((swizzle>>(3*i))&7)!=6;
        const auto saved=plan;
        assert(decodeUnorm8VertexFetch(instruction(swizzle),{},plan)==valid);
        if(!valid){assert(plan.layout.swizzle==saved.layout.swizzle);continue;}
        KnownVertexVector result;assert(readUnorm8VertexFetch(plan,bytes.data(),4,0,sentinel,result));
        for(unsigned i=0;i<4;++i){const auto s=(swizzle>>(3*i))&7;
            const std::array<float,4> expected{{1.0f/255.0f,1.0f,128.0f*(1.0f/255.0f),64.0f*(1.0f/255.0f)}};
            assert(result.values[i]==(s<4?expected[s]:s==7?sentinel.values[i]:float(s-4)));}
    }
    for(const auto& [w,b]:std::array<std::pair<unsigned,unsigned>,17>{{
        {0,0},{0,11},{0,18},{0,19},{1,12},{1,13},{1,14},{1,15},{1,16},
        {1,22},{1,23},{1,24},{1,29},{1,31},{2,30},{2,31},{2,0}}}){
        auto words=instruction();words[w]^=1u<<b;
        const auto saved=plan;assert(!decodeUnorm8VertexFetch(words,{},plan));
        assert(plan.layout.swizzle==saved.layout.swizzle);++guards;
    }
    // Full float and packed mini may share addressing, never the mini's junk operands.
    auto full=std::array<std::uint32_t,3>{(1u<<19)|(31u<<20)|(2u<<25),(57u<<16)|0x3000|0xE88,9};
    assert(decodeFloatVertexFetch(full,{},floatPlan));
    auto mini=instruction(0x60A);mini[0]|=(63u<<5)|(1u<<11)|(3u<<25);
    mini[1]|=1u<<30;mini[2]=6u<<8;
    assert(!decodeUnorm8VertexFetch(mini,{},plan));++guards;
    assert(decodeUnorm8VertexFetch(mini,floatPlan,plan));
    assert(plan.layout.fetchSlot==95 && plan.layout.strideWords==9 && plan.layout.sourceRegister==0);
    for(unsigned i=0;i<4;++i){put(bytes,i*36+24,0xFF000000);KnownVertexVector result;
        assert(readUnorm8VertexFetch(plan,bytes.data(),144,i,{},result));
        assert((result.values==std::array<float,4>{0,0,0,1}));}
    for(unsigned index:{4u,0xFFFFFFu,0x1000000u}){auto result=sentinel;
        assert(!readUnorm8VertexFetch(plan,bytes.data(),144,index,{},result));assert(same(result,sentinel));++guards;}
    for(unsigned size:{0u,3u,24u,4097u}){auto result=sentinel;
        assert(!readUnorm8VertexFetch(plan,bytes.data(),size,0,{},result));assert(same(result,sentinel));++guards;}
    auto result=sentinel;assert(!readUnorm8VertexFetch(plan,nullptr,144,0,{},result));assert(same(result,sentinel));++guards;
    assert(decodeUnorm8VertexFetch(instruction(0xFFF),{},plan));
    assert(readUnorm8VertexFetch(plan,bytes.data(),4,0,{},result));assert(result.known==0);
    auto invalid=sentinel;invalid.values[1]=std::numeric_limits<float>::infinity();result=sentinel;
    assert(!readUnorm8VertexFetch(plan,bytes.data(),4,0,invalid,result));assert(same(result,sentinel));++guards;
    invalid=sentinel;invalid.known=16;assert(!readUnorm8VertexFetch(plan,bytes.data(),4,0,invalid,result));++guards;
    std::printf("UNORM8_VERTEX_FETCH_V1 lane_values=1024 swizzles=4096 guards=%u retail_rounding_verified=0 gpu_executed=0\n",guards);
}
