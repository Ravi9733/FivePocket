#include "guest_vmx_short.h"

#include <array>
#include <bit>
#include <cassert>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma STDC FENV_ACCESS ON

static unsigned unpackChecks, packChecks;

static std::uint32_t packedReference(std::uint32_t bits) {
    float value = std::bit_cast<float>(bits);
    constexpr float low = 3.0f - 32767.0f * 0x1p-22f;
    constexpr float high = 3.0f + 32767.0f * 0x1p-22f;
    value = value >= low ? value : low;
    value = value <= high ? value : high;
    const auto q = static_cast<int>((value - 3.0f) * 0x1p22f);
    return static_cast<std::uint32_t>(q) & 0xffffu;
}

template<unsigned Mask, unsigned Shift>
static void checkPack(const std::uint32_t (&src)[4]) {
    const auto xy = (packedReference(src[3]) << 16) | packedReference(src[2]);
    const auto zw = (packedReference(src[1]) << 16) | packedReference(src[0]);
    // Explicit host destination lane sources: 0 = preserved, 1 = ZW, 2 = XY.
    constexpr unsigned controls[3][4][4] = {
        {{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}},
        {{1,2,0,0},{0,1,2,0},{0,0,1,2},{0,0,0,1}},
        {{1,2,0,0},{0,1,2,0},{0,0,1,2},{2,0,0,0}}};
    struct Guarded {std::uint32_t before, words[4], after;};
    Guarded dst{0xcafebabeu,{0x11223344u,0xaabbccddu,0x76543210u,0x55667788u},0xdeadbeefu};
    const auto original = dst;
    std::uint32_t copy[4]; std::memcpy(copy,src,16);
    std::feclearexcept(FE_ALL_EXCEPT); std::feraiseexcept(FE_DIVBYZERO);
    const int flags=std::fetestexcept(FE_ALL_EXCEPT), rounding=std::fegetround();
    recomp::vmx::packNormShort4<Mask,Shift>(dst.words,copy);
    assert(std::memcmp(copy,src,16)==0);
    recomp::vmx::packNormShort4<Mask,Shift>(copy,copy);
    assert(std::fetestexcept(FE_ALL_EXCEPT)==flags && std::fegetround()==rounding);
    for(unsigned c=0;c<4;++c) {
        const auto control=controls[Mask-1][Shift][c];
        assert(dst.words[c]==(control==1 ? zw : control==2 ? xy : original.words[c]));
        assert(copy[c]==(control==1 ? zw : control==2 ? xy : src[c]));
    }
    assert(dst.before==original.before && dst.after==original.after);
    ++packChecks;
}

static void checkPacks(const std::uint32_t (&src)[4]) {
    checkPack<1,0>(src);checkPack<1,1>(src);checkPack<1,2>(src);checkPack<1,3>(src);
    checkPack<2,0>(src);checkPack<2,1>(src);checkPack<2,2>(src);checkPack<2,3>(src);
    checkPack<3,0>(src);checkPack<3,1>(src);checkPack<3,2>(src);checkPack<3,3>(src);
}

static void checkUnpack(std::uint32_t xy, std::uint32_t zw) {
    std::uint32_t src[4]={zw,xy,0xcafebabeu,0xabcdef01u}, dst[4];
    recomp::vmx::unpackNormShort4(dst,src);
    const unsigned fields[4]={xy>>16,xy&65535u,zw>>16,zw&65535u};
    for(unsigned c=0;c<4;++c) {
        const int q=fields[c]<32768 ? int(fields[c]) : int(fields[c])-65536;
        const auto expected=q==-32768 ? 0x7fc00000u : std::bit_cast<std::uint32_t>(3.0f+float(q)*0x1p-22f);
        assert(dst[3-c]==expected);
    }
    assert(src[0]==zw && src[1]==xy && src[2]==0xcafebabeu && src[3]==0xabcdef01u);
    recomp::vmx::unpackNormShort4(src,src);
    assert(std::memcmp(src,dst,16)==0);
    ++unpackChecks;
}

int main() {
    // Published instr_vpkd3d128.s / instr_vupkd3d128.s SHORT4 vectors.
    std::uint32_t packed[4]={0,0,0xcdcdcdcdu,0xcdcdcdcdu};
    const std::uint32_t input[4]={0x40400000u,0x4040007fu,0x403ffff8u,0x403f8001u};
    recomp::vmx::packNormShort4<2,0>(packed,input);
    assert(packed[0]==0x007f0000u && packed[1]==0x8001fff8u && packed[2]==0xcdcdcdcdu && packed[3]==0xcdcdcdcdu);
    std::uint32_t v[4]={0x007ffff8u,0x7fffffffu,0xcdcdcdcdu,0xcdcdcdcdu};
    recomp::vmx::unpackNormShort4(v,v);
    assert(v[3]==0x40407fffu && v[2]==0x403fffffu && v[1]==0x4040007fu && v[0]==0x403ffff8u);
    for(unsigned q=0;q<65536;++q)
        checkUnpack((q<<16)|((q*17+31)&65535u), (((q*37+13)&65535u)<<16)|((q*7+1)&65535u));
    for(int q=-32769;q<=32769;++q) {
        std::uint32_t src[4];
        for(unsigned c=0;c<4;++c) src[c]=std::bit_cast<std::uint32_t>(3.0f+float(c%2 ? -q : q)*0x1p-22f);
        checkPacks(src);
    }
    for(int rounding:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        assert(std::fesetround(rounding)==0);
        for(unsigned exponent=0;exponent<256;++exponent)
        for(unsigned sign=0;sign<2;++sign)
        for(unsigned fraction:{0u,1u,0x3fffffu,0x400000u,0x7fffffu}) {
            const auto value=(sign<<31)|(exponent<<23)|fraction;
            const std::uint32_t src[4]={value,0x403fc001u,value,0x40404000u};
            checkPacks(src);
        }
    }
    assert(std::fesetround(FE_TONEAREST)==0);
    assert(unpackChecks==65536 && packChecks==909348);
    std::printf("VMX_NORMSHORT4_BIASED_PACK_UNPACK_ALIAS_MASK_FP_PASS %u %u\n",unpackChecks,packChecks);
}
