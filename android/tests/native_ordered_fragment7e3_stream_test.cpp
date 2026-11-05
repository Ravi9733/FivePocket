#include "native_ordered_fragment7e3_stream.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    constexpr unsigned capacity=4;
    std::vector<unsigned> original(8+capacity*10,OrderedFragment7e3Guard);
    original[4]=3;original[5]=0;
    for(unsigned i=0;i<3;++i) {
        const unsigned at=8+i*10;
        original[at]=2-i;original[at+1]=91;
        for(unsigned c=0;c<4;++c)original[at+2+c]=colorBits(.5f);
        original[at+6]=original[at+8]=colorBits(.5f);
        original[at+7]=*truncateXenosFloat24Depth(.5f);
    }
    const auto decoded=decodeNativeOrderedFragment7e3Stream(original,capacity,3);
    assert(decoded && decoded->fragments.size()==3);
    for(unsigned i=0;i<3;++i)assert(decoded->fragments[i].primitive==i && decoded->fragments[i].pixel==91);
    for(unsigned kind=0;kind<17;++kind) {
        auto words=original;unsigned cap=capacity,primitives=3;
        if(kind==0)cap=0;
        if(kind==1)cap=OrderedFragment7e3MaxCapacity+1;
        if(kind==2)primitives=0;
        if(kind==3)primitives=65536;
        if(kind==4)words.pop_back();
        if(kind==5)words[0]^=1;
        if(kind==6)words[4]=capacity+1;
        if(kind==7)words[5]=8;
        if(kind==8)words[8]=3;
        if(kind==9)words[9]=1280*720;
        if(kind==10)words[10]=0x7FC00000;
        if(kind==11)words[14]=colorBits(-1.f);
        if(kind==12)words[15]^=1;
        if(kind==13)words[16]=colorBits(1.f);
        if(kind==14)words[17]^=1;
        if(kind==15)words[38]=0; // write outside completed prefix
        if(kind==16)words[18]=words[8]; // duplicated same sample in one primitive
        assert(!decodeNativeOrderedFragment7e3Stream(words,cap,primitives));
    }
    auto empty=std::vector<unsigned>(8+capacity*10,OrderedFragment7e3Guard);empty[4]=empty[5]=0;
    assert(decodeNativeOrderedFragment7e3Stream(empty,capacity,3)->fragments.empty());
    std::puts("ORDERED_FRAGMENT_STREAM_PASS arbitrary_append_order=1 duplicates_refused=1 overflow_refused=1 guards=1 negatives=17 live_draw=0");
}
