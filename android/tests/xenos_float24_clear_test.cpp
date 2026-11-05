#include "xenos_float24_clear.h"
#include <cassert>
#include <bit>
#include <cstdio>
#include <limits>
using namespace recomp::gpu;
int main() {
    assert(decodeXenosFloat24(0)==0.f);
    assert(decodeXenosFloat24(1)==0x1p-34f);
    assert(decodeXenosFloat24(0x100000)==0x1p-14f);
    assert(decodeXenosFloat24(0xF00000)==1.f);
    assert(decodeXenosFloat24(0xFFFFFF)==2.f-0x1p-20f);
    assert(!decodeXenosFloat24(0x1000000));
    unsigned count=0;
    // Exhaust every24-bit storage pattern: all are finite and exactly
    // representable in float32, including the subnormal20e4 range.
    for(unsigned word=0;word<=0xFFFFFF;++word) {
        const auto value=decodeXenosFloat24(word);assert(value);
        // Independent IEEE bit construction (no frexp/ldexp or encoder): a
        // subnormal20e4 mantissa's highest set bit has value2^(highest-34).
        unsigned expected=0;
        const auto exponent=word>>20,mantissa=word&0xFFFFF;
        if(exponent)expected=((exponent+112)<<23)|(mantissa<<3);
        else if(mantissa) {
            const unsigned highest=31-std::countl_zero(mantissa);
            expected=((highest+93)<<23)|((mantissa<<(23-highest))&0x7FFFFF);
        }
        assert(std::bit_cast<unsigned>(*value)==expected);
        assert(encodeExactXenosFloat24(*value)==word);++count;
    }
    for(float bad:{-1.f,2.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),
                   0x1p-35f,std::nextafter(1.f,2.f)})assert(!encodeExactXenosFloat24(bad));
    for(unsigned stencil=0;stencil<256;++stencil) {
        const auto zero=planExactXenosFloat24Clear(0,stencil);assert(zero && zero->encoded==0 && zero->stencil==stencil);
        const auto one=planExactXenosFloat24Clear(1,stencil);assert(one && one->encoded==0xF00000);
    }
    assert(!planExactXenosFloat24Clear(.1,0));assert(!planExactXenosFloat24Clear(0,256));
    assert(!planExactXenosFloat24Clear(1.5,0));
    std::printf("FLOAT24_EXACT_CLEAR_PASS encodings=%u stencil_values=256 rounding_not_assumed=1\n",count);
}
