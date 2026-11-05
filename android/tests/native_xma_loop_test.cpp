#include "native_xma_context_pool.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
using namespace recomp::audio;
using recomp::bootstrap::GuestXmaBackend;
namespace {
void be(std::uint8_t* p, unsigned v) { p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v; }
unsigned word(const std::uint8_t* p) {
    return unsigned(p[0])<<24|unsigned(p[1])<<16|unsigned(p[2])<<8|p[3];
}
struct Bits {
    std::vector<std::uint8_t> bytes=std::vector<std::uint8_t>(64);
    unsigned size=0;
    void put(unsigned v,unsigned n) {
        for(unsigned i=n;i;--i,++size) bytes[size/8]|=((v>>(i-1))&1)<<(7-size%8);
    }
};
// Authored no-coefficient frames. The real FFmpeg XMAFRAMES codec decodes these;
// this is not a fake decoder. Tests prove traversal/windows/counts, not audible
// quality or nonzero-sample selection (which requires separate PCM tests).
Bits frame(unsigned channels,bool follows) {
    Bits b;b.put(0,15);b.put(1,1);b.put(0,1); // fixed tile, 512 samples
    if(channels==2)b.put(0,1); // no postprocessing transform
    b.put(0,8);b.put(0,1);b.put(0,2); // DRC, no skip, extension/reserved
    if(channels==2){b.put(0,1);b.put(1,1);b.put(0,1);} // no channel transform
    b.put(0,channels);b.put(0,1);b.put(follows,1);
    const unsigned size=b.size;b.size=0;b.put(size,15);b.size=size;
    return b;
}
void copy(std::vector<std::uint8_t>& dest,unsigned at,const Bits& b,unsigned from,unsigned n) {
    for(unsigned i=0;i<n;++i) {
        const auto v=(b.bytes[(from+i)/8]>>(7-(from+i)%8))&1;
        dest[(at+i)/8]=(dest[(at+i)/8]&~(1U<<(7-(at+i)%8)))|(v<<(7-(at+i)%8));
    }
}
struct Fixture {
    std::vector<std::uint8_t> storage=std::vector<std::uint8_t>(GuestXmaBackend::PoolBytes);
    std::vector<std::uint8_t> input=std::vector<std::uint8_t>(4096);
    std::vector<std::uint8_t> output=std::vector<std::uint8_t>(3072,0xCD);
    NativeXmaContextPool pool{0xA1000000,0x1000000,storage.data(),
        [this](auto a,auto n,bool w)->std::uint8_t* {
            if(!w&&a==0x3000000&&n<=input.size())return input.data();
            if(w&&a==0x2000000&&n<=output.size())return output.data();
            return nullptr;
        }};
    Fixture(){assert(pool.allocate()==0xA1000000);}
    unsigned get(unsigned i)const{return word(storage.data()+4*i);}
    void put(unsigned i,unsigned v){be(storage.data()+4*i,v);}
    void setup(unsigned channels,unsigned loops,unsigned skip,unsigned end,
               unsigned quantum,unsigned padding,bool split=false,bool same=false) {
        const auto a=frame(channels,true),last=frame(channels,false);
        assert(a.size==last.size);
        unsigned start=32,finish=same?start:start+a.size;
        if(!split) {
            input[0]=(same?2:3)<<2;input[2]=1;
            copy(input,start,a,0,a.size);
            if(!same)copy(input,finish,a,0,a.size);
            copy(input,finish+a.size,last,0,last.size);
        } else {
            finish=16377;start=finish-a.size;
            const unsigned relative=start-32;
            input[0]=(2<<2)|(relative>>13);input[1]=relative>>5;input[2]=((relative&31)<<3)|1;
            copy(input,start,a,0,a.size);copy(input,finish,a,0,7);
            const unsigned remaining=a.size-7;
            input[2048]=4;input[2050]=(remaining<<3)|1;
            copy(input,16384+32,a,7,remaining);
            copy(input,16384+32+remaining,last,0,last.size);
        }
        put(0,(10U<<27)|(12U<<22)|(1U<<20)|(loops<<12)|(split?2:1));
        put(1,0x80000000U|((channels-1)<<29)|(padding<<24)|(quantum<<20)|(skip<<17)|(end<<12));
        put(2,start);put(3,start);put(4,finish);put(5,0x3000000);put(7,0x2000000);put(9,10);
    }
    bool kick(){unsigned v=__builtin_bswap32(1U);return pool.access(0x7FEA1940,true,v);}
    void drain(){put(9,(get(0)>>27)&31);put(1,get(1)|0x80000000U);}
    void runFinite(unsigned frames,unsigned blocks) {
        for(unsigned k=0;k<128;++k) {
            const auto before=pool.writtenPcmBytes();
            const auto at=(get(0)>>27)*256;
            assert(kick());
            const auto written=pool.writtenPcmBytes()-before;
            assert(written<=output.size()&&written%256==0);
            for(unsigned i=0;i<written;++i)assert(output[(at+i)%output.size()]==0);
            if(pool.writtenPcmBytes()==blocks*256&&!(get(0)&(3U<<20)))break;
            assert(pool.writtenPcmBytes()<=blocks*256);drain();
        }
        assert(pool.decodedFrames()==frames);
        assert(pool.writtenPcmBytes()==blocks*256);
        assert(!(get(0)&(255U<<12))&&!(get(0)&(3U<<20)));
    }
};
}
int main() {
    unsigned cases=0;
    for(unsigned channels:{1U,2U})for(unsigned skip:{0U,1U,3U,4U})
    for(unsigned end:{0U,2U,3U})for(unsigned quantum:{1U,4U})for(unsigned padding:{0U,1U}) {
        Fixture f;f.setup(channels,2,skip,end,quantum,padding);
        f.runFinite(7,channels*(12+2*(4-skip+end+1)));++cases;
    }
    for(unsigned channels:{1U,2U})for(unsigned skip:{0U,3U,4U}) {
        Fixture f;f.setup(channels,2,skip,0,1,0,true);
        f.runFinite(7,channels*(12+2*(4-skip+1)));++cases;
    }
    for(unsigned channels:{1U,2U}) {
        Fixture f;f.setup(channels,2,1,2,1,0,false,true);
        f.runFinite(4,channels*(3+2+3+4));++cases;
        Fixture infinite;infinite.setup(channels,255,3,0,1,0);
        for(unsigned k=0;k<12;++k) {
            const auto before=infinite.pool.writtenPcmBytes();
            assert(infinite.kick());
            assert(infinite.pool.writtenPcmBytes()==before+infinite.output.size());
            assert(((infinite.get(0)>>12)&255)==255);
            assert(infinite.get(0)&(1U<<20));infinite.drain();
        }
        ++cases;
        // Clear must discard buffered PCM AND the pending loop-restart skip.
        unsigned v=__builtin_bswap32(1U);assert(infinite.pool.access(0x7FEA1A80,true,v));
        const auto framesBefore=infinite.pool.decodedFrames(),bytesBefore=infinite.pool.writtenPcmBytes();
        infinite.setup(channels,0,3,0,1,0);
        for(unsigned k=0;k<8;++k){assert(infinite.kick());if(!(infinite.get(0)&(1U<<20)))break;infinite.drain();}
        assert(infinite.pool.decodedFrames()==framesBefore+3);
        assert(infinite.pool.writtenPcmBytes()==bytesBefore+channels*12*256);++cases;
    }
    for(unsigned invalid:{5U,6U,7U}) {
        Fixture f;f.setup(1,255,invalid,0,1,0);assert(!f.kick());
        assert(f.pool.decodedFrames()==0&&f.pool.writtenPcmBytes()==0);++cases;
    }
    Fixture empty;empty.setup(1,255,3,0,1,0,false,true);assert(!empty.kick());++cases;
    std::printf("NATIVE_XMA_LOOP_PASS cases=%u: real mono/stereo decode; inclusive end; finite/infinite repeats; skip0..4; split header; ring wrap/padding/quantum; reset and invalid guards\n",cases);
}
