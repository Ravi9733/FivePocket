#include "native_r32_float_resolve.h"
#include "rgb7e3_float_resolve_fixture.h"
#include "native_serialized_resource_batch.h"
#include <cstdio>
using namespace recomp::gpu;
using namespace rgb7e3_test;
namespace {
Gta5TileResolveCapture r32Authored(unsigned size=64) {
    auto c=authored();auto& s=*c.surfaces->surfaces[0].header;
    assert(size==1 || size==4 || size==16 || size==64);
    put(s,24,size==64?0x01000050:0x00800050);put(s,28,size==1?0x000E02D0:0x000E05A0);
    put(s,36,size==64?0x00FC01F8:size==16?0x003C0078:size==4?0x000C0018:0);
    put(*c.state,10368,word(s,24));put(*c.state,10372,word(s,28));
    put(*c.pointed.rectangle,8,size);put(*c.pointed.rectangle,12,size);
    const std::array<unsigned,6> f{size==64?0x80800156u:0x80400156u,0xA10000A4,
        size==64?0x7E03Fu:size==16?0x1E00Fu:size==4?0x6003u:0u,0x16D1,0,0x200};
    for(unsigned i=0;i<6;++i)put(*c.destinationHeader,28+4*i,f[i]);return c;
}
// Independent bank/pipe address formula at four bytes per texel, endian8in32.
unsigned r32Offset(unsigned x,unsigned y,unsigned pitch) {
    assert(pitch%32==0);
    const unsigned outer=((y/32)*(pitch/32)+x/32)*64,inner=(y/2%8)*8+x%8;
    const unsigned v=(outer+inner)*4,bank=y/16%2,pipe=(x/8%4)^((y/8%2)*2);
    return (v&15)|((y%2)*16)|((v&16)*2)|(pipe*64)|((v&224u)*8)|((v&~255u)*16)|(bank*2048);
}
void check(const Gta5TileResolveCapture& c,bool liveCapture,unsigned specialRotation=0) {
    const unsigned size=word(*c.pointed.rectangle,8),pitch=size==64?64:32;
    const unsigned base=size==1?720:1440;
    const char* why=nullptr;const auto p=planNativeR32FloatResolve(c,&why);
    if(!p)std::printf("R32_PLAN_REFUSED %s\n",why);assert(p);
    assert(p->destination.format==36 && p->destination.endian==2 && p->storage.description.format==6);
    assert(p->Base==base && p->Pitch==80 && p->Width==size && p->Height==size);
    assert(!planXenosTexture2D(p->destination)); // Raw address view is not RGBA8 sampling admission.
    NativeEdram32Bands owner;
    const std::vector<std::uint8_t> before(p->storage.sourceBytes,0xA5);
    assert(!packNativeR32FloatResolve(c,owner,before));
    std::vector<std::uint8_t> raw(size*size*4),expected=before;std::vector<bool> touched(before.size());
    // Include signed zeros, infinities and NaN payloads: byte copy must not
    // normalize them, although the preceding production shader requires finite output.
    constexpr unsigned special[]{0,0x80000000,0x7F800000,0xFF800000,0x7FC12345,1,0xFFFFFFFF};
    for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x) {
        const unsigned i=y*size+x,value=i<7?special[(i+specialRotation)%7]:(i*2654435761u);
        for(unsigned b=0;b<4;++b) {
            raw[i*4+b]=value>>(8*b);const unsigned at=(r32Offset(x,y,pitch)+b)^3;
            assert(at<expected.size() && !touched[at]);touched[at]=true;expected[at]=raw[i*4+b];
        }
    }
    assert(owner.recordCompletedRawColorRows(base,80,size,size,raw,owner.nextSerial()));
    assert(owner.knownTiles()==0 && owner.readKnownColorRows(base,80,size,size)); // Padding stays unknown.
    const auto serial=owner.nextSerial();const auto source=owner.readKnownColorRows(base,80,size,size);
    auto packed=packNativeR32FloatResolve(c,owner,before);assert(packed && *packed==expected);
    assert(linearizeXenosTexture2D(p->storage.description,*packed)==std::optional{raw});
    assert(owner.nextSerial()==serial && owner.readKnownColorRows(base,80,size,size)==source);
    assert(std::count(touched.begin(),touched.end(),true)==size*size*4);
    NativeEdram32Bands missing;
    if(size>1) {
        std::vector<std::uint8_t> rows(raw.begin(),raw.begin()+(size-1)*size*4);
        assert(missing.recordCompletedRawColorRows(base,80,size,size-1,rows,missing.nextSerial()));
        std::vector<std::uint8_t> last(raw.end()-size*4,raw.end()-4);
        assert(missing.recordCompletedRawColorRows(base,80,size-1,1,last,missing.nextSerial(),size-1));
    } // For1x1, the missing final word is an empty owner; never record zero rows.
    assert(!packNativeR32FloatResolve(c,missing,before));
    for(unsigned tile=base;tile<base+(size+15)/16;++tile){auto bad=owner;assert(bad.invalidate(tile,1));assert(!packNativeR32FloatResolve(c,bad,before));}
    {NativeEdram32Bands wrong;assert(wrong.recordCompletedRawColorRows(size==1?1440:720,80,size,size,raw,wrong.nextSerial()));
     assert(!packNativeR32FloatResolve(c,wrong,before));}
    std::vector<std::uint8_t> destination=before;unsigned calls=0;
    const auto publish=[&](const auto& bytes) {
        ++calls;const NativeSerializedResourceUpdate u{p->destination.baseAddress,{destination.data(),destination.size()},&before,&bytes};
        return publishNativeSerializedResourceBatch(&u,1);
    };
    assert(publishNativeR32FloatResolve(c,owner,before,[&]{return sameNativeR32FloatInputs(c,c);},publish));
    assert(calls==1 && destination==expected);destination=before;
    auto changed=c;put(*changed.destinationHeader,32,word(*changed.destinationHeader,32)^0x1000);
    assert(!sameNativeR32FloatInputs(c,changed));
    assert(!publishNativeR32FloatResolve(c,owner,before,[&]{return sameNativeR32FloatInputs(c,changed);},publish));assert(calls==1);
    destination.back()^=1;const auto stale=destination;
    assert(!publishNativeR32FloatResolve(c,owner,before,[]{return true;},publish));assert(calls==2 && destination==stale);
    for(unsigned arg:{1u,4u,5u,6u}) {auto bad=c;bad.arguments[arg]=1;assert(!planNativeR32FloatResolve(bad));}
    for(unsigned at:{10368u,10372u,12408u,11068u,12816u,12820u,12824u,12828u,12832u}) {
        auto bad=c;put(*bad.state,at,word(*bad.state,at)^1);assert(!planNativeR32FloatResolve(bad));
    }
    for(unsigned at:{24u,28u,36u}) {auto bad=c;auto& h=*bad.surfaces->surfaces[0].header;put(h,at,word(h,at)^1);assert(!planNativeR32FloatResolve(bad));}
    for(unsigned i=0;i<6;++i) {auto bad=c;put(*bad.destinationHeader,28+i*4,word(*bad.destinationHeader,28+i*4)^1);assert(!planNativeR32FloatResolve(bad));}
    for(unsigned i=0;i<4;++i) {auto bad=c;put(*bad.pointed.rectangle,4*i,word(*bad.pointed.rectangle,4*i)^1);assert(!planNativeR32FloatResolve(bad));}
    // Individually supported fields cannot be mixed across any two profiles.
    for(unsigned otherSize:{1u,4u,16u,64u})if(otherSize!=size) {
        const auto other=r32Authored(otherSize);
        {auto bad=c;bad.pointed.rectangle=other.pointed.rectangle;assert(!planNativeR32FloatResolve(bad));}
        {auto bad=c;bad.destinationHeader=other.destinationHeader;assert(!planNativeR32FloatResolve(bad));}
        {auto bad=c;bad.surfaces->surfaces[0].header=other.surfaces->surfaces[0].header;assert(!planNativeR32FloatResolve(bad));}
    }
    {auto bad=c;const unsigned wrong=0xE0000|(size==1?1440:720);
     put(*bad.state,10372,wrong);put(*bad.surfaces->surfaces[0].header,28,wrong);assert(!planNativeR32FloatResolve(bad));}
    for(unsigned unsupported:{0u,2u,8u,32u,128u,UINT32_MAX}) {
        auto bad=c;put(*bad.pointed.rectangle,8,unsupported);put(*bad.pointed.rectangle,12,unsupported);
        assert(!planNativeR32FloatResolve(bad));
    }
    {auto bad=c;bad.state.reset();assert(!planNativeR32FloatResolve(bad));}
    {auto bad=c;bad.state->pop_back();assert(!planNativeR32FloatResolve(bad));}
    {auto bad=c;bad.surfaces.reset();assert(!planNativeR32FloatResolve(bad));}
    {auto bad=c;bad.surfaces->surfaces[0].header.reset();assert(!planNativeR32FloatResolve(bad));}
    {auto bad=c;bad.destinationHeader.reset();assert(!planNativeR32FloatResolve(bad));}
    {auto bad=c;bad.pointed.stack92.reset();assert(!planNativeR32FloatResolve(bad));}
    {auto bad=c;put(*bad.pointed.stack100,0,1);assert(!planNativeR32FloatResolve(bad));}
    {auto shortBytes=before;shortBytes.pop_back();assert(!packNativeR32FloatResolve(c,owner,shortBytes));}
    std::printf("R32_FLOAT_RESOLVE_PASS captured_state=%u authored_source=1 pixels=%u storage_bytes=%u endian=2 bit_patterns_preserved=1 independent_tiling=1 last_word_required=1 stale_refused=1 source_unchanged=1 sampling_admission=0 gameplay=0\n",unsigned(liveCapture),size*size,p->storage.sourceBytes);
}
}
int main(int argc,char** argv){
    assert(argc==1 || argc==2);for(unsigned size:{64u,16u,4u})check(r32Authored(size),false);
    for(unsigned i=0;i<7;++i)check(r32Authored(1),false,i);
    if(argc==2)check(captured(argv[1]),true);
}
