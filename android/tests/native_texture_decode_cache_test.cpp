#include "native_texture_decode_cache.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
static XenosTextureDescription texture(unsigned w,unsigned h,unsigned pitch,unsigned format,bool tiled,unsigned endian=0) {
    const std::array<std::uint32_t,6> fetch{2u|((pitch/32)<<22)|(tiled?0x80000000u:0),
        0x10000u|format|(endian<<6),(w-1)|((h-1)<<13),0x688,0,0x200};
    return *XenosTextureDescription::decode(fetch);
}
int main() {
    unsigned cases=0;std::uint32_t random=0x84BD7631;
    for(unsigned format:{2u,6u,18u,20u})for(bool tiled:{false,true})for(unsigned endian=0;endian<4;++endian)
    for(unsigned width:{1u,7u,32u,33u}) {
        const auto d=texture(width,35,(width+31)&~31u,format,tiled,endian);
        const auto plan=planXenosTexture2D(d);assert(plan);
        std::vector<std::uint8_t> source(plan->sourceBytes);
        for(auto& b:source){random=random*1664525u+1013904223u;b=random>>24;}
        const auto original=source;const auto expected=linearizeXenosTexture2D(d,source);assert(expected);
        NativeTextureDecodeCache cache;const auto a=cache.linearize(d,source);assert(a && *a==*expected && source==original);
        assert(cache.linearize(d,source)==a && cache.entryCount()==1);
        // Same source address, changed contents MUST miss and freshly decode.
        source[0]^=0x7F;const auto changed=cache.linearize(d,source);
        assert(changed && changed!=a && *changed==*linearizeXenosTexture2D(d,source));
        assert(cache.linearize(d,original)==a && *a==*expected);
        // Same bytes, changed encoded descriptor also MUST miss. Address is
        // included even though it doesn't alter this CPU conversion's output.
        auto alternate=d;alternate.fetch[1]+=0x1000;
        const auto relocated=cache.linearize(alternate,original);
        assert(relocated && relocated!=a && *relocated==*expected);
        auto invalid=d;invalid.fetch[1]=(invalid.fetch[1]&~63u)|19;
        const auto retained=cache.retainedBytes();assert(!cache.linearize(invalid,original));
        auto shortSource=original;shortSource.pop_back();assert(!cache.linearize(d,shortSource));
        assert(cache.retainedBytes()==retained && cache.linearize(d,original)==a);
        assert(cache.retainedBytes()<=32u*1024*1024 && cache.entryCount()<=8);++cases;
    }
    const auto d=texture(4,4,32,18,false);std::vector<std::uint8_t> source(8,0);
    NativeTextureDecodeCache small(72,8),one(1024,1),disabled(0,0);
    const auto old=small.linearize(d,source),oldOne=one.linearize(d,source),oldDisabled=disabled.linearize(d,source);
    assert(old && oldOne && oldDisabled);
    source[0]=1;const auto next=small.linearize(d,source);assert(next && next!=old);
    assert(small.retainedBytes()==72 && small.entryCount()==1 && old->size()==64);
    assert(one.linearize(d,source)!=oldOne && one.entryCount()==1);
    assert(disabled.linearize(d,source) && !disabled.retainedBytes() && !disabled.entryCount());
    NativeTextureDecodeCache bounded;for(unsigned i=0;i<32;++i){source[0]=i;assert(bounded.linearize(d,source));}
    assert(bounded.entryCount()==8 && bounded.retainedBytes()==8*72);
    std::puts("NATIVE_TEXTURE_DECODE_CACHE_PASS exact_source_and_fetch=1 stale_address_rejected=1 invalid_inputs_rejected=1 bounded_eviction=1 retained_outputs_immutable=1");
    std::printf("NATIVE_TEXTURE_DECODE_CACHE_FORMAT_CASES=%u\n",cases);
}
