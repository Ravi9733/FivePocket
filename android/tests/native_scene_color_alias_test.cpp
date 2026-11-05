#include "native_scene_color_alias.h"
#include "native_fixed16_edram.h"
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace recomp::gpu;
namespace {
unsigned word(const std::vector<std::uint8_t>& b,unsigned at) {
    assert(at+4<=b.size());unsigned v=0;for(unsigned i=0;i<4;++i)v=v*256+b[at+i];return v;
}
void put(std::vector<std::uint8_t>& b,unsigned at,unsigned v) {
    assert(at+4<=b.size());for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);
}
Gta5ClearCapture authored() {
    Gta5ClearCapture c;c.state=std::vector<std::uint8_t>(13864);auto& s=*c.state;
    s[11068]=0x20;put(s,10368,0x14000500);put(s,13124,2);put(s,13556,1280);put(s,13560,720);
    const unsigned bases[]{384,768,1536,1152,0};
    for(unsigned i=0;i<5;++i) {
        auto& f=c.surfaces[i];f.object=f.tileCache=0x1000+i*0x100;f.header=std::vector<std::uint8_t>(40);
        const unsigned info=bases[i]|(i==4?0x10000:0);
        put(*f.header,24,0x14000500);put(*f.header,28,info);put(*f.header,36,0x13FC0BF8);
        put(s,12816+i*4,f.object);put(s,13104+i*4,f.tileCache);put(s,i==4?10376:i==0?10372:10376+i*4,info);
    }
    put(s,13100,0x1000);
    for(unsigned bin=0;bin<2;++bin) {
        put(s,13128+bin*16+4,bin?384:0);put(s,13128+bin*16+8,1280);put(s,13128+bin*16+12,bin?720:384);
        put(s,13368+bin*8+4,bin?384:0);
    }
    return c;
}
void checkShadowPreservation(const NativeEdram32Bands& scene) {
    Gta5Fixed16ClearPlan shadow{0x2000,256,256,320,64,64,1,4,0,{}};
    shadow.depth=Gta5Depth24ClearPlan{0x2100,256,256,320,0,64,1,1,{}};
    const auto before=scene.readColorWords(720,1280,1280,720);assert(before);
    auto owner=scene;
    // Authored prior values in BOTH subsequently written attachments must be
    // discarded. The scene's disjoint colour source must survive all three
    // callers (clear, inline rectangle, indexed) of the production helper.
    for(unsigned stage=0;stage<3;++stage) {
        const std::vector<std::uint8_t> old(320*256*4,0x77);
        assert(owner.recordCompletedRawColorRows(0,320,320,256,old,owner.nextSerial()));
        assert(owner.recordCompletedRawColorRows(64,320,320,256,old,owner.nextSerial()));
        NativeEdram32Mutation mutation(owner);
        assert(executeNativeEdram32Fixed16Write(mutation,shadow,[&] {
            assert(!owner.knownTiles());
            assert(!mutation.candidate().readColorWords(0,320,320,256));
            assert(!mutation.candidate().readColorWords(64,320,320,256));
            assert(mutation.candidate().readColorWords(720,1280,1280,720)==before);
            return true; // Authored fence + current-input success, NOT GPU proof.
        }));
        assert(owner.readColorWords(720,1280,1280,720)==before);
        assert(!owner.readColorWords(0,320,320,256) && !owner.readColorWords(64,320,320,256));
    }
    for(unsigned failure=0;failure<3;++failure) {
        owner=scene;
        try {
            NativeEdram32Mutation mutation(owner);
            assert(!executeNativeEdram32Fixed16Write(mutation,shadow,[&] {
                if(failure==2)throw 7;
                return false; // GPU failure OR changed inputs after fence.
            }));
        } catch(int value) {assert(value==7 && failure==2);}
        assert(!owner.knownTiles());
    }
    for(unsigned bad=0;bad<8;++bad) {
        auto plan=shadow;owner=scene;unsigned executions=0;
        if(bad==0)plan.tileCount=63;
        if(bad==1)plan.depth->tileCount=63;
        if(bad==2)plan.depth->baseTile=64;
        if(bad==3)plan.baseTile=2040;
        if(bad==4)plan.format=7;
        if(bad==5)plan.depth->format=0;
        if(bad==6)plan.samples=3;
        if(bad==7)plan.depth->pitch=400;
        {NativeEdram32Mutation mutation(owner);
         assert(!executeNativeEdram32Fixed16Write(mutation,plan,[&]{++executions;return true;}));}
        assert(!executions && !owner.knownTiles());
    }
    // A valid writer cannot invent previously unknown disjoint source pixels.
    owner.reset();
    {NativeEdram32Mutation mutation(owner);
     assert(executeNativeEdram32Fixed16Write(mutation,shadow,[]{return true;}));}
    assert(!owner.readColorWords(720,1280,1280,720));
    std::puts("SCENE_FIXED16_PRESERVATION_PASS clear_rectangle_indexed=1 both_written_attachments_unknown=1 "
        "disjoint_words_identical=1 failure_exception_changed_inputs_unknown=1 bad_footprints_no_execution=1 gpu=0");
}
void check(const Gta5ClearCapture& c,bool historical) {
    assert(describeGta5LogicalTileSurfaces(c));
    NativeEdram32Bands owner;
    for(unsigned bin=0;bin<2;++bin) {
        NativeEdram32Mutation mutation(owner);
        const auto plans=prepareNativeSceneColorAliases(mutation,c,bin);assert(plans);
        assert(stageCompletedNativeSceneColors(mutation,*plans,[&](unsigned slot) {
            assert(!owner.knownTiles()); // Unpublished even after earlier readbacks.
            const auto& p=(*plans)[slot];
            assert(p.physical.width==1280 && p.physical.height==(bin?336:384));
            assert((p.sourceRectangle==std::array<unsigned,4>{0,bin?384u:0u,1280,bin?720u:384u}));
            const auto height=p.physical.height;std::vector<std::uint8_t> bytes(1280*height*4);
            for(unsigned y=0;y<height;++y)for(unsigned x=0;x<1280;++x) {
                const unsigned raw=(bin<<28)|(slot<<24)|(y<<12)|x;
                for(unsigned i=0;i<4;++i)bytes[(std::size_t{y}*1280+x)*4+i]=raw>>(8*i);
            }
            return std::optional{std::move(bytes)};
        }));
        assert(!owner.knownTiles());mutation.commit();
    }
    assert(owner.knownTiles()==1536);
    for(unsigned slot=0;slot<4;++slot) {
        const auto p=planNativeSceneColorAlias(c,0,slot);assert(p);
        const auto words=owner.readColorWords(p->physical.baseTile,1280,1280,384);assert(words);
        for(unsigned y=0;y<384;++y)for(unsigned x=0;x<1280;++x)
            assert((*words)[std::size_t{y}*1280+x]==((y<336?1u<<28:0u)|(slot<<24)|(y<<12)|x));
    }
    // The tail of scene colour0 at physical720..767 belongs to the earlier
    // bin, even though the most recent logical bin starts at Y384.
    const auto tail=owner.readColorWords(720,1280,1280,48);assert(tail);
    for(unsigned y=0;y<48;++y)assert((*tail)[y*1280]==((y+336)<<12));
    assert(!owner.readColorWords(0,1280,1280,16)); // no invented depth/colour data
    assert(!planNativeSceneColorAlias(c,2,0) && !planNativeSceneColorAlias(c,0,4));
    {auto bad=c;bad.state.reset();assert(!planNativeSceneColorAlias(bad,0,0));}
    {auto bad=c;bad.surfaces[0].header.reset();assert(!planNativeSceneColorAlias(bad,0,0));}
    {auto bad=c;put(*bad.state,13380,385);assert(!planNativeSceneColorAlias(bad,1,0));}
    {auto bad=c;put(*bad.state,13104,0x2000);assert(!planNativeSceneColorAlias(bad,0,0));}
    {auto bad=c;put(*bad.surfaces[0].header,28,word(*bad.surfaces[0].header,28)|0x10000);
     put(*bad.state,10372,word(*bad.state,10372)|0x10000);assert(!planNativeSceneColorAlias(bad,0,0));}
    checkShadowPreservation(owner);
    // A missing/uninitialized readback, wrong-sized output or a later failed
    // resource publication cannot make an earlier staged attachment visible.
    for(unsigned failure=0;failure<6;++failure) {
        auto rejected=owner;
        {
            NativeEdram32Mutation mutation(rejected);
            const auto plans=prepareNativeSceneColorAliases(mutation,c,1);assert(plans);
            unsigned readbacks=0;
            const bool staged=stageCompletedNativeSceneColors(mutation,*plans,[&](unsigned slot)
                    ->std::optional<std::vector<std::uint8_t>> {
                ++readbacks;
                if(failure<4 && slot==failure)return std::nullopt;
                const auto size=1280*336*4-(failure==4 && slot==3?1:0);
                return std::vector<std::uint8_t>(size,0x11);
            });
            assert(staged==(failure==5));
            assert(readbacks==(failure<4?failure+1:4));
            // Even a fully staged pass may fail current-input/publication
            // validation. Deliberately do not commit the transaction.
            assert(!rejected.knownTiles());
        }
        assert(!rejected.knownTiles());
    }
    {auto rejected=owner;auto bad=c;bad.state.reset();NativeEdram32Mutation mutation(rejected);
     assert(!prepareNativeSceneColorAliases(mutation,bad,0) && !rejected.knownTiles());}
    std::puts("SCENE_COLOR_ALIAS_TRANSACTION_PASS four_actual_readbacks_required=1 stage_not_commit=1 "
        "missing_malformed_and_failed_publication_unknown=1 authored_readbacks=1 gpu=0");
    std::printf("SCENE_COLOR_ALIAS_PLAN_PASS historical_state=%u authored_pixels=1 two_bins=1 shorter_bin_preserves_48_rows=1 "
        "physical_origin_not_logical_origin=1 missing_and_changed_inputs_refused=1 gpu=0 live_import=0 gameplay=0\n",unsigned(historical));
}
}
int main(int argc,char** argv) {
    assert(argc==1 || argc==2);check(authored(),false);
    if(argc==2) {
        const std::string dir=argv[1];const auto read=[&](const std::string& name,unsigned size) {
            std::ifstream f(dir+"/tile-recorded-clear-0-"+name+".bin",std::ios::binary|std::ios::ate);
            assert(f && f.tellg()==size);f.seekg(0);std::vector<std::uint8_t> bytes(size);
            assert(f.read(reinterpret_cast<char*>(bytes.data()),size));return bytes;
        };
        Gta5ClearCapture c;c.state=read("state",13864);
        for(unsigned i=0;i<5;++i) {
            auto& s=c.surfaces[i];s.object=word(*c.state,12816+i*4);s.tileCache=word(*c.state,13104+i*4);
            s.header=read("surface-"+std::to_string(i),40);
        }
        check(c,true);
    }
}
