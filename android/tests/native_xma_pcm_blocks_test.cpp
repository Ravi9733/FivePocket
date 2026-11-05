#include "native_xma_pcm_blocks.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace recomp::audio;
int main() {
    unsigned cases=0;
    for(unsigned channels:{1U,2U}) {
        NativeXmaFrameDecoder::Frame frame;frame.channels=channels;frame.samples=512;
        for(unsigned c=0;c<channels;++c)for(unsigned s=0;s<512;++s)
            frame.planar[c][s]=float(int(s)-256)*(c?-1:1)/128.0f;
        for(unsigned first=0;first<=channels*4;++first)
        for(unsigned count=0;count<=channels*4-first;++count)
        for(unsigned start:{0U,256U,1792U}) {
            std::vector<std::uint8_t> ring(2048,0xCD);unsigned write=start;
            assert(writeXmaPcmBlocks(frame,first,count,ring.data(),ring.size(),write));
            assert(write==(start+count*256)%ring.size());
            for(unsigned i=0;i<ring.size();i+=2) {
                const auto distance=(i+ring.size()-start)%ring.size();
                if(distance>=count*256){assert(ring[i]==0xCD&&ring[i+1]==0xCD);continue;}
                const auto index=first*128+distance/2;
                const auto s=index/channels,c=index%channels;
                const float f=frame.planar[c][s];
                const int expected=f<=-1?-32767:f>=1?32767:int(f*32767);
                assert((unsigned(ring[i])<<8|ring[i+1])==std::uint16_t(expected));
            }
            ++cases;
        }
        std::vector<std::uint8_t> untouched(2048,0xCD);unsigned at=0;
        assert(!writeXmaPcmBlocks(frame,channels*4+1,1,untouched.data(),2048,at));
        assert(!writeXmaPcmBlocks(frame,channels*4,1,untouched.data(),2048,at));
        assert(!writeXmaPcmBlocks(frame,0,1,untouched.data(),255,at));
        at=1;assert(!writeXmaPcmBlocks(frame,0,1,untouched.data(),2048,at));
        for(auto b:untouched)assert(b==0xCD);
    }
    std::printf("XMA_PCM_BLOCK_WINDOWS_PASS cases=%u: distinct nonzero stereo ramps; exact sample selection/interleaving/BE16/clipping/wrap and reject-before-write\n",cases);
}
