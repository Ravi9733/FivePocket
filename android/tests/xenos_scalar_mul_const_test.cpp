#include "xenos_scalar_mul_const.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
namespace {
std::array<std::uint32_t,3> instruction(unsigned source,unsigned ca,unsigned ta,unsigned mask=15){
    return {((42u|(source&1))<<26)|(((source+11)%64)<<8)|(mask<<20),
            (((ca+1)%4)<<6)|(source&0x3C)|ta,(2u<<24)|(((source>>1)&1)<<29)|((source*3)%256)};
}
std::array<std::uint8_t,16> constant(float value){
    std::array<std::uint8_t,16> bytes{};std::uint32_t bits;std::memcpy(&bits,&value,4);
    for(unsigned c=0;c<4;++c)for(unsigned i=0;i<4;++i)bytes[c*4+i]=static_cast<std::uint8_t>(bits>>(24-i*8));
    return bytes;
}
}
int main(){
    ScalarMulConst plan;const auto data=constant(0.5f);const KnownVertexVector temp{{2,4,6,8},15};
    unsigned cases=0;
    for(unsigned r=0;r<64;++r)for(unsigned ca=0;ca<4;++ca)for(unsigned ta=0;ta<4;++ta)for(unsigned mask=0;mask<16;++mask){
        assert(decodeScalarMulConst(instruction(r,ca,ta,mask),plan));
        assert(plan.temporaryRegister==r && plan.constantComponent==ca && plan.temporaryComponent==ta);
        assert(plan.constantRegister==(r*3)%256 && plan.destinationRegister==(r+11)%64 && plan.writeMask==mask);
        float output=1234;assert(evaluateScalarMulConst(plan,data.data(),16,temp,output));assert(output==temp.values[ta]*0.5f);++cases;
    }
    for(const auto& pair:std::array<std::pair<unsigned,unsigned>,11>{{{0,6},{0,7},{0,14},{0,15},{0,24},{0,25},{1,24},{1,27},{1,28},{1,30},{0,28}}}){
        auto words=instruction(0,0,0);words[pair.first]^=1u<<pair.second;
        const auto saved=plan;assert(!decodeScalarMulConst(words,plan));assert(plan.writeMask==saved.writeMask);
    }
    assert(decodeScalarMulConst(instruction(0,0,0,0),plan));
    for(float bad:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::denorm_min()}){
        auto source=constant(bad);float output=1234;
        assert(!evaluateScalarMulConst(plan,source.data(),16,temp,output) && output==1234);
        auto changed=temp;changed.values[0]=bad;
        assert(!evaluateScalarMulConst(plan,data.data(),16,changed,output) && output==1234);
    }
    auto zero=constant(-0.0f);float output=1234;
    assert(evaluateScalarMulConst(plan,zero.data(),16,temp,output));assert(output==0 && !std::signbit(output));
    auto unknown=temp;unknown.known=14;output=1234;
    assert(!evaluateScalarMulConst(plan,data.data(),16,unknown,output) && output==1234);
    assert(!evaluateScalarMulConst(plan,nullptr,16,temp,output) && output==1234);
    for(unsigned size:{0u,15u,17u})assert(!evaluateScalarMulConst(plan,data.data(),size,temp,output) && output==1234);
    auto tiny=constant(std::numeric_limits<float>::min());auto fraction=temp;fraction.values[0]=0.5f;
    assert(!evaluateScalarMulConst(plan,tiny.data(),16,fraction,output) && output==1234);
    auto huge=constant(std::numeric_limits<float>::max());assert(!evaluateScalarMulConst(plan,huge.data(),16,temp,output) && output==1234);
    std::printf("SCALAR_MUL_CONST_V1 addressing_masks=%u coissued_vector_executed=0 gpu_executed=0\n",cases);
}
