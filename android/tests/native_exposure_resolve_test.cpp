#include "native_exposure_resolve.h"
#include "native_r32_float_resolve.h"
#include "rgb7e3_float_resolve_fixture.h"
#include "native_serialized_resource_batch.h"
#include <cstdio>
using namespace recomp::gpu;
using namespace rgb7e3_test;
namespace {
Gta5TileResolveCapture exposureAuthored() {
    auto c=authored();auto& h=*c.surfaces->surfaces[0].header;
    put(h,24,0x800050);put(h,28,0x702D0);put(h,36,0);
    put(*c.state,10368,0x800050);put(*c.state,10372,0x702D0);put(*c.state,11068,0x8C08);
    put(*c.pointed.rectangle,8,1);put(*c.pointed.rectangle,12,1);
    put(*c.destinationHeader,28,0x80400156);put(*c.destinationHeader,36,0);
    return c;
}
void check(const Gta5TileResolveCapture& c,bool actual) {
    // Reproduce the missing owner connection: neither existing physical-copy
    // route can consume this coherent RT7 state, regardless of display status.
    assert(!planNativeR32FloatResolve(c) && !planNativeRgb7e3FloatResolve(c));
    const char* why=nullptr;const auto p=planNativeExposureResolve(c,&why);
    if(!p)std::printf("EXPOSURE_COPY_REFUSED %s\n",why);assert(p);
    assert(p->destination.format==29 && p->destination.endian==1 && p->storage.description.format==32);
    assert(p->storage.sourceBytes==8192 && p->storage.linearBytes==8);
    constexpr auto model=NativeEdram64Model::Xenia40x16;
    const std::vector<std::uint8_t> prior(8192,0xA5);
    constexpr std::array<unsigned,12> patterns{0,0x8000,1,0x3FF,0x400,0x3C00,0xBC00,0x7BFF,0x7C00,0xFC00,0x7E23,0xFFFF};
    for(unsigned rotation=0;rotation<patterns.size();++rotation) {
        std::vector<std::uint8_t> raw(8),expected=prior;
        for(unsigned k=0;k<4;++k)for(unsigned b=0;b<2;++b) {
            const auto v=patterns[(k+rotation)%patterns.size()]>>(8*b);
            raw[k*2+b]=v;expected[(k*2+b)^1]=v; // Independent8in16 endian; pixel0 at byte0.
        }
        NativeEdram32Bands owner;assert(!packNativeExposureResolve(c,owner,prior,model));
        const NativeOffscreenDrawPlan target{p->sourceObject,1,1,80,720,2,7};
        {NativeEdram32Mutation tx(owner);assert(recordCompletedNativeColorDraw(tx,target,raw,model));tx.commit();}
        const auto serial=owner.nextSerial();const auto words=owner.readKnownColorRows(720,160,2,1);
        assert(words && !owner.readKnownColorRows(720,160,3,1) && !owner.knownTiles());
        const auto packed=packNativeExposureResolve(c,owner,prior,model);assert(packed && *packed==expected);
        assert(linearizeXenosExpanded16FloatStorage(p->destination,*packed)==std::optional{raw});
        assert(owner.nextSerial()==serial && owner.readKnownColorRows(720,160,2,1)==words);
        assert(!packNativeExposureResolve(c,owner,prior,NativeEdram64Model::Unspecified));
        NativeEdram32Bands missing;
        assert(missing.recordCompletedRawColorRows(720,160,1,1,{raw.begin(),raw.begin()+4},missing.nextSerial()));
        assert(!packNativeExposureResolve(c,missing,prior,model)); // Both RG and BA must exist.
        auto invalid=owner;assert(invalid.invalidate(720,1));assert(!packNativeExposureResolve(c,invalid,prior,model));
        NativeEdram32Bands wrong;
        assert(wrong.recordCompletedRawColorRows(1440,160,2,1,raw,wrong.nextSerial()));
        assert(!packNativeExposureResolve(c,wrong,prior,model));
        std::vector<std::uint8_t> destination=prior;unsigned calls=0;
        const auto publish=[&](const auto& bytes){++calls;
            const NativeSerializedResourceUpdate u{p->destination.baseAddress,{destination.data(),destination.size()},&prior,&bytes};
            return publishNativeSerializedResourceBatch(&u,1);};
        assert(publishNativeExposureResolve(c,owner,prior,model,[&]{return sameNativeExposureResolveInputs(c,c);},publish));
        assert(calls==1 && destination==expected);
        assert(!publishNativeExposureResolve(c,owner,prior,model,[]{return false;},publish) && calls==1);
        destination=prior;destination.back()^=1;const auto stale=destination;
        assert(!publishNativeExposureResolve(c,owner,prior,model,[]{return true;},publish));assert(calls==2 && destination==stale);
        auto shortBytes=prior;shortBytes.pop_back();assert(!packNativeExposureResolve(c,owner,shortBytes,model));
    }
    unsigned negatives=0;
    const auto reject=[&](const auto& b){assert(!planNativeExposureResolve(b));assert(!sameNativeExposureResolveInputs(c,b));++negatives;};
    for(unsigned arg:{1u,4u,5u,6u}){auto b=c;b.arguments[arg]=1;reject(b);}
    for(unsigned at:{10368u,10372u,12408u,11068u,12816u,12820u,12824u,12828u,12832u}) {
        auto b=c;put(*b.state,at,word(*b.state,at)^1);reject(b);
    }
    for(unsigned at:{24u,28u,36u}){auto b=c;auto& h=*b.surfaces->surfaces[0].header;put(h,at,word(h,at)^1);reject(b);}
    for(unsigned i=0;i<6;++i){auto b=c;put(*b.destinationHeader,28+4*i,word(*b.destinationHeader,28+4*i)^1);reject(b);}
    for(unsigned i=0;i<4;++i){auto b=c;put(*b.pointed.rectangle,i*4,word(*b.pointed.rectangle,i*4)^1);reject(b);}
    for(unsigned format:{0u,3u,6u,14u}){auto b=c;put(*b.state,10372,format*65536+720);put(*b.surfaces->surfaces[0].header,28,format*65536+720);reject(b);}
    {auto b=c;b.state.reset();reject(b);} {auto b=c;b.state->pop_back();reject(b);}
    {auto b=c;b.surfaces.reset();reject(b);} {auto b=c;b.surfaces->surfaces[0].header.reset();reject(b);}
    {auto b=c;b.destinationHeader.reset();reject(b);} {auto b=c;b.pointed.rectangle.reset();reject(b);}
    {auto b=c;b.pointed.stack92.reset();reject(b);} {auto b=c;put(*b.pointed.stack100,0,1);reject(b);}
    {auto b=c;put(*b.destinationHeader,32,word(*b.destinationHeader,32)^0x1000);assert(planNativeExposureResolve(b));assert(!sameNativeExposureResolveInputs(c,b));}
    std::printf("EXPOSURE_RESOLVE_PASS actual_state=%u pixels=1 format7_to_expand29=1 copy_storage32=1 bitwise=1 two_words_required=1 endian=1 padding_preserved=1 source_unchanged=1 stale_refused=1 negatives=%u gpu=0 gameplay=0\n",unsigned(actual),negatives);
}
}
int main(int argc,char** argv){assert(argc==1 || argc==2);check(exposureAuthored(),false);if(argc==2)check(captured(argv[1]),true);}
