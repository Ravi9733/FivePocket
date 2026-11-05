#include "native_xma_context_pool.h"
#include "guest_execution_guard.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace recomp::audio;
using namespace recomp::bootstrap;
static void be(std::uint8_t* p,std::uint32_t v) {p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static std::uint32_t word(const std::uint8_t* p) {return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];}
static void bits(std::uint8_t* p,unsigned at,const std::uint8_t* data,unsigned from,unsigned count) {
    for(unsigned i=0;i<count;++i) {
        const auto v=(data[(from+i)/8]>>(7-(from+i)%8))&1;
        p[(at+i)/8]=(p[(at+i)/8]&~(1U<<(7-(at+i)%8)))|(v<<(7-(at+i)%8));
    }
}
int main() {
    constexpr std::uint32_t V=0xA1000000,P=0x1000000;
    std::vector<std::uint8_t> storage(GuestXmaBackend::PoolBytes,0xAB), output(2048,0xCD),input(2048),other(4096);
    NativeXmaContextPool pool(V,P,storage.data(),[&](auto address,auto bytes,bool write)->std::uint8_t* {
        if (address==0x2000000 && bytes<=output.size()) return output.data();
        if (!write && address==0x3000000 && bytes<=input.size()) return input.data();
        if (!write && address==0x4000000 && bytes<=other.size()) return other.data();
        return nullptr;
    });
    assert(pool.ready());
    std::vector<std::uint32_t> contexts;
    for (unsigned i=0;i<320;++i) { auto c=pool.allocate(); assert(c==V+64*i); contexts.push_back(c); }
    assert(pool.allocate()==0);
    assert(!pool.release(V+1)); assert(pool.release(V+64)); assert(!pool.release(V+64));
    assert(pool.allocate()==V+64);
    std::uint32_t value=0;
    assert(pool.access(0x7FEA1800,false,value) && value==__builtin_bswap32(P));
    assert(!pool.access(0x7FEA1801,false,value));
    assert(pool.access(0x7FEA1804,false,value) && value==__builtin_bswap32(3U));
    assert(!pool.access(0x7FEA1808,false,value));
    // Real clear masks only documented bits, preserving other context fields.
    be(storage.data(),0xFFFFFFFF); be(storage.data()+4,0xFFFFFFFF); be(storage.data()+36,0xFFFFFFFF);
    value=__builtin_bswap32(1U); assert(pool.access(0x7FEA1A80,true,value));
    assert(word(storage.data())==(0xFFFFFFFFU&~((3U<<20)|(31U<<27))));
    assert(word(storage.data()+4)==0x7FFFFFFF); assert(word(storage.data()+36)==0xFFFFFFE0);
    std::fill(storage.begin(),storage.begin()+64,0);
    // Empty input is actual starvation, not invented PCM or completion data.
    // The output buffer stays valid: with write==read, a set bit31 means an
    // EMPTY ring to the title, a clear one a FULL ring.
    be(storage.data(),8U<<22); be(storage.data()+4,0x80000000U); be(storage.data()+28,0x2000000);
    assert(pool.access(0x7FEA1940,true,value));
    assert(word(storage.data()+4)&0x80000000);
    for (auto b:output) assert(b==0xCD);
    assert(pool.decodedFrames()==0 && pool.writtenPcmBytes()==0);
    // Invalid physical input is rejected, with no arbitrary output mutation.
    be(storage.data(),(8U<<22)|(1U<<20)|1U); be(storage.data()+4,0x80000000U);
    be(storage.data()+20,0xDEAD0000);
    assert(!pool.access(0x7FEA1940,true,value));
    assert(pool.failure()=="XMA input physical allocation unavailable");
    // Authored mono silent XMA frame, derived from FFmpeg's documented decoder
    // bit grammar: 15-bit length=31, fixed tile, full 512-sample subframe,
    // DRC=0, no skip/extension/transform/coefficients, two zero trailer bits.
    // This validates actual codec output, not a replacement silence backend.
    const std::uint8_t silent[]{0,0x3F,0,0};
    input[0]=4; input[2]=1; bits(input.data(),32,silent,0,31);
    be(storage.data()+20,0x3000000);
    assert(pool.access(0x7FEA1940,true,value));
    assert(pool.decodedFrames()==1 && pool.writtenPcmBytes()==1024);
    for(unsigned i=0;i<1024;++i) assert(output[i]==0);
    for(unsigned i=1024;i<2048;++i) assert(output[i]==0xCD);
    assert(((word(storage.data())>>27)&31)==4);
    // Split the 15-bit frame header across two noncontiguous input buffers.
    // The first packet also asks to skip an interleaved unrelated packet.
    for (const bool additionalFrame : {false,true}) {
    assert(pool.access(0x7FEA1A80,true,value));
    std::fill(input.begin(),input.end(),0); std::fill(other.begin(),other.end(),0);
    const unsigned offset=16377-32;
    input[0]=4|(offset>>13); input[1]=(offset>>5)&255; input[2]=((offset&31)<<3)|1; input[3]=1;
    bits(input.data(),16377,silent,0,7);
    other[2048+2]=1; bits(other.data()+2048,32,silent,7,24);
    if (additionalFrame) {
        // A frame whose trailer is zero can span into a packet that has NEW
        // frames. Its trailer must not skip those frames in the next packet.
        other[2048]=4; other[2048+2]=(24<<3)|1;
        bits(other.data()+2048,56,silent,0,31);
    }
    be(storage.data(),(8U<<22)|(3U<<20)|1U);
    be(storage.data()+4,0x80000000U|2U); be(storage.data()+8,0); be(storage.data()+16,0);
    be(storage.data()+20,0x3000000); be(storage.data()+24,0x4000000);
    assert(pool.access(0x7FEA1940,true,value));
    assert(pool.decodedFrames()==(additionalFrame?4U:2U));
    assert(pool.writtenPcmBytes()==(additionalFrame?4096U:2048U));
    }
    // A count of one is a block quantum, not a one-block-per-kick ceiling.
    // Decode two frames into an initially empty ring whose offsets wrap.
    assert(pool.access(0x7FEA1A80,true,value));
    std::fill(input.begin(),input.end(),0); std::fill(output.begin(),output.end(),0xCD);
    input[0]=8;input[2]=1;
    const std::uint8_t first[]{0,0x3F,0,2};
    bits(input.data(),32,first,0,31); bits(input.data(),63,silent,0,31);
    be(storage.data(),(6U<<27)|(8U<<22)|(1U<<20)|1U);
    be(storage.data()+4,0x80000000U|(1U<<20));be(storage.data()+8,0);
    be(storage.data()+16,0);be(storage.data()+36,6);
    std::uint32_t control=__builtin_bswap32(2U);
    assert(pool.access(0x7FEA1804,true,control));
    assert(pool.access(0x7FEA1940,true,value));
    assert(pool.decodedFrames()==4 && pool.writtenPcmBytes()==4096);
    for(auto b:output) assert(b==0xCD);
    control=__builtin_bswap32(3U); assert(pool.access(0x7FEA1804,true,control));
    assert(pool.decodedFrames()==6 && pool.writtenPcmBytes()==6144);
    assert(((word(storage.data())>>27)&31)==6 && !(word(storage.data()+4)>>31));
    for(auto b:output) assert(b==0);
    // Wire the actual PPC endian/scalar seam, including unknown and partial
    // MMIO accesses. Null RAM proves accepted MMIO never falls through to RAM.
    assert(bindGuestXmaRegisters([&](auto a,bool w,std::uint32_t& v){return pool.access(a,w,v);}));
    GuestExecutionGuard guard;
    const auto result=guard.run(100,[&]{
        assert(__builtin_bswap32(loadGuestScalar<std::uint32_t>(nullptr,0x7FEA1800))==P);
        storeGuestScalar<std::uint32_t>(nullptr,0x7FEA1A40,__builtin_bswap32(1U));
    });
    assert(result.reason==GuestExecutionStopReason::Completed);
    const auto unknown=guard.run(100,[&]{(void)loadGuestScalar<std::uint32_t>(nullptr,0x7FEA1808);});
    assert(unknown.reason==GuestExecutionStopReason::UnsupportedScalarAccess);
    resetGuestXmaRegisters();
    for (auto c:contexts) assert(pool.release(c));
    for (auto b:storage) assert(b==0);
    std::puts("NATIVE_XMA_POOL_PASS: 320 real contexts; actual authored-frame PCM decode; split header/noncontiguous buffers/interleaved skip; MMIO/ownership guards; no game PCM claim");
}
