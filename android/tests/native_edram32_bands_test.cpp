#include "native_edram32_bands.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    NativeEdram32Bands bands;
    NativeOffscreenDrawPlan p{0x1000,160,32,160,5,4};
    std::vector<std::uint8_t> bytes(160*32*4);
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<160;++x) {
        const unsigned value=(y<<16)|(x<<8)|(x^y);
        for(unsigned c=0;c<4;++c)bytes[4*(y*160+x)+c]=value>>(8*c);
    }
    assert(!bands.readDepthWords(5,160,160,32));
    assert(bands.recordCompletedColorBand(p,bytes,1) && bands.knownTiles()==4);
    auto result=bands.readDepthWords(5,160,160,32);assert(result);
    const auto colour=bands.readColorWords(5,160,160,32);assert(colour);
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<160;++x) {
        const unsigned sx=x/80*80+(x%80+40)%80;
        assert((*result)[y*160+x]==((y<<16)|(sx<<8)|(sx^y)));
        assert((*colour)[y*160+x]==((y<<16)|(x<<8)|(x^y)));
    }
    assert(!bands.recordCompletedColorBand(p,bytes,1));
    for(unsigned invalid=0;invalid<6;++invalid) {
        auto bad=p;
        if(invalid==0)bad.format=4;if(invalid==1)bad.pitch=240;
        if(invalid==2)bad.height=31;if(invalid==3)bad.baseTile=2046;
        if(invalid==4)bad.width=159;if(invalid==5)bad.tileCount=3;
        assert(!bands.recordCompletedColorBand(bad,bytes,2));
    }
    assert(bands.knownTiles()==4 && bands.readDepthWords(5,160,160,32)==result);
    assert(!bands.readDepthWords(5,160,160,48)); // Adjacent bytes are unknown.
    assert(bands.invalidate(7,1));assert(bands.knownTiles()==3);
    assert(!bands.readDepthWords(5,160,160,32));
    assert(!bands.readColorWords(5,160,160,32));
    assert(bands.readDepthWords(5,160,160,16));
    assert(bands.recordCompletedColorBand(p,bytes,2));
    assert(bands.readDepthWords(5,160,160,32)==result);
    assert(!bands.invalidate(2047,2) && bands.knownTiles()==0);
    assert(!bands.readDepthWords(5,160,160,32));
    assert(bands.recordCompletedColorBand(p,bytes,1));
    bands.reset();assert(!bands.readDepthWords(5,160,160,16));
    assert(bands.recordCompletedColorBand(p,bytes,1));
    // The global owner is already unknown when the submission starts.
    {
        NativeEdram32Mutation mutation(bands);
        assert(!bands.knownTiles());
        assert(executeNativeEdram32ColorBand(mutation,p,[&]{assert(!bands.knownTiles());return true;},
            [&]{assert(!bands.knownTiles());return std::optional{bytes};}));
        assert(bands.readDepthWords(5,160,160,32)==result);
    }
    // A stencil consumer publishes back into the very same raw owner. It
    // cannot be a private GPU image that later consumers never observe.
    auto changed=*result;for(auto& value:changed)value=(value&0xFFFFFF00u)|32u;
    {
        NativeEdram32Mutation mutation(bands);
        assert(executeNativeEdram32Stencil(mutation,5,160,160,32,[&](const auto& before) {
            assert(before==*result && !bands.knownTiles());return std::optional{changed};
        }));
    }
    assert(bands.readDepthWords(5,160,160,32)==std::optional{changed});
    {
        NativeEdram32Mutation mutation(bands); // verified disjoint writer
        assert(mutation.candidate().invalidate(10,1));mutation.commit();mutation.commit();
    }
    assert(bands.readDepthWords(5,160,160,32)==std::optional{changed});
    {
        NativeEdram32Mutation mutation(bands);
        assert(!executeNativeEdram32Stencil(mutation,5,160,160,32,[&](const auto& before) {
            auto corrupt=before;corrupt[0]^=0x100u;return std::optional{corrupt};
        }));
    }
    assert(!bands.knownTiles()); // partial/incorrect GPU result is not restored
    assert(bands.recordCompletedColorBand(p,bytes,1));
    {NativeEdram32Mutation mutation(bands);} // unknown writer/early return
    assert(!bands.knownTiles());
    assert(bands.recordCompletedColorBand(p,bytes,1));
    try {NativeEdram32Mutation mutation(bands);throw 7;}catch(int){}
    assert(!bands.knownTiles());
    unsigned readbacks=0;
    {
        NativeEdram32Mutation mutation(bands);
        assert(!executeNativeEdram32ColorBand(mutation,p,[]{return false;},[&]{++readbacks;return std::optional{bytes};}));
    }
    assert(!bands.knownTiles() && !readbacks);
    {
        NativeEdram32Mutation mutation(bands);
        assert(!executeNativeEdram32ColorBand(mutation,p,[]{return true;},[&]{return std::optional<std::vector<std::uint8_t>>{};}));
    }
    assert(!bands.knownTiles());
    // Partial DONT_CARE replacement invalidates its whole allocation, then
    // publishes only the proved full-width tile-row prefix. Never padding.
    const std::vector<std::uint8_t> prefix(bytes.begin(),bytes.begin()+160*16*4);
    assert(bands.recordCompletedColorBand(p,bytes,1));
    {NativeEdram32Mutation mutation(bands);
     assert(executeNativeEdram32ColorRows(mutation,p,[]{return true;},[&]{return std::optional{prefix};},[]{return 16u;}));}
    assert(bands.knownTiles()==2 && bands.readDepthWords(5,160,160,16));
    assert(!bands.readDepthWords(7,160,160,16) && !bands.readDepthWords(5,160,160,32));
    for(unsigned rows:{0u,15u,33u}) {
        {NativeEdram32Mutation mutation(bands);
         assert(!executeNativeEdram32ColorRows(mutation,p,[]{return true;},[&]{return std::optional{prefix};},[&]{return rows;}));}
        assert(!bands.knownTiles());
    }
    {NativeEdram32Mutation mutation(bands);
     assert(!executeNativeEdram32ColorRows(mutation,p,[]{return true;},[&]{return std::optional{bytes};},[]{return 16u;}));}
    assert(!bands.knownTiles()); // full-size bytes must not masquerade as prefix
    std::puts("EDRAM32_PARTIAL_ROWS_PASS covered_prefix_only=1 discarded_tail_unknown=1 invalid_rows_and_bytes_refused=1 gpu=0");
    // Padding tiles in a pitch are not invented by depth publication.
    NativeEdram32Bands pitched;
    const std::vector<std::uint32_t> narrow(80*32,0x12345620u);
    assert(pitched.recordCompletedDepthWords(0,160,80,32,narrow,1));
    assert(pitched.knownTiles()==2 && pitched.readDepthWords(0,160,80,32)==std::optional{narrow});
    assert(!pitched.readDepthWords(0,160,160,32));
    assert(!pitched.readColorWords(0,160,160,32));
    assert(!pitched.recordCompletedDepthWords(0,160,80,32,narrow,1));
    assert(!pitched.recordCompletedDepthWords(2047,160,80,32,narrow,2));
    assert(!pitched.recordCompletedDepthWords(0,160,79,32,narrow,2));
    // Two partial writers can establish a tile, but neither padding nor an
    // untouched row may be inferred from a GPU attachment's allocated extent.
    NativeEdram32Bands partial;
    const std::vector<std::uint8_t> half(80*8*4,0x12),otherHalf(80*8*4,0x34);
    assert(partial.recordCompletedRawColorRows(6,160,80,8,half,1));
    assert(partial.knownTiles()==0 && !partial.readColorWords(6,160,80,16));
    {NativeEdram32Mutation mutation(partial);assert(!partial.knownTiles());mutation.commit();}
    assert(partial.recordCompletedRawColorRows(6,160,80,8,otherHalf,2,8));
    assert(partial.knownTiles()==1);
    const auto whole=partial.readColorWords(6,160,80,16);assert(whole);
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<80;++x)
        assert((*whole)[y*80+x]==(y<8?0x12121212u:0x34343434u));
    assert(!partial.readColorWords(6,160,160,16)); // second tile never touched
    // A new partial replacement retains the previously known untouched rows.
    assert(partial.recordCompletedRawColorRows(6,160,80,8,half,3,8));
    const auto replaced=partial.readColorWords(6,160,80,16);assert(replaced);
    assert(std::all_of(replaced->begin(),replaced->end(),[](auto w){return w==0x12121212u;}));
    // Invalidation also destroys sub-tile provenance; subsequent half writes
    // must not resurrect the old other half through stale masks.
    assert(partial.invalidate(6,1));
    assert(partial.recordCompletedRawColorRows(6,160,80,8,half,4));
    assert(!partial.readColorWords(6,160,80,16));partial.reset();
    assert(partial.recordCompletedRawColorRows(6,160,80,8,otherHalf,1,8));
    assert(!partial.readColorWords(6,160,80,16));
    const std::vector<std::uint8_t> notATile(79*16*4,0x56);
    partial.reset();assert(partial.recordCompletedRawColorRows(6,160,79,16,notATile,1));
    assert(!partial.readColorWords(6,160,80,16)); // missing rightmost column
    assert(!partial.recordCompletedRawColorRows(2047,160,80,32,bytes,2));
    assert(!partial.recordCompletedRawColorRows(6,160,80,8,half,2,UINT32_MAX));
    assert(!partial.recordCompletedRawColorRows(6,160,80,8,half,1));
    std::puts("EDRAM32_PARTIAL_WORDS_PASS two_halves_join=1 untouched_rows_preserved=1 unknown_padding_refused=1 invalidate_and_reset_masks=1 overflow_refused=1 gpu=0");
    std::puts("EDRAM32_TRANSACTION_PASS before_submit_unknown=1 failures_unknown=1 depth_preserved=1 stencil_writeback=1 padding_unknown=1 gpu=0");
    std::puts("EDRAM32_BANDS_PASS full_tiles=4 half_tile_permutation=1 partial_unknown_refused=1 invalidation=1 no_live_owner_claim=1 gpu=0");
}
