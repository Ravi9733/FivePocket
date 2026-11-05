#include "xenos_linear_r8_plane.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    unsigned pixels=0;
    for(const auto& words:std::array<std::array<std::uint32_t,6>,2>{{
        {0x0A024802,0x10000002,0x0059E4FF,0x00281400,3,0x210},
        {0x06024802,0x11000002,0x002CE27F,0x00281400,3,0x210}}}){
        LinearR8Plane plan;assert(decodeLinearR8Plane(words,plan));
        std::vector<std::uint8_t> source(plan.pitch*plan.height,0xE7),expected;
        for(unsigned y=0;y<plan.height;++y)for(unsigned x=0;x<plan.width;++x){
            const auto value=static_cast<std::uint8_t>((x*7)^(y*13));source[y*plan.pitch+x]=value;
            expected.push_back(value);++pixels;}
        std::vector<std::uint8_t> output{17};assert(unpackLinearR8Plane(plan,source.data(),source.size(),output));
        assert(output==expected);
        for(unsigned size:{0u,4u,plan.pitch*plan.height-4,plan.pitch*plan.height+4}){
            output={17};assert(!unpackLinearR8Plane(plan,source.data(),size,output));assert(output==std::vector<std::uint8_t>{17});}
        assert(!unpackLinearR8Plane(plan,nullptr,source.size(),output));
        for(unsigned word=0;word<6;++word)for(unsigned bit=0;bit<32;++bit){
            auto changed=words;changed[word]^=1u<<bit;LinearR8Plane candidate{};
            if(decodeLinearR8Plane(changed,candidate))assert(word==1 && bit>=12 && bit<29);
        }
        auto forged=plan;forged.pitch=plan.width-1;assert(!unpackLinearR8Plane(forged,source.data(),source.size(),output));
    }
    std::printf("LINEAR_R8_PLANE_V1 checked_pixels=%u row_padding_excluded=1 gpu_executed=0\n",pixels);
}
