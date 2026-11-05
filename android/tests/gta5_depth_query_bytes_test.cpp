#include "gta5_depth_query_bytes.h"
#include "native_visibility_queries.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace recomp::gpu;
// Authored ABI/lifecycle inputs. No fabricated hardware measurements, guest
// writes, retirement or installation authority are supplied by this test.
int main() {
    std::array<std::uint8_t,64> bytes{};
    const auto put=[&](unsigned at,std::uint32_t value) {
        for(unsigned b=0;b<4;++b)bytes[at+b]=std::uint8_t(value>>(b*8));
    };
    for(unsigned i=0;i<8;++i) {put(i*4,1000+i*101);put(32+i*4,700+i*99);}
    const auto decoded=decodeGta5DepthQueryBytes(bytes);assert(decoded);
    assert((decoded->rawConsumerDelta==std::array<std::uint32_t,4>{618,610,626,602}));
    assert(!decoded->originalPendingSentinel);
    assert(decoded->begin.values[0][0]==700 && decoded->end.values[0][0]==1000);
    // Distinct byte lanes detect accidental host/native or PPC endianness.
    put(0,0x01020304);assert(decodeGta5DepthQueryBytes(bytes)->end.values[0][0]==0x01020304);
    for(unsigned i=0;i<8;++i) {put(i*4,0xFFFFFFFEu+i);put(32+i*4,0xFFFFFFFCu+i);}
    assert((decodeGta5DepthQueryBytes(bytes)->rawConsumerDelta==std::array<std::uint32_t,4>{4,4,4,4}));
    for(unsigned i=0;i<8;++i)put(i*4,0xFFFFFFFCu+i);
    assert((decodeGta5DepthQueryBytes(bytes)->rawConsumerDelta==std::array<std::uint32_t,4>{0,0,0,0}));
    // An empty query is a zero DIFFERENCE, not permission to zero old totals.
    assert(decodeGta5DepthQueryBytes(bytes)->begin.values[0][0]==0xFFFFFFFCu);
    put(16,0xEDFEFFFFu);put(20,0xEDFEFFFFu);
    assert(bytes[16]==0xFF && bytes[18]==0xFE && bytes[19]==0xED);
    assert(decodeGta5DepthQueryBytes(bytes)->originalPendingSentinel);
    // The original specifically tests both sentinels; do not silently change
    // the predicate, or equate a single changed word with fence completion.
    put(20,0);assert(!decodeGta5DepthQueryBytes(bytes)->originalPendingSentinel);
    put(16,0xFFFFFEEDu);put(20,0xFFFFFEEDu);
    assert(!decodeGta5DepthQueryBytes(bytes)->originalPendingSentinel);
    for(unsigned n=0;n<=96;++n)if(n!=64)
        assert(!decodeGta5DepthQueryBytes(std::vector<std::uint8_t>(n)));
    for(unsigned n=0;n<=64;++n)if(n!=32)
        assert(!decodeXenosDepthSampleCounts(std::vector<std::uint8_t>(n)));
    // Keep the two query mechanisms separate. A decoded zero sample delta
    // cannot provide the earlier potential-visibility measurement.
    NativeVisibilityQueries visibility;NativeVisibilityQueries::Journal journal;
    assert(visibility.command(10,journal));const auto before=journal;
    assert(!visibility.draw(0x15,[]{return std::optional<NativeVisibilityQueries::Measurement>{
        {false,NativeVisibilityQueries::Stage::FinalSamples}};}));
    assert(journal==before && !visibility.command(0x10A,journal));
    puts("GTA5_DEPTH_QUERY_BYTES_PASS endian=le end_offset=0 begin_offset=32 counters=4 wrapping=1 pending_not_completion=1 query_stages_separate=1 execution=0");
}
