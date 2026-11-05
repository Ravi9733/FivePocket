#include "rgb7e3_float_resolve_fixture.h"
#include "native_rgb7e3_replacement.h"
#include "native_serialized_resource_batch.h"
#include <bit>
#include <cstdio>
#include <string_view>
using namespace recomp::gpu;
using namespace rgb7e3_test;
namespace {
std::uint16_t half(float value) {
    static_assert(sizeof(_Float16)==2);const _Float16 converted=value;
    return std::bit_cast<std::uint16_t>(converted);
}
void check(const Gta5TileResolveCapture& c,bool capturedState) {
    const char* why=nullptr;const auto p=planNativeRgb7e3FloatResolve(c,&why);
    if(!p)std::fprintf(stderr,"RGB7E3_FLOAT_PLAN_REFUSED %s\n",why);assert(p);
    const unsigned width=word(*c.pointed.rectangle,8),height=word(*c.pointed.rectangle,12);
    assert((width==640 && height==360) || (width==320 && height==180) || (width==160 && height==90) || (width==80 && height==45));
    const unsigned tiles=(width/80)*((height+15)/16),known=(width/80)*(height/16);
    const bool clearing=c.arguments[1]==0x100;const unsigned base=width==160?768:width==80?780:720;
    const unsigned padded=width==80?96:width; // Literal: the tiled destination pitch.
    assert(p->width==width && p->height==height && p->pitch==width && p->base==base);
    assert(p->clearsSource==clearing && p->clearTiles==tiles && c.arguments[1]==(clearing?0x100u:0u));
    assert(p->storage.bytesPerTexel==8 && p->storage.linearBytes==width*height*8);
    assert(p->destination.format==29 && p->storage.description.format==32 && p->destination.endian==1);
    assert(!planXenosTexture2D(p->destination)); // No general admission widened.
    NativeEdram32Bands owner;
    const std::vector<std::uint8_t> before(p->storage.sourceBytes,0xA5);
    assert(!packNativeRgb7e3FloatResolve(c,owner,before,&why));
    assert(std::string_view(why)=="physical-colour-unknown");
    std::vector<float> rgba(width*height*4);
    std::vector<std::uint8_t> expectedLinear(width*height*8),expected=before;
    std::vector<bool> touched(before.size());
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        const unsigned index=y*width+x;
        const std::array<unsigned,3> codes{index%1024,(index*3+1)%1024,(index*7+3)%1024};
        for(unsigned channel=0;channel<4;++channel) {
            const auto value=channel<3?std::ldexp(float((codes[channel]>>7)?128+(codes[channel]&127):codes[channel]&127),
                (codes[channel]>>7)?int(codes[channel]>>7)-10:-9):float(index%4)*(1.f/3.f);
            rgba[index*4+channel]=value;const auto h=half(value);
            for(unsigned byte=0;byte<2;++byte) {
                const auto at=(offset(x,y,padded)+channel*2+byte)^1;
                assert(at<expected.size() && !touched[at]);touched[at]=true;
                expected[at]=h>>(byte*8);expectedLinear[index*8+channel*2+byte]=h>>(byte*8);
            }
        }
    }
    const NativeOffscreenDrawPlan source{p->sourceObject,width,height,width,base,tiles,3};
    {NativeEdram32Mutation mutation(owner);
     assert(recordCompletedNativeRgb7e3Replacement(mutation,source,rgba,Color7e3Rounding::XeniaShiftRound));mutation.commit();}
    assert(owner.knownTiles()==known && !owner.readColorWords(base,width,width,height));
    assert(owner.readKnownColorRows(base,width,width,height));
    assert(!owner.readKnownColorRows(base,width,width,height+1)); // Padding rows really remain UNKNOWN.
    const auto serial=owner.nextSerial();const auto packed=packNativeRgb7e3FloatResolve(c,owner,before,&why);
    assert(packed && *packed==expected && std::string_view(why)=="rgb7e3-float-packed-no-publication");
    assert(linearizeXenosExpanded16FloatStorage(p->destination,*packed)==std::optional{expectedLinear});
    assert(owner.nextSerial()==serial && owner.knownTiles()==known && before==std::vector<std::uint8_t>(before.size(),0xA5));
    assert(std::count(touched.begin(),touched.end(),true)==width*height*8);
    for(unsigned tile:{base,base+tiles/2,base+tiles-1}) {auto unknown=owner;assert(unknown.invalidate(tile,1));assert(!packNativeRgb7e3FloatResolve(c,unknown,before));}
    // Missing a single last-row word must not inherit its neighbours' knownness.
    NativeEdram32Bands oneMissing;
    std::vector<std::uint8_t> raw(width*(height-1)*4);assert(oneMissing.recordCompletedRawColorRows(base,width,width,height-1,raw,1));
    raw.resize((width-1)*4);assert(oneMissing.recordCompletedRawColorRows(base,width,width-1,1,raw,2,height-1));
    assert(!oneMissing.readKnownColorRows(base,width,width,height));assert(!packNativeRgb7e3FloatResolve(c,oneMissing,before));
    std::vector<std::uint8_t> destination=before;unsigned publications=0;
    const auto publish=[&](const auto& completed) {
        ++publications;const NativeSerializedResourceUpdate update{p->destination.baseAddress,{destination.data(),destination.size()},&before,&completed};
        return publishNativeSerializedResourceBatch(&update,1);
    };
    assert(publishNativeRgb7e3FloatResolve(c,owner,before,[&]{return sameNativeRgb7e3FloatInputs(c,c);},publish,&why));
    assert(destination==expected && publications==1);
    destination=before;auto changed=c;put(*changed.destinationHeader,32,word(*changed.destinationHeader,32)^0x1000);
    assert(!sameNativeRgb7e3FloatInputs(c,changed));
    assert(!publishNativeRgb7e3FloatResolve(c,owner,before,[&]{return sameNativeRgb7e3FloatInputs(c,changed);},publish,&why));
    assert(destination==before && publications==1 && std::string_view(why)=="changed-inputs");
    destination.back()^=1;const auto stale=destination;
    assert(!publishNativeRgb7e3FloatResolve(c,owner,before,[]{return true;},publish,&why));
    assert(destination==stale && publications==2 && std::string_view(why)=="publication");
    assert(!publishNativeRgb7e3FloatResolve(c,oneMissing,before,[]{return true;},publish));assert(publications==2);
    for(unsigned arg:{1u,4u,5u,6u}) {auto bad=c;bad.arguments[arg]=1;assert(!planNativeRgb7e3FloatResolve(bad));}
    for(unsigned at:{10368u,10372u,12408u,11068u,12816u,12820u,12824u,12828u,12832u}) {
        auto bad=c;put(*bad.state,at,word(*bad.state,at)^1);assert(!planNativeRgb7e3FloatResolve(bad));
    }
    for(unsigned at:{24u,28u,36u}) {auto bad=c;auto& h=*bad.surfaces->surfaces[0].header;put(h,at,word(h,at)^1);assert(!planNativeRgb7e3FloatResolve(bad));}
    for(unsigned i=0;i<6;++i) {auto bad=c;auto& h=*bad.destinationHeader;put(h,28+i*4,word(h,28+i*4)^1);assert(!planNativeRgb7e3FloatResolve(bad));}
    for(unsigned i=0;i<4;++i) {auto bad=c;put(*bad.pointed.rectangle,4*i,word(*bad.pointed.rectangle,4*i)^1);assert(!planNativeRgb7e3FloatResolve(bad));}
    for(unsigned address:{0u,0x8000005Du,0xFFFFF05Du,0x1FFFF05Du}) {
        auto bad=c;put(*bad.destinationHeader,32,address);assert(!planNativeRgb7e3FloatResolve(bad));
    }
    {auto bad=c;bad.state.reset();assert(!planNativeRgb7e3FloatResolve(bad));}
    {auto bad=c;bad.state->pop_back();assert(!planNativeRgb7e3FloatResolve(bad));}
    {auto bad=c;bad.surfaces.reset();assert(!planNativeRgb7e3FloatResolve(bad));}
    {auto bad=c;bad.surfaces->surfaces[0].header.reset();assert(!planNativeRgb7e3FloatResolve(bad));}
    {auto bad=c;bad.destinationHeader.reset();assert(!planNativeRgb7e3FloatResolve(bad));}
    {auto bad=c;bad.pointed.stack92.reset();assert(!planNativeRgb7e3FloatResolve(bad));}
    {auto bad=c;put(*bad.pointed.stack100,0,1);assert(!planNativeRgb7e3FloatResolve(bad));}
    {auto bytes=before;bytes.pop_back();assert(!packNativeRgb7e3FloatResolve(c,owner,bytes));}
    // A rectangle, source attachment, base, flags or destination from different
    // profiles must not become a partially accepted resize, crop or rebase.
    unsigned crossRefusals=0;
    const auto refuse=[&](const Gta5TileResolveCapture& bad){assert(!planNativeRgb7e3FloatResolve(bad));++crossRefusals;};
    for(const auto& row:AuthoredRgb7e3Profiles) {
        if(row.width==width)continue;const auto other=authoredProfile(row);
        {auto bad=c;bad.pointed.rectangle=other.pointed.rectangle;refuse(bad);}
        {auto bad=c;bad.destinationHeader=other.destinationHeader;refuse(bad);}
        {auto bad=c;bad.surfaces->surfaces[0].header=other.surfaces->surfaces[0].header;
         put(*bad.state,10368,word(*other.state,10368));put(*bad.state,10372,word(*other.state,10372));refuse(bad);}
        // Flags from the other row, unless another admitted row of this size takes them.
        if(other.arguments[1]!=c.arguments[1] && std::none_of(std::begin(NativeRgb7e3FloatResolveProfiles),std::end(NativeRgb7e3FloatResolveProfiles),
            [&](const auto& r){return r.width==width && r.height==height && r.flags==other.arguments[1];}))
            {auto bad=c;bad.arguments[1]=other.arguments[1];refuse(bad);}
        // Base alone: RB_COLOR_INFO from the other row, everything else ours.
        if(row.colorInfo!=word(*c.state,10372)) {
            auto bad=c;put(*bad.state,10372,row.colorInfo);put(*bad.surfaces->surfaces[0].header,28,row.colorInfo);refuse(bad);
        }
        // Everything from the other row except its rectangle.
        {auto bad=other;bad.pointed.rectangle=c.pointed.rectangle;refuse(bad);}
    }
    // Flags other than the row's exact value: depth clear, both clears, sample
    // select, controls and exponent bias all refuse, with or without bit8.
    for(unsigned flags:{0x100u,0x200u,0x300u,0x10u,0x2000u,0x04000000u,0x04000100u,0x0u}) {
        if(flags==c.arguments[1] || std::any_of(std::begin(NativeRgb7e3FloatResolveProfiles),std::end(NativeRgb7e3FloatResolveProfiles),
            [&](const auto& r){return r.width==width && r.height==height && r.flags==flags;}))continue; // Another admitted row.
        auto bad=c;bad.arguments[1]=flags;
        const char* reason=nullptr;assert(!planNativeRgb7e3FloatResolve(bad,&reason));
        assert(std::string_view(reason)=="copy-flags-subresource");
    }
    // The 823C2C68 8-rounded extent and off-by-one/odd pitch shapes refuse.
    for(const auto& shape:{std::array<unsigned,2>{width,(height+7)&~7u},{width,height-1},{width-1,height},{width+32,height}}) {
        if(shape[0]==width && shape[1]==height)continue;
        auto bad=c;put(*bad.pointed.rectangle,8,shape[0]);put(*bad.pointed.rectangle,12,shape[1]);
        assert(!planNativeRgb7e3FloatResolve(bad));
    }
    // A padded destination pitch (one more 32-texel unit) is not this copy.
    {auto bad=c;put(*bad.destinationHeader,28,word(*bad.destinationHeader,28)+(1u<<22));assert(!planNativeRgb7e3FloatResolve(bad));}
    // hiz pitch alone differing from the colour pitch.
    {auto bad=c;const auto info=word(*bad.state,10368)+(32u<<18);put(*bad.state,10368,info);
     put(*bad.surfaces->surfaces[0].header,24,info);assert(!planNativeRgb7e3FloatResolve(bad));}
    if(clearing) {
        // The clear vector is consumed: absent, misaligned or any other value refuses.
        {auto bad=c;bad.pointed.color.reset();assert(!planNativeRgb7e3FloatResolve(bad));}
        {auto bad=c;bad.pointed.color->pop_back();assert(!planNativeRgb7e3FloatResolve(bad));}
        {auto bad=c;bad.arguments[7]=0;assert(!planNativeRgb7e3FloatResolve(bad));}
        {auto bad=c;bad.arguments[7]+=4;assert(!planNativeRgb7e3FloatResolve(bad));}
        for(unsigned i=0;i<4;++i){auto bad=c;put(*bad.pointed.color,4*i,word(*bad.pointed.color,4*i)^1);
            const char* reason=nullptr;assert(!planNativeRgb7e3FloatResolve(bad,&reason));
            assert(std::string_view(reason)=="clear-color");}
        {auto changedColor=c;put(*changedColor.pointed.color,0,0x3F800000);assert(!sameNativeRgb7e3FloatInputs(c,changedColor));}
        // Caller contract: publication first, then the recorded clear. The word is
        // RB_COLOR_CLEAR from the original prefix (C0000000) and equals the
        // XeniaShiftRound 7e3 packing of the admitted vector {0,0,0,1}; the
        // 8-row-rounded footprint ends on a tile boundary, so every tile becomes
        // fully known, the pre-clear pixels are never republished and
        // neighbouring tiles keep their ownership.
        {std::array<std::uint32_t,4> vector{0,0,0,0x3F800000};unsigned packedClear=0;
         assert(packColor7e3(vector,packedClear,Color7e3Rounding::XeniaShiftRound) && packedClear==NativeRgb7e3FloatResolveClearWord);}
        auto cleared=owner;std::vector<std::uint8_t> neighbour(80*16*4,0x5A);
        assert(cleared.recordCompletedRawColorRows(base-1,80,80,16,neighbour,cleared.nextSerial()));
        assert(cleared.recordCompletedRawColorRows(base+tiles,80,80,16,neighbour,cleared.nextSerial()));
        const auto knownBefore=cleared.knownTiles();
        assert(packNativeRgb7e3FloatResolve(c,cleared,before)==std::optional{expected});
        const unsigned rows=nativeRgb7e3FloatResolveClearRows(height);
        assert(rows%8==0 && rows>=height && rows<height+8 && rows==(height+15)/16*16);
        {auto q=*p;q.clearsSource=false;auto o=cleared;assert(!recordNativeRgb7e3FloatResolveClear(o,q) && o.knownTiles()==knownBefore);}
        {auto q=*p;q.clearTiles+=1;auto o=cleared;assert(!recordNativeRgb7e3FloatResolveClear(o,q) && o.knownTiles()==knownBefore);}
        assert(recordNativeRgb7e3FloatResolveClear(cleared,*p) && cleared.knownTiles()==knownBefore-known+tiles);
        const auto clearWords=cleared.readColorWords(base,width,width,rows);
        assert(clearWords && clearWords->size()==std::size_t{width}*rows &&
            std::all_of(clearWords->begin(),clearWords->end(),[](auto v){return v==NativeRgb7e3FloatResolveClearWord;}));
        const auto afterClear=packNativeRgb7e3FloatResolve(c,cleared,before,&why);
        assert(afterClear && *afterClear!=expected);
        const auto clearLinear=linearizeXenosExpanded16FloatStorage(p->destination,*afterClear);assert(clearLinear);
        for(std::size_t i=0;i<clearLinear->size();i+=8)
            assert((*clearLinear)[i]==0 && (*clearLinear)[i+2]==0 && (*clearLinear)[i+4]==0 &&
                   (*clearLinear)[i+6]==0x00 && (*clearLinear)[i+7]==0x3C); // (0,0,0,1.0) binary16
        assert(cleared.readColorWords(base-1,80,80,16) && cleared.readColorWords(base+tiles,80,80,16));
        std::printf("RGB7E3_FLOAT_CLEAR_PASS width=%u height=%u rows=%u word=c0000000 tiles_known=%u "
                    "preclear_republished=0 neighbours_preserved=1\n",width,height,rows,tiles);
    } else {
        // A non-clearing copy does not read, and so does not require, the vector.
        auto noColor=c;noColor.pointed.color.reset();assert(planNativeRgb7e3FloatResolve(noColor));
    }
    std::printf("RGB7E3_FLOAT_RESOLVE_PASS captured_state=%u authored_source=1 pixels=%u storage_bytes=%u independent_tiling=1 binary16=1 endian=1 unknown_padding_preserved=1 stale_refused=1 gpu=0 gameplay=0\n",unsigned(capturedState),width*height,p->storage.sourceBytes);
    std::printf("RGB7E3_FLOAT_PROFILE_PASS width=%u height=%u base=%u source_clear=%u cross_profile_refusals=%u last_word_required=1 captured_state=%u\n",width,height,base,unsigned(clearing),crossRefusals,unsigned(capturedState));
}
}
int main(int argc,char** argv) {
    assert(argc==1 || argc==2);
    for(unsigned code=0;code<1024;++code) {
        unsigned decoded=0;assert(unpack7e3(code,decoded));const float value=colorFloat(decoded);
        const auto h=rgb7e3ToBinary16(code);assert(h==half(value));
        if(code) {
            assert(((decoded-0x38000000u)&8191u)==0); // Exact, no lost mantissa bits.
            assert(h==((decoded-0x38000000u)>>13));
        }
        for(unsigned channel=0;channel<3;++channel)assert(color7e3ToBinary16(code<<(channel*10))[channel]==h);
    }
    for(unsigned alpha=0;alpha<4;++alpha) {
        const float value=float(alpha)*(1.f/3.f);const auto bits=colorBits(value);
        const auto rtz=alpha?((bits-0x38000000u)>>13):0;
        assert(color7e3ToBinary16(alpha<<30)[3]==half(value) && rtz==half(value));
    }
    std::puts("RGB7E3_HALF_EXHAUSTIVE_PASS rgb_codes=1024 alpha_codes=4 rne_rtz_agree=1 host_float16_reference=1");
    for(const auto& row:AuthoredRgb7e3Profiles)check(authoredProfile(row),false);
    // T2 row near-misses: unpadded hi-Z pitch80, unpadded tiled pitch64 (2 units),
    // T1's base, flags0 and a not-admitted 40x23 row all refuse.
    {AuthoredRgb7e3Profile t2=AuthoredRgb7e3Profiles[3];unsigned t2Refusals=0;
     const auto refuseT2=[&](const AuthoredRgb7e3Profile& r){assert(!planNativeRgb7e3FloatResolve(authoredProfile(r)));++t2Refusals;};
     {auto r=t2;r.surfaceInfo=0x01400050;refuseT2(r);}
     {auto r=t2;r.fetch0=0x80800156;refuseT2(r);}
     {auto r=t2;r.colorInfo=0x30300;refuseT2(r);}
     {auto r=t2;r.flags=0;refuseT2(r);}
     refuseT2({40,23,0x00800028,0x3030F,0x009C00B0,0x80400156,0x2C027,0x100});
     assert(t2Refusals==5);}
    // Flag0x100 on either observed flags0 row refuses (row-exact flags).
    for(unsigned row=0;row<2;++row){auto bad=authoredProfile(AuthoredRgb7e3Profiles[row]);bad.arguments[1]=0x100;
        assert(!planNativeRgb7e3FloatResolve(bad));}
    if(argc==2)check(captured(argv[1]),true);
}
