#include "native_raw10_texture_capture.h"
#include "native_stopped_physical32_capture.h"
#include <cassert>
#include <iostream>

using namespace recomp::gpu;
int main() {
    constexpr unsigned address=0x06AA0000,bytes=3768320;
    const std::array<unsigned,6> fetch{0x8A004802,address|0xB6,0x0059E4FF,0x00A86C14,3,0x210};
    unsigned calls=0;
    std::vector<std::uint8_t> expected(bytes);
    for(unsigned i=0;i<bytes;++i)expected[i]=static_cast<std::uint8_t>((i*37u)^(i>>11));
    Gta5ResourceReader reader=[&](unsigned a,unsigned n)->std::optional<std::vector<std::uint8_t>> {
        ++calls;assert(a==address && n==bytes);return expected;
    };
    const auto captured=captureNativeRaw10Base(5,fetch,reader,bytes);
    assert(captured && calls==1 && captured->slot==5 && captured->fetch==fetch &&
           !captured->literalZero && captured->source==expected);
    assert(captured->source.size()>16u*4096); // Not the old unknown-layout prefix.
    const auto d=XenosTextureDescription::decode(captured->fetch);
    assert(d && d->format==54 && d->width==1280 && d->height==720 &&
           d->pitchTexels==1280 && d->exponentBias==3 && d->endian==2);
    // Admission/budget/overflow checks must reject before even asking the reader.
    unsigned refused=0;
    const auto reject=[&](unsigned slot,const auto& f,unsigned budget=bytes) {
        const auto before=calls;assert(!captureNativeRaw10Base(slot,f,reader,budget));
        assert(calls==before);++refused;
    };
    reject(32,fetch);reject(5,fetch,bytes-1);
    for(unsigned i=0;i<6;++i) {auto f=fetch;f[i]^=1;reject(5,f);}
    auto zero=fetch;zero[1]=0xB6;reject(5,zero);
    auto overflow=fetch;overflow[1]=0x1FFFF0B6;reject(5,overflow);
    auto alias=fetch;alias[1]=0xA6AA00B6;reject(5,alias);
    assert(!captureNativeRaw10Base(5,fetch,{}));
    Gta5ResourceReader missing=[](unsigned,unsigned)->std::optional<std::vector<std::uint8_t>> {return std::nullopt;};
    Gta5ResourceReader prefix=[](unsigned,unsigned)->std::optional<std::vector<std::uint8_t>> {return std::vector<std::uint8_t>(65536);};
    Gta5ResourceReader oversized=[](unsigned,unsigned n)->std::optional<std::vector<std::uint8_t>> {return std::vector<std::uint8_t>(n+1);};
    assert(!captureNativeRaw10Base(5,fetch,missing));
    assert(!captureNativeRaw10Base(5,fetch,prefix));
    assert(!captureNativeRaw10Base(5,fetch,oversized));
    std::vector<std::uint8_t> state(13860),rgba(1280*720*4);
    const auto put=[&](unsigned at,unsigned v){for(unsigned b=0;b<4;++b)state[at+b]=v>>(24-8*b);};
    put(10368,0x14000500);put(10372,0x302D0);put(10376,0x10000);
    std::vector<unsigned> color(1280*720),depth(color.size());
    for(unsigned i=0;i<color.size();++i) {
        color[i]=i*19u;depth[i]=(i*1237u)^0xC5381234;
        for(unsigned b=0;b<4;++b)rgba[4*i+b]=color[i]>>(8*b);
    }
    NativeEdram32Bands owner;
    assert(!captureNativeStoppedPhysical32(owner,state));
    assert(owner.recordCompletedRawColorRows(720,1280,1280,720,rgba,owner.nextSerial()));
    assert(!captureNativeStoppedPhysical32(owner,state));
    assert(owner.recordCompletedDepthWords(0,1280,1280,720,depth,owner.nextSerial()));
    const auto serial=owner.nextSerial();
    const auto snapshot=captureNativeStoppedPhysical32(owner,state);
    assert(snapshot && snapshot->serial==serial-1 && snapshot->color==color && snapshot->depth==depth);
    assert(owner.nextSerial()==serial && owner.readColorWords(720,1280,1280,720)==color &&
           owner.readDepthWords(0,1280,1280,720)==depth);
    for(unsigned offset:{10368u,10372u,10376u}) {
        auto wrong=state;wrong[offset+3]^=1;assert(!captureNativeStoppedPhysical32(owner,wrong));
    }
    auto shortState=state;shortState.pop_back();assert(!captureNativeStoppedPhysical32(owner,shortState));
    for(unsigned tile:{0u,719u,720u,1439u}) {
        auto incomplete=owner;assert(incomplete.invalidate(tile,1));
        assert(!captureNativeStoppedPhysical32(incomplete,state));
    }
    std::cout<<"RAW10_TEXTURE_CAPTURE_PASS bytes="<<bytes<<" pre_read_refusals="<<refused
             <<" partial_missing_oversized_refused=1 physical_snapshot_exact=1 ownership_unchanged=1 sampling_admission=0\n";
}
