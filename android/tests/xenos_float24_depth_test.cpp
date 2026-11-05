#include "xenos_float24_depth.h"
#include <bit>
#include <cassert>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
int main() {
    unsigned cases=0;
    // Every representable depth in[0,1], plus every adjacent binary32 below
    // the next20e4 value: no accidental rounding upward or exponent carry.
    for(unsigned bits=0;bits<=0xF00000;++bits) {
        const auto exact=*decodeXenosFloat24(bits);assert(truncateXenosFloat24Depth(exact)==bits);
        if(bits<0xF00000) {
            const auto next=*decodeXenosFloat24(bits+1);
            assert(truncateXenosFloat24Depth(std::nextafter(next,0.f))==bits);
        }
        ++cases;
    }
    assert(truncateXenosFloat24Depth(-0.f)==0);
    for(float f:{std::numeric_limits<float>::denorm_min(),0x1p-35f,std::nextafter(0x1p-34f,0.f)})
        assert(truncateXenosFloat24Depth(f)==0);
    for(float f:{-1.f,std::nextafter(1.f,2.f),2.f,std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::quiet_NaN()})assert(!truncateXenosFloat24Depth(f));
    // Independent integer view of uniformly stepped IEEE encodings. No
    // frexp/ldexp in this expectation, including the20e4 denormal range.
    for(unsigned bits=0;bits<=0x3F800000;bits+=251) {
        const auto exponent=bits>>23;
        const auto expected=bits>=0x38800000?(bits-0x38000000)>>3:
            exponent<93?0:((bits&0x7FFFFF)|0x800000)>>(116-exponent);
        assert(truncateXenosFloat24Depth(std::bit_cast<float>(bits))==expected);++cases;
    }
    std::printf("FLOAT24_DEPTH_TRUNCATE_PASS cases=%u explicit_truncation=1 xbox_hardware_verified=0\n",cases);
}
