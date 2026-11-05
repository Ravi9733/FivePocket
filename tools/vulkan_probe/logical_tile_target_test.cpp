#include "native_logical_tile_target.h"
#include "xenos_texture_2d.h"
#include "xenos_color_copy.h"
#include "native_fixed16_clear_target.h"
#include "native_fixed16_edram.h"
#include "../../android/tests/gta5_logical_tile_clear_fixture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <limits>

namespace {
void checkPhysicalColorConnection(recomp::gpu::ProbeDevice& device,const std::vector<std::uint8_t>& state) {
    using namespace recomp::gpu;
    const auto capture=test::authoredLogicalClear(state);
    auto target=NativeLogicalTileTarget::create(device,state);assert(target);
    NativeEdram32Bands owner;
    std::array<std::array<std::array<std::uint8_t,4>,4>,2> colors{};
    for(unsigned bin=0;bin<2;++bin)for(unsigned slot=0;slot<4;++slot)for(unsigned c=0;c<4;++c)
        colors[bin][slot][c]=std::uint8_t(13+bin*113+slot*23+c*7);
    assert(!target->readCapturedColorTile(capture,0,0)); // Uninitialized is not black.
    for(unsigned bin=0;bin<2;++bin) {
        NativeEdram32Mutation mutation(owner);
        const auto plans=prepareNativeSceneColorAliases(mutation,capture,bin);assert(plans);
        if(bin)assert(target->carryCapturedTile(capture,0,1));
        NativeLogicalTileTarget::Clear clear{1u<<bin,0x3F,colors[bin],0.5f,19,{}};
        assert(target->clear(clear)); // Actual fenced GPU producer, authored values.
        assert(stageCompletedNativeSceneColors(mutation,*plans,[&](unsigned slot) {
            assert(!owner.knownTiles());return target->readCapturedColorTile(capture,bin,slot);
        }));
        assert(!owner.knownTiles());mutation.commit();
    }
    const auto expectedWord=[&](unsigned bin,unsigned slot) {
        unsigned word=0;for(unsigned c=0;c<4;++c)word|=unsigned(colors[bin][slot][c])<<(8*c);return word;
    };
    for(unsigned slot=0;slot<4;++slot) {
        const auto plan=planNativeSceneColorAlias(capture,0,slot);assert(plan);
        const auto words=owner.readColorWords(plan->physical.baseTile,1280,1280,384);assert(words);
        for(unsigned y=0;y<384;++y)for(unsigned x=0;x<1280;++x)
            assert((*words)[y*1280+x]==expectedWord(y<336?1:0,slot));
    }
    assert(!owner.readDepthWords(0,1280,1280,384)); // No unproved depth encoding imported.
    const auto copySource=owner.readColorWords(720,1280,1280,720);assert(copySource);
    // Carry the same physical owner through a REAL disjoint fixed16/depth
    // clear. The written encoding stays unknown; no SNORM-as-raw publication.
    Gta5Fixed16ClearPlan shadow{0x2000,256,256,320,64,64,1,4,0,{}};
    const auto depth=planExactXenosFloat24Clear(0.5,23);assert(depth);
    shadow.depth=Gta5Depth24ClearPlan{0x2100,256,256,320,0,64,1,1,*depth};
    auto shadowTarget=NativeFixed16ClearTarget::create(device,shadow);assert(shadowTarget);
    {NativeEdram32Mutation mutation(owner);
     assert(executeNativeEdram32Fixed16Write(mutation,shadow,[&] {
         assert(!owner.knownTiles());return shadowTarget->clear(shadow.color,shadow.depth->value);
     }));}
    assert(owner.readColorWords(720,1280,1280,720)==copySource);
    assert(!owner.readColorWords(64,320,320,256) && !owner.readDepthWords(0,320,320,256));
    auto bad=capture;bad.surfaces[0].header.reset();assert(!target->readCapturedColorTile(bad,0,0));
    bad=capture;test::tileWord(*bad.state,13380,385);assert(!target->readCapturedColorTile(bad,1,0));
    bad=capture;test::tileWord(*bad.state,10372,0x10180);test::tileWord(*bad.surfaces[0].header,28,0x10180);
    assert(!target->readCapturedColorTile(bad,0,0));
    assert(!target->readCapturedColorTile(capture,2,0) && !target->readCapturedColorTile(capture,0,4));
    assert(owner.readColorWords(720,1280,1280,720)==copySource);
    std::puts("SCENE_COLOR_ALIAS_GPU_CONNECTION_PASS authored_clear_producer=1 captured_layout=1 "
        "fenced_same_target_readback=1 four_colors=1 second_bin_origin_zero=1 earlier_48_rows_preserved=1 "
        "real_disjoint_fixed16_clear=1 written_ranges_unknown=1 changed_surface_refused=1 "
        "scene_draws_tested=0 live_import=0 gameplay=0");
}
}
int main(int argc,char** argv) {
    using namespace recomp::gpu;
    assert(argc==2);
    std::vector<std::uint8_t> state(13860);
    std::ifstream input(argv[1],std::ios::binary);
    assert(input.read(reinterpret_cast<char*>(state.data()),state.size()) && input.peek()==EOF);
    const auto plan=describeGta5DriverTiling(state);
    assert(plan && plan->width==1280 && plan->height==720 && plan->tiles.size()==2 && plan->disjointFullCoverage);
    ProbeDevice device;ShaderRequirements requirements;
    assert(device.inspect() && device.create(requirements));
    auto target=NativeLogicalTileTarget::create(device,state);assert(target);
    assert(!target->readback());
    NativeLogicalTileTarget::Clear first{1,0x3F,{},0.25f,17,{}};
    for(unsigned a=0;a<4;++a)first.colors[a]={std::uint8_t(11+a*31),std::uint8_t(43+a*29),std::uint8_t(199-a*17),std::uint8_t(255-a*13)};
    assert(target->clear(first) && !target->readback());
    assert(target->readColorRegion(0,{0,0,1280,384}));
    assert(!target->readColorRegion(0,{0,384,1280,720}));
    assert(!target->readColorRegion(0,{0,383,1280,385}));
    auto second=first;second.tiles=2;second.depth=0.75f;second.stencil=211;
    for(auto& color:second.colors)for(auto& c:color)c=255-c;
    assert(target->clear(second));
    const auto verify=[&](const NativeLogicalTileTarget::Snapshot& s,
            const NativeLogicalTileTarget::Clear& top,const NativeLogicalTileTarget::Clear& bottom) {
        for(unsigned a=0;a<4;++a)assert(s.colors[a].size()==1280*720*4);
        assert(s.depth.size()==1280*720 && s.stencil.size()==1280*720);
        for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
            const auto& value=y<384?top:bottom;const auto index=y*1280+x;
            for(unsigned a=0;a<4;++a)for(unsigned c=0;c<4;++c)assert(s.colors[a][index*4+c]==value.colors[a][c]);
            assert(s.depth[index]==value.depth && s.stencil[index]==value.stencil);
        }
    };
    auto initial=target->readback();assert(initial);verify(*initial,first,second);
    std::puts("LOGICAL_MRT_TWO_TILE_CLEAR_PASS colors=4 depth_stencil=1 split=384 all_pixels_exact=1");
    // Change just color2 of tile1; preserve three colors, tile0, depth/stencil.
    auto edit=second;edit.aspects=4;edit.colors[2]={127,128,1,254};
    assert(target->clear(edit));second.colors[2]=edit.colors[2];
    auto after=target->readback();assert(after);verify(*after,first,second);
    // Independent depth and stencil aspect clears preserve each other.
    edit=first;edit.aspects=0x10;edit.depth=0.5f;
    assert(target->clear(edit));first.depth=edit.depth;
    edit=second;edit.aspects=0x20;edit.stencil=89;
    assert(target->clear(edit));second.stencil=edit.stencil;
    after=target->readback();assert(after);verify(*after,first,second);
    std::puts("LOGICAL_MRT_PARTIAL_PRESERVATION_PASS isolated_color=1 isolated_depth=1 isolated_stencil=1");
    const auto before=target->completedClearSerial();
    for(auto bad:{0u,4u,UINT32_MAX}){edit=first;edit.tiles=bad;assert(!target->clear(edit));}
    for(auto bad:{0u,0x40u,UINT32_MAX}){edit=first;edit.aspects=bad;assert(!target->clear(edit));}
    for(auto bad:{-1.f,2.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        edit=first;edit.depth=bad;assert(!target->clear(edit));
    }
    assert(target->completedClearSerial()==before);after=target->readback();assert(after);verify(*after,first,second);
    for(unsigned slot=0;slot<4;++slot)for(const std::array<unsigned,4> r:
            {std::array<unsigned,4>{0,384,1280,720},{19,373,1279,391},{1279,719,1280,720}}) {
        auto region=target->readColorRegion(slot,r);assert(region && region->size()==(r[2]-r[0])*(r[3]-r[1])*4);
        for(unsigned y=r[1];y<r[3];++y)for(unsigned x=r[0];x<r[2];++x)for(unsigned c=0;c<4;++c)
            assert((*region)[((y-r[1])*(r[2]-r[0])+x-r[0])*4+c]==after->colors[slot][(y*1280+x)*4+c]);
    }
    assert(!target->readColorRegion(4,{0,0,1280,720}));
    assert(!target->readColorRegion(0,{0,0,1281,720}));
    assert(!target->readColorRegion(0,{0,384,1280,384}));
    assert(target->completedClearSerial()==before);
    std::puts("LOGICAL_MRT_REGION_READ_PASS partial_initialization_checked=1 second_tile_origin=384 crossing_seam=1 single_pixel=1");
    for(unsigned slot=0;slot<4;++slot) {
        auto top=target->readColorRegion(slot,{0,0,1280,384});
        auto bottom=target->readColorRegion(slot,{0,384,1280,720});assert(top && bottom);
        for(unsigned endian=0;endian<4;++endian) {
            const std::array<std::uint32_t,6> fetch{2u|(40u<<22)|0x80000000u,0x10006u|(endian<<6),
                1279u|(719u<<13),0x688,0,0x200};
            const auto desc=XenosTextureDescription::decode(fetch);assert(desc);
            const auto storage=planXenosTexture2D(*desc);assert(storage);
            std::vector<std::uint8_t> prior(storage->sourceBytes,0xA5);
            auto copied=storeXenosTexture2DRegion(*desc,{0,0,1280,384},*top,prior);assert(copied);
            copied=storeXenosTexture2DRegion(*desc,{0,384,1280,720},*bottom,*copied);assert(copied);
            assert(linearizeXenosTexture2D(*desc,*copied)==std::optional{after->colors[slot]});
            // Compare to the independent pre-existing GPU-copy address helper,
            // including every destination padding byte, not just a round trip.
            auto expected=prior;constexpr unsigned swap[]{0,1,3,2};
            for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
                unsigned offset=0;assert(colorCopyOffset(1280,720,x,y,offset));
                for(unsigned c=0;c<4;++c)expected[(offset+c)^swap[endian]]=after->colors[slot][(y*1280+x)*4+c];
            }
            assert(*copied==expected);
        }
    }
    std::puts("LOGICAL_MRT_TO_XENOS_STORAGE_PASS source_layout=captured destination=authored colors=4 endian_modes=4 tile1_origin=384 padding_exact=1 guest_publication=0");
    // Same captured tile layout, AUTHORED headers/API args. Exercise the real
    // API planner -> Vulkan connection, not just manually unpacked clear values.
    auto call=test::authoredLogicalClear(state);call.call.flags=0x30;
    test::tileWord(*call.state,13084,3);call.color.reset();
    assert(target->clearCapturedCall(call));
    first.depth=0.5f;first.stencil=37;after=target->readback();assert(after);verify(*after,first,second);
    // Clip across both tiles: only color1 and depth, leaving stencil and the
    // other three attachments untouched. Check every pixel outside as well.
    call=test::authoredLogicalClear(state);call.call.flags=0x12;
    test::clearRectangle(call,{13,373,1007,390});
    const auto planned=planGta5LogicalTileClear(call);assert(planned);
    const auto prior=*after;assert(target->clearCapturedCall(call));after=target->readback();assert(after);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
        const unsigned pixel=y*1280+x;const bool selected=x>=13 && x<1007 && y>=373 && y<390;
        for(unsigned a=0;a<4;++a)for(unsigned c=0;c<4;++c)
            assert(after->colors[a][pixel*4+c]==(selected && a==1?planned->colors[1][c]:prior.colors[a][pixel*4+c]));
        assert(after->depth[pixel]==(selected?planned->depth:prior.depth[pixel]));
        assert(after->stencil[pixel]==prior.stencil[pixel]);
    }
    const auto serial=target->completedClearSerial();const auto unchanged=*after;
    test::clearRectangle(call,{0,720,1280,721});assert(target->clearCapturedCall(call)); // Empty after clipping.
    call=test::authoredLogicalClear(state);call.surfaces[3].header.reset();assert(!target->clearCapturedCall(call));
    call=test::authoredLogicalClear(state);test::tileWord(*call.state,10372,0x10180);
    test::tileWord(*call.surfaces[0].header,28,0x10180);assert(!target->clearCapturedCall(call)); // Held image's encoding changed.
    assert(target->completedClearSerial()==serial);after=target->readback();assert(after);
    assert(after->colors==unchanged.colors && after->depth==unchanged.depth && after->stencil==unchanged.stencil);
    // A partial first clear must not falsely define the entire target.
    auto fresh=NativeLogicalTileTarget::create(device,state);assert(fresh);
    call=test::authoredLogicalClear(state);test::clearRectangle(call,{13,373,1007,390});
    assert(fresh->clearCapturedCall(call) && !fresh->readback());
    assert(!fresh->readColorRegion(0,{0,0,1280,384}));
    std::puts("LOGICAL_MRT_CAPTURED_CLEAR_CONNECTION_PASS headers=authored depth_only_bin0=1 clipped_seam=1 untouched_exact=1 rejection_preserves=1 partial_initialization_not_promoted=1");
    auto initialDepth=NativeLogicalTileTarget::create(device,state);assert(initialDepth);
    call=test::authoredLogicalClear(state);call.call.flags=0x30;call.call.depthBits=0;call.call.stencil=0;
    test::tileWord(*call.state,13084,3);call.color.reset();
    assert(initialDepth->clearCapturedCall(call) && !initialDepth->readback());
    auto depth=initialDepth->readDepthRegion({0,0,1280,384});auto stencil=initialDepth->readStencilRegion({0,0,1280,384});
    assert(depth && stencil && depth->size()==1280*384 && stencil->size()==depth->size());
    assert(std::all_of(depth->begin(),depth->end(),[](float v){return v==0;}));
    assert(std::all_of(stencil->begin(),stencil->end(),[](auto v){return v==0;}));
    assert(!initialDepth->readDepthRegion({0,383,1280,385}));
    assert(!initialDepth->readStencilRegion({0,384,1280,720}));
    assert(!initialDepth->readColorRegion(0,{0,0,1280,384}));
    assert(!initialDepth->readDepthRegion({0,0,1281,384}));
    depth=initialDepth->readDepthRegion({1279,383,1280,384});stencil=initialDepth->readStencilRegion({1279,383,1280,384});
    assert(depth && depth->size()==1 && (*depth)[0]==0 && stencil && stencil->size()==1 && (*stencil)[0]==0);
    assert(initialDepth->completedClearSerial()==1);
    std::puts("LOGICAL_MRT_DEPTH_STENCIL_REGION_PASS initial_clear_bin0_only=1 color_uninitialized=1 unknown_tile_rejected=1 single_pixel=1 fresh_fence=1");
    auto invalid=state;invalid[10369]=1;assert(!NativeLogicalTileTarget::create(device,invalid));
    invalid=state;invalid[11068]=0;assert(!NativeLogicalTileTarget::create(device,invalid));
    checkPhysicalColorConnection(device,state);
    std::puts("LOGICAL_MRT_REJECTION_PRESERVES_PASS negative_calls=10 fresh_fences=1 guest_publication=0 gameplay_verified=0");
}
