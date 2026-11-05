#include "xenos_straight_line_shader_identity.h"
#include <cassert>
#include <cstdio>
using Bytes=std::vector<std::uint8_t>;
using recomp::gpu::sameXenosStraightLineShader;
void word(Bytes& bytes,unsigned at,std::uint32_t v) {
    assert(at+4<=bytes.size());for(unsigned i=0;i<4;++i)bytes[at+i]=v>>(24-8*i);
}
void pair(Bytes& bytes,unsigned at,std::uint64_t a,std::uint64_t b) {
    word(bytes,at,a);word(bytes,at+4,(a>>32)|((b&0xFFFF)<<16));word(bytes,at+8,b>>16);
}
std::uint64_t exec(unsigned address,unsigned count,bool end) {
    return (std::uint64_t{end?2u:1u}<<44)|(std::uint64_t{count}<<12)|address;
}
Bytes shader(unsigned first=2,unsigned last=3) {
    Bytes bytes(84,0xA5);
    pair(bytes,0,exec(first,1,false),(12ULL<<44)|(2ULL<<41));
    pair(bytes,12,exec(last,1,true),0);
    return bytes;
}
int main() {
    auto original=shader();assert(sameXenosStraightLineShader(original,original));
    unsigned reachable=0,unused=0;
    // Every bit of every control/instruction byte remains part of identity.
    for(unsigned at=0;at<original.size();++at)for(unsigned bit=0;bit<8;++bit) {
        auto changed=original;changed[at]^=1u<<bit;
        const bool expect=at>=48;
        assert(sameXenosStraightLineShader(original,changed)==expect);
        assert(sameXenosStraightLineShader(changed,original)==expect);
        if(expect)++unused;else ++reachable;
    }
    // Noncontiguous EXEC ranges: a gap may differ, a later referenced body may not.
    auto gap=shader(3,5),other=gap;other[25]^=1;other[50]^=1;other[80]^=1;
    assert(sameXenosStraightLineShader(gap,other));
    other[61]^=1;assert(!sameXenosStraightLineShader(gap,other));
    for(unsigned opcode:{3u,4u,5u,6u,7u,8u,9u,10u,11u,13u,14u,15u}) {
        auto unknown=original;pair(unknown,12,(std::uint64_t{opcode}<<44)|(1u<<12)|3u,0);
        other=unknown;other.back()^=1;assert(!sameXenosStraightLineShader(unknown,other));
        // Exact old matches are not reinterpreted as newly supported programs.
        assert(sameXenosStraightLineShader(unknown,unknown));
    }
    for(unsigned invalid=0;invalid<8;++invalid) {
        auto bad=original;
        switch(invalid) {
        case 0:pair(bad,12,exec(3,1,false),0);break; // Unterminated.
        case 1:pair(bad,12,exec(0,1,true),0);break; // CF/body overlap.
        case 2:pair(bad,12,exec(6,2,true),0);break; // Body outside extent.
        case 3:pair(bad,12,exec(3,0,true),0);break;
        case 4:pair(bad,12,exec(3,7,true),0);break;
        case 5:pair(bad,12,exec(3,1,true)|(4ULL<<16),0);break; // Sequence overflow.
        case 6:pair(bad,0,1,0);break; // Noncanonical NOP.
        case 7:pair(bad,0,exec(2,1,false),(12ULL<<44));break; // Memory ALLOC.
        }
        other=bad;other.back()^=1;assert(!sameXenosStraightLineShader(bad,other));
    }
    assert(!sameXenosStraightLineShader({},{}));
    other=original;other.pop_back();assert(!sameXenosStraightLineShader(original,other));
    auto shortCode=Bytes(4);other=shortCode;other[0]=1;assert(!sameXenosStraightLineShader(shortCode,other));
    auto large=Bytes(4104);other=large;other.back()=1;assert(!sameXenosStraightLineShader(large,other));
    // Control budget guard even for identical canonical prefix ending after128 CFs.
    auto longControl=Bytes(816);pair(longControl,64*12,exec(65,1,true),0);
    other=longControl;other.back()=1;assert(!sameXenosStraightLineShader(longControl,other));
    std::printf("XENOS_STRAIGHT_LINE_IDENTITY_PASS reachable_bit_mutations=%u unused_bit_mutations=%u gap_reference_checked=1 unsupported_control_rejected=1 exact_matches_preserved=1 GPU=0\n",reachable,unused);
}
