// HOST component test. Authored physical words are never a live source receipt.
#include "native_raw10_color_resolve.h"
#include "native_serialized_resource_batch.h"
#include "xenos_raw10_texture.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string_view>
#ifdef RECOMP_PRIVATE_RAW10_CALL
#include RECOMP_PRIVATE_RAW10_CALL
#endif
using namespace recomp::gpu;
namespace {
void put(std::vector<std::uint8_t>& b,unsigned at,unsigned v) {
    assert(at+4<=b.size());for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);
}
unsigned word(const std::vector<std::uint8_t>& b,unsigned at) {
    assert(at+4<=b.size());unsigned v=0;for(unsigned i=0;i<4;++i)v=v*256+b[at+i];return v;
}
Gta5TileResolveCapture authored() {
    Gta5TileResolveCapture c;c.stack=0x64000000;c.arguments={0x10001000,0,0x64000200,0x10002000,0,0,0,0x64000220,0x82000004};
    c.state=std::vector<std::uint8_t>(13860);c.surfaces.emplace();
    c.surfaces->surfaces[0].object=0x10003000;c.surfaces->surfaces[0].header=std::vector<std::uint8_t>(40);
    auto& h=*c.surfaces->surfaces[0].header;put(h,24,0x14000500);put(h,28,0x202D0);put(h,36,0x13FC1678);
    auto& s=*c.state;put(s,12816,0x10003000);put(s,10368,0x14000500);put(s,10372,0xA02D0);
    put(s,12408,1);put(s,11068,0x2C00);
    c.destinationHeader=std::vector<std::uint8_t>(52);
    const std::array<unsigned,6> fetch{0x8A000002,0x010000B6,0x0059E4FF,0xC14,0,0x200};
    for(unsigned i=0;i<6;++i)put(*c.destinationHeader,28+i*4,fetch[i]);
    c.pointed.rectangle=std::vector<std::uint8_t>(16);put(*c.pointed.rectangle,8,1280);put(*c.pointed.rectangle,12,720);
    c.pointed.stack92=std::vector<std::uint8_t>(4);c.pointed.stack100=std::vector<std::uint8_t>(4);
    c.pointed.color=std::vector<std::uint8_t>(16);return c;
}
// Independent general 2D outer/inner -> bank/pipe construction, four bytes per
// texel. It does not call the production's 32x32-specialized offset routine.
unsigned offset(unsigned x,unsigned y) {
    const unsigned outer=((y/32)*40+x/32)*64;
    const unsigned inner=(y/2%8)*8+x%8;
    const unsigned v=(outer+inner)*4;
    const unsigned bank=y/16%2,pipe=(x/8%4)^((y/8%2)*2);
    return (v&15)|((y%2)*16)|((v&16)*2)|(pipe*64)|((v&224u)*8)|((v&~255u)*16)|(bank*2048);
}
void check(const Gta5TileResolveCapture& c,bool captured) {
    const char* why=nullptr;const auto plan=planNativeRaw10ColorResolve(c,&why);
    if(!plan)std::fprintf(stderr,"RAW10 plan rejected: %s\n",why);
    assert(plan && std::string_view(why)=="raw10-layout-planned-source-unproven");
    assert(plan->destination.width==1280 && plan->destination.height==720);
    NativeEdram32Bands owner;std::vector<std::uint8_t> before(plan->Bytes,0xA5);
    assert(!packNativeRaw10ColorResolve(c,owner,before,&why));
    assert(std::string_view(why)=="physical-colour-unknown");
    std::vector<std::uint8_t> rgba(1280*720*4);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
        // All 1024 values in each RGB channel and all four alpha codes occur.
        const unsigned r=x%1024,g=(x+3*y)%1024,b=(3*x+7*y)%1024,a=(x+y)%4;
        const unsigned raw=r+g*1024+b*1048576+a*1073741824u;
        for(unsigned i=0;i<4;++i)rgba[(std::size_t{y}*1280+x)*4+i]=raw>>(8*i);
    }
    const NativeOffscreenDrawPlan source{0x1000,1280,720,1280,720,720};
    assert(owner.recordCompletedColorBand(source,rgba,1));const auto serial=owner.nextSerial();
    const auto packed=packNativeRaw10ColorResolve(c,owner,before,&why);assert(packed);
    assert(std::string_view(why)=="raw10-packed-no-publication");
    auto expected=before;std::vector<bool> touched(before.size()/4);
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
        const unsigned r=x%1024,g=(x+3*y)%1024,b=(3*x+7*y)%1024,a=(x+y)%4;
        const unsigned at=offset(x,y);assert(at%4==0 && at+4<=expected.size() && !touched[at/4]);touched[at/4]=true;
        put(expected,at,b+g*1024+r*1048576+a*1073741824u);
    }
    assert(*packed==expected && std::count(touched.begin(),touched.end(),true)==1280*720);
    // Connected copy -> packed texture consumer, not just a standalone layout
    // round trip. Independent expected R/B swap above must survive endian and
    // tiled import with all ten bits per RGB channel (and both alpha bits).
    const auto imported=linearizeXenosRaw10UnsignedTexture(plan->destination,*packed);
    assert(imported && imported->size()==rgba.size());
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
        const auto at=(std::size_t{y}*1280+x)*4;
        const unsigned r=x%1024,g=(x+3*y)%1024,b=(3*x+7*y)%1024,a=(x+y)%4;
        const unsigned wanted=b+g*1024+r*1048576+a*1073741824u;
        for(unsigned i=0;i<4;++i)assert((*imported)[at+i]==((wanted>>(8*i))&255));
    }
    assert(plan->destination.fetch==describeGta5TextureHeader(*c.destinationHeader)->fetch);
    std::printf("RAW10_COPY_TO_TEXTURE_IMPORT_PASS captured_state=%u pixels=921600 rgb_bits=10 alpha_bits=2 swizzle_and_bias_not_baked=1 gpu=0\n",unsigned(captured));
    assert(owner.nextSerial()==serial && owner.knownTiles()==720 && before==std::vector<std::uint8_t>(plan->Bytes,0xA5));
    for(unsigned tile:{720u,1079u,1439u}) {
        auto missing=owner;assert(missing.invalidate(tile,1));
        assert(!packNativeRaw10ColorResolve(c,missing,before,&why) && std::string_view(why)=="physical-colour-unknown");
    }
    auto shortBefore=before;shortBefore.pop_back();assert(!packNativeRaw10ColorResolve(c,owner,shortBefore));
    // Same physical store through a depth view must not accidentally swap the
    // colour half-tile. Verify depth publication -> raw colour copy connection.
    const auto depth=owner.readDepthWords(720,1280,1280,720);assert(depth);
    NativeEdram32Bands depthOwner;assert(depthOwner.recordCompletedDepthWords(720,1280,1280,720,*depth,1));
    assert(packNativeRaw10ColorResolve(c,depthOwner,before)==packed);
    // A preceding 640x360 raw32 producer shares this store with the 1280x720
    // copy. Its pitch differs and its last physical tile row is only half
    // written. Preserve old pixels outside those exact writes; never round the
    // write up to its allocation/attachment extent. This is an authored
    // producer connection, not a claim about the live game's initialized data.
    auto overwritten=owner;std::vector<std::uint8_t> raw32(640*360*4);
    for(unsigned y=0;y<360;++y)for(unsigned x=0;x<640;++x) {
        const unsigned raw=0x3F800000u+(y*640+x);
        for(unsigned i=0;i<4;++i)raw32[(y*640+x)*4+i]=raw>>(8*i);
        const unsigned tile=(y/16)*8+x/80;
        const unsigned aliasX=(tile%16)*80+x%80,aliasY=(tile/16)*16+y%16;
        const unsigned r=raw%1024,g=(raw/1024)%1024,b=(raw/1048576)%1024,a=raw/1073741824u;
        put(expected,offset(aliasX,aliasY),b+g*1024+r*1048576+a*1073741824u);
    }
    const NativeOffscreenDrawPlan raw32Plan{0x3000,640,360,640,720,184,14};
    {NativeEdram32Mutation mutation(overwritten);
     assert(recordCompletedNativeRaw32Draw(mutation,raw32Plan,raw32));mutation.commit();}
    assert(overwritten.knownTiles()==720 && packNativeRaw10ColorResolve(c,overwritten,before)==std::optional{expected});
    NativeEdram32Bands unknownRest;
    {NativeEdram32Mutation mutation(unknownRest);
     assert(recordCompletedNativeRaw32Draw(mutation,raw32Plan,raw32));mutation.commit();}
    assert(unknownRest.knownTiles()==176 && !packNativeRaw10ColorResolve(c,unknownRest,before));
    for(unsigned format:{4u,7u,12u,2u}) {
        auto rejected=owner;auto invalid=raw32Plan;invalid.format=format;
        {NativeEdram32Mutation mutation(rejected);assert(!recordCompletedNativeRaw32Draw(mutation,invalid,raw32));}
        assert(!rejected.knownTiles());
    }
    {auto rejected=owner;auto invalid=raw32Plan;--invalid.tileCount;
     {NativeEdram32Mutation mutation(rejected);assert(!recordCompletedNativeRaw32Draw(mutation,invalid,raw32));}
     assert(!rejected.knownTiles());}
    // Connected wide MRT/pyramid sequence -> same raw10 copy. The explicit
    //40x16 reference EMULATOR model is independently addressed here; these
    // authored words are not an Xbox-hardware or native-GPU comparison.
    for(const auto extent:{std::array<unsigned,3>{640,360,640},{320,180,320},{160,90,160},{80,45,80},{40,11,80}}) {
        const unsigned width=extent[0],height=extent[1],pitch=extent[2],wordsPerPixel=width==40?1:2;
        NativeOffscreenDrawPlan p{0x3100,width,height,pitch,904,0,wordsPerPixel==2?7u:14u};
        p.tileCount=*nativeOffscreenTileCount(pitch,height,p.format);
        std::vector<std::uint8_t> bytes(std::size_t{width}*height*wordsPerPixel*4);
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)for(unsigned half=0;half<wordsPerPixel;++half) {
            const unsigned raw=0x30000000u|(half<<24)|(height<<14)|(y<<4)|x;
            const auto at=((y*width+x)*wordsPerPixel+half)*4;
            for(unsigned i=0;i<4;++i)bytes[at+i]=raw>>(8*i);
            const unsigned samplesPerTile=80/wordsPerPixel;
            const unsigned tile=904+(y/16)*(pitch/samplesPerTile)+x/samplesPerTile;
            const unsigned inRow=(x%samplesPerTile)*wordsPerPixel+half;
            const unsigned aliasX=((tile-720)%16)*80+inRow,aliasY=((tile-720)/16)*16+y%16;
            const unsigned r=raw%1024,g=(raw/1024)%1024,b=(raw/1048576)%1024,a=raw/1073741824u;
            put(expected,offset(aliasX,aliasY),b+g*1024+r*1048576+a*1073741824u);
        }
        if(p.format==7) {
            auto rejected=owner;NativeEdram32Mutation mutation(rejected);
            assert(!recordCompletedNativeColorDraw(mutation,p,bytes)); // No implicit model.
            assert(!recordCompletedNativeColorDraw(mutation,p,bytes,static_cast<NativeEdram64Model>(2)));
            auto truncated=bytes;truncated.pop_back();
            assert(!recordCompletedNativeColorDraw(mutation,p,truncated,NativeEdram64Model::Xenia40x16));
        }
        {NativeEdram32Mutation mutation(overwritten);
         assert(recordCompletedNativeColorDraw(mutation,p,bytes,NativeEdram64Model::Xenia40x16));mutation.commit();}
        assert(packNativeRaw10ColorResolve(c,overwritten,before)==std::optional{expected});
    }
    assert(overwritten.knownTiles()==720);
    // The same packer and publication helper used by the dispatcher feed the
    // real compare-before-publish primitive, not a callback that always passes.
    auto destination=before;unsigned publications=0;
    const auto publish=[&](const auto& completed) {
        ++publications;
        const NativeSerializedResourceUpdate update{0xA1000000,{destination.data(),destination.size()},&before,&completed};
        return publishNativeSerializedResourceBatch(&update,1);
    };
    assert(sameNativeRaw10ColorInputs(c,c));
    assert(publishNativeRaw10ColorResolve(c,overwritten,before,[&]{return sameNativeRaw10ColorInputs(c,c);},publish,&why));
    assert(publications==1 && destination==expected && std::string_view(why)=="raw10-published");
    destination=before;auto changed=c;put(*changed.destinationHeader,32,word(*changed.destinationHeader,32)^0x1000);
    assert(!sameNativeRaw10ColorInputs(c,changed));
    assert(!publishNativeRaw10ColorResolve(c,overwritten,before,[&]{return sameNativeRaw10ColorInputs(c,changed);},publish,&why));
    assert(publications==1 && destination==before && std::string_view(why)=="changed-inputs");
    destination.back()^=1;const auto stale=destination;
    assert(!publishNativeRaw10ColorResolve(c,overwritten,before,[]{return true;},publish,&why));
    assert(publications==2 && destination==stale && std::string_view(why)=="publication");
    assert(!publishNativeRaw10ColorResolve(c,unknownRest,before,[]{return true;},publish));
    assert(publications==2 && destination==stale);
    std::printf("RAW64_REFERENCE_TO_RAW10_PUBLICATION_PASS captured_state=%u reference_model=xenia40x16 "
        "four_wide_levels=1 final_partial_raw32=1 independent_word_and_texture_addressing=1 "
        "padding_preserved=1 actual_publication_primitive=1 stale_or_unknown_no_write=1 gpu=0 xbox_layout_verified=0\n",unsigned(captured));
    for(unsigned arg:{1u,4u,5u,6u}) {auto bad=c;bad.arguments[arg]=1;assert(!planNativeRaw10ColorResolve(bad));}
    for(unsigned at:{10368u,10372u,12408u,11068u,12816u,12820u,12824u,12828u,12832u}) {
        auto bad=c;put(*bad.state,at,word(*bad.state,at)^1u);assert(!planNativeRaw10ColorResolve(bad));
    }
    for(unsigned at:{24u,28u,36u}) {
        auto bad=c;auto& h=*bad.surfaces->surfaces[0].header;put(h,at,word(h,at)^1u);assert(!planNativeRaw10ColorResolve(bad));
    }
    for(unsigned i=0;i<6;++i) {auto bad=c;auto& h=*bad.destinationHeader;put(h,28+i*4,word(h,28+i*4)^1u);assert(!planNativeRaw10ColorResolve(bad));}
    // Sampling bias must survive in metadata, but must NOT alter raw copied
    // bits. All other sampling-bias profiles and neighbouring bits still fail.
    for(unsigned bias=0;bias<64;++bias) {
        auto variant=c;put(*variant.destinationHeader,40,0xC14|(bias<<13));
        const auto p=planNativeRaw10ColorResolve(variant);
        assert(bool(p)==(bias==0 || bias==3));
        if(p) {
            assert(p->destination.exponentBias==int(bias));
            assert(packNativeRaw10ColorResolve(variant,overwritten,before)==std::optional{expected});
        }
    }
    std::printf("RAW10_SAMPLING_BIAS_PASS captured_state=%u profiles=2 other_bias_refusals=62 metadata_preserved=1 copy_bits_unchanged=1\n",unsigned(captured));
    // Xenia's RT2/RT10 bitwise-equivalent copy classification agrees with
    // both original CPU prefixes. Test view/expanded coherence, not just an
    // accepted register number, and retain the same connected publisher.
    for(unsigned expanded=0;expanded<2;++expanded) {
        auto variant=c;put(*variant.state,10372,expanded?0xA02D0:0x202D0);
        put(*variant.state,12408,expanded);
        assert(planNativeRaw10ColorResolve(variant));
        assert(packNativeRaw10ColorResolve(variant,overwritten,before)==std::optional{expected});
        assert(owner.nextSerial()==serial);
        auto mismatch=variant;put(*mismatch.state,12408,expanded^1u);
        assert(!planNativeRaw10ColorResolve(mismatch));
        auto wrongFormat=variant;put(*wrongFormat.state,10372,0x302D0);
        assert(!planNativeRaw10ColorResolve(wrongFormat));
        auto unknownFlag=variant;put(*unknownFlag.state,12408,2);
        assert(!planNativeRaw10ColorResolve(unknownFlag));
        auto otherView=variant;put(*otherView.state,10372,expanded?0x202D0:0xA02D0);
        put(*otherView.state,12408,expanded^1u);
        assert(planNativeRaw10ColorResolve(otherView));
        assert(!sameNativeRaw10ColorInputs(variant,otherView));
        assert(!packNativeRaw10ColorResolve(variant,unknownRest,before));
    }
    std::printf("RAW10_SOURCE_VIEW_PASS captured_state=%u ordinary2_expanded10=2 mismatched_pairs_refused=2 float3_refused=1 unknown_flag_refused=1 copy_bits_identical=1 changed_inputs_refused=1 unknown_physical_refused=1\n",unsigned(captured));
    for(unsigned i=0;i<4;++i) {auto bad=c;put(*bad.pointed.rectangle,i*4,word(*bad.pointed.rectangle,i*4)^1u);assert(!planNativeRaw10ColorResolve(bad));}
    {auto bad=c;bad.state.reset();assert(!planNativeRaw10ColorResolve(bad));}
    {auto bad=c;bad.state->pop_back();assert(!planNativeRaw10ColorResolve(bad));}
    {auto bad=c;bad.surfaces.reset();assert(!planNativeRaw10ColorResolve(bad));}
    {auto bad=c;bad.surfaces->surfaces[0].header.reset();assert(!planNativeRaw10ColorResolve(bad));}
    {auto bad=c;bad.destinationHeader.reset();assert(!planNativeRaw10ColorResolve(bad));}
    {auto bad=c;bad.pointed.stack92.reset();assert(!planNativeRaw10ColorResolve(bad));}
    {auto bad=c;put(*bad.pointed.stack100,0,1);assert(!planNativeRaw10ColorResolve(bad));}
    {auto unused=c;unused.arguments[7]=0;unused.f1=0x3FF0000000000000ull;unused.pointed.color.reset();assert(planNativeRaw10ColorResolve(unused));}
    std::printf("RAW10_COLOR_RESOLVE_PASS captured_state=%u authored_source=1 pixels=921600 padding_preserved=81920 "
        "independent_tiling=1 rgb_swap=1 endian=1 depth_colour_alias=1 pitched_partial_producer=1 unknown_refused=1 source_unchanged=1 gpu=0 publication=0 gameplay=0\n",unsigned(captured));
}
}
int main(int argc,char** argv) {
    assert(argc==1 || argc==2);check(authored(),false);
    if(argc==2) {
#ifdef RECOMP_PRIVATE_RAW10_CALL
        const std::string dir=argv[1];const auto read=[&](const std::string& key,unsigned size) {
            std::ifstream f(dir+"/native-resolve-failure-"+key+".bin",std::ios::binary|std::ios::ate);
            assert(f && f.tellg()==size);f.seekg(0);std::vector<std::uint8_t> b(size);
            assert(f.read(reinterpret_cast<char*>(b.data()),size));return b;
        };
        Gta5TileResolveCapture c;c.arguments=SavedArgs;c.stack=SavedStack;c.f1=SavedF1;
        c.state=read("state",13860);c.destinationHeader=read("destination-header",52);c.surfaces.emplace();
        for(unsigned i=0;i<5;++i) {
            auto& s=c.surfaces->surfaces[i];s.object=word(*c.state,12816+i*4);
            if(s.object)s.header=read("surface-"+std::to_string(i),40);
        }
        c.pointed.rectangle=read("rectangle",16);c.pointed.stack92=read("stack92",4);
        c.pointed.stack100=read("stack100",4);c.pointed.color=read("clear-color",16);check(c,true);
#else
        (void)argv;assert(false && "capture call identity must be compiled and receipt-bound");
#endif
    }
}
