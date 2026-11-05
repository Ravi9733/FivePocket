#pragma once
// v319 (stage 1, shadow mode; measurement only): the game's D3D dirty words (device +0..+39: five big-endian u64, MAP.md section b,
// local-analysis/d3d-map) against the words our capture's state diff finds changed between two captured draws. bitOf() maps a
// byte offset of the captured state ranges ([1152,1928), [10112,10864), [12816,12836)) to the dirty bit that covers it; the
// draw entries' snapshots (taken before the commit consumes them) are OR'd until the next capture. Host test:
// tests/dirty_shadow_v319_test.cpp (mutants RECOMP_V319_MUTANT_SLOT / RECOMP_V319_MUTANT_REG must fail).
#include <cstdint>
namespace v319 {
struct Dirty {
    std::uint64_t w[5]{};
    void orIn(const Dirty& o) noexcept {for(int i=0;i<5;++i)w[i]|=o.w[i];}
    bool fetchClean() const noexcept {return (w[3]&0xFFFFFFFFull)==0;}                 // no fetch slot dirty (b31..b0)
    bool shaderClean() const noexcept {return ((w[2]>>17)&0xFull)==0;}                  // no shader re-commit (b17..b20)
    bool constantsOnly() const noexcept {return !w[2] && !w[3] && !w[4];}
};
inline Dirty readDirty(const std::uint8_t* device) noexcept {
    Dirty d;
    for(int i=0;i<5;++i){std::uint64_t v=0;for(int b=0;b<8;++b)v=(v<<8)|device[8*i+b];d.w[i]=v;}
    return d;
}
enum Region : unsigned {Fetch=0,VsConst=1,BoolLoop=2,Plane=3,Reg=4,Surface=5,Gap=6,Regions=7};
struct Bit {unsigned word=0,bit=0;bool tracked=false;Region region=Gap;};
inline Bit bitOf(unsigned off) noexcept {
    if(off>=1152 && off<1920) {
        const unsigned s=(off-1152)/24;
#if defined(RECOMP_V319_MUTANT_SLOT)
        return {3,(32-s)&63,true,Fetch};
#else
        return {3,31-s,true,Fetch};                       // fetch slot s = dirty[3] b(31-s)
#endif
    }
    if(off>=1920 && off<1920+4096){const unsigned k=(off-1920)/16;return {0,63-k/4,true,VsConst};} // VS c[4j..4j+3] = dirty[0] b(63-j)
    if(off>=10112 && off<10272)return {4,56,true,BoolLoop};                                         // bool/loop block = dirty[4] b56
    if(off>=10272 && off<10368){const unsigned k=(off-10272)/16;return {4,54-k,true,Plane};}        // clip plane k = dirty[4] b(54-k)
    struct Block {unsigned offset,count,word,top;};
    // register blocks: the first register's bit, then one lower per register (MAP.md b, = gta5DrawRegisterOffset's layout)
    constexpr Block blocks[]{{10368,16,2,57},{10444,21,2,41},{10528,5,2,16},{10548,12,2,11},
                             {10596,21,3,54},{10680,38,4,37},{10832,8,4,45}};
    for(const auto& b:blocks)if(off>=b.offset && off<b.offset+4*b.count) {
        const unsigned r=(off-b.offset)/4;
#if defined(RECOMP_V319_MUTANT_REG)
        return {b.word,b.top-b.count+1+r,true,Reg};       // (the block's bits in the wrong order)
#else
        return {b.word,b.top-r,true,Reg};
#endif
    }
    if(off>=12816 && off<12836)return {0,0,false,Surface}; // bound surfaces: no dirty bit of their own
    return {0,0,false,Gap};
}
inline bool covered(const Bit& b,const Dirty& d) noexcept {return b.tracked && ((d.w[b.word]>>b.bit)&1u);}
}
