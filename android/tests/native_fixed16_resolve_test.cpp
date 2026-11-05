#include "native_fixed16_resolve.h"
#include "native_geometry_sampler_plan.h"
#include <cassert>
#include <cstdio>

using namespace recomp::gpu;
using Bytes=std::vector<std::uint8_t>;
static XenosTextureDescription texture(unsigned w,unsigned h,unsigned pitch,bool tiled,unsigned endian) {
    // Authored header: float storage, signed channels, integer-number selector,
    // R111 swizzle. No real game payload or address is embedded here.
    return *XenosTextureDescription::decode({2u|0x154u|((pitch/32)<<22)|(tiled?0x80000000u:0u),
        0x10000u|30u|(endian<<6),(w-1)|((h-1)<<13),0x16D1,0,0x200});
}
static void put(Bytes& b,unsigned at,unsigned v) {
    for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(24-8*i);
}
static unsigned tiled16(unsigned x,unsigned y,unsigned pitch) {
    // Independently evaluated pinned ReX tiled16 equation, not a call to the
    // production private helper. This checks logBpp selection and byte mapping;
    // it is reference-code agreement, not independently captured Xbox output.
    const unsigned macro=((x/32)+(y/32)*(pitch/32))*256;
    const unsigned micro=((x%8)+((y&14)*4))*2;
    const unsigned offset=macro+(micro/16)*32+(micro%16)+(y%2)*16;
    return (offset/512)*4096+(y&16)*128+(offset&448)*4+
        ((((y&8)/4+x/8)%4)*64)+(offset%64);
}
int main() {
    // Exhaustive input space: compare integer bit packing with the compiler's
    // separately implemented IEEE binary16 conversion of the explicit float32
    // decoded value. This is a CPU rounding check, NOT a target GPU/Xbox oracle.
    for(unsigned i=0;i<65536;++i) {
        __extension__ typedef _Float16 Half;
        const Half value=static_cast<Half>(nativeFixed16ResolveValue(i));
        std::uint16_t expected;std::memcpy(&expected,&value,2);
        assert(nativeFixed16ResolveHalfRne(i)==expected);
        // Independent RTZ oracle: the compiler's nearest half, stepped toward
        // zero only if rounding increased its magnitude. No reused bit formula.
        if(std::abs(float(value))>std::abs(nativeFixed16ResolveValue(i)))--expected;
        assert(nativeFixed16ResolveHalfRtz(i)==expected);
    }
    assert(nativeFixed16ResolveHalfRne(0)==0);
    assert(nativeFixed16ResolveHalfRne(1)==0x1400);
    assert(nativeFixed16ResolveHalfRne(32767)==0x5000);
    assert(nativeFixed16ResolveHalfRne(32768)==0xD000);
    assert(nativeFixed16ResolveHalfRne(32769)==0xD000);
    assert(nativeFixed16ResolveHalfRne(65535)==0x9400);

    // Full producer->packing->storage import connection; vary both channels so
    // an accidental RG byte copy or wrong selected channel cannot pass.
    for(const auto extent:{std::array<unsigned,3>{256,256,256},{33,17,64},{1,1,32}})
    for(bool tiled:{false,true})for(unsigned endian=0;endian<4;++endian) {
        const auto [w,h,pitch]=extent;
        const auto d=texture(w,h,pitch,tiled,endian);
        const auto layout=planXenosTexture2D(d);assert(layout && layout->bytesPerTexel==2);
        assert(layout->linearBytes==w*h*2);
        if(tiled)assert(layout->sourceBytes==(w==256?131072u:w==33?4096u:3072u));
        Gta5Fixed16ClearPlan source{0x10300000,w,h,((w+79)/80)*80,0,((w+79)/80)*((h+15)/16),1,4,0,std::nullopt};
        NativeOffscreenResolveRequest request{0x64010000,0,0x64010100};
        NativeOffscreenResolveArguments args;args.stack92=args.stack100=Bytes(4);
        args.rectangle=Bytes(16);put(*args.rectangle,8,w);put(*args.rectangle,12,h);
        const auto plan=planNativeFixed16Resolve(source,request,args,d);assert(plan);
        Bytes rg(w*h*4),previous(layout->sourceBytes,0xA5);
        for(unsigned i=0;i<w*h;++i) {
            rg[i*4]=i;rg[i*4+1]=i>>8;rg[i*4+2]=~i;rg[i*4+3]=(~i)>>8;
        }
        const auto packed=packNativeFixed16Resolve(*plan,rg,previous);assert(packed && previous==Bytes(previous.size(),0xA5));
        const auto linear=linearizeXenosTexture2D(d,*packed);assert(linear && linear->size()==w*h*2);
        Bytes touched(previous.size());constexpr unsigned endianXor[]{0,1,3,2};
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
            const auto i=y*w+x;const auto half=nativeFixed16ResolveHalfRtz(i);
            const auto address=tiled?tiled16(x,y,pitch):(y*pitch+x)*2;
            for(unsigned c=0;c<2;++c) {
                const auto at=(address+c)^endianXor[endian];assert(at<touched.size() && !touched[at]);
                touched[at]=1;assert((*packed)[at]==std::uint8_t(half>>(c*8)));
                assert((*linear)[i*2+c]==std::uint8_t(half>>(c*8)));
            }
        }
        for(unsigned i=0;i<touched.size();++i)if(!touched[i])assert((*packed)[i]==0xA5);
        assert(planNativeGeometrySampler2D(d.fetch));
        const auto chain=planXenosUnpackedMipChain(d.fetch);assert(chain && chain->baseBytes==layout->sourceBytes);
        assert(linearizeXenosUnpackedMipChain(d.fetch,*packed,{})->front()==*linear);
        const auto small=Bytes(previous.size()-1);assert(!packNativeFixed16Resolve(*plan,rg,small));
        auto truncated=rg;truncated.pop_back();assert(!packNativeFixed16Resolve(*plan,truncated,previous));
        auto forged=*plan;forged.storage.bytesPerTexel=4;assert(!packNativeFixed16Resolve(forged,rg,previous));
        for(unsigned bit=0;bit<32;++bit) {auto bad=request;bad.flags=1u<<bit;assert(!planNativeFixed16Resolve(source,bad,args,d));}
        auto bad=request;bad.mip=1;assert(!planNativeFixed16Resolve(source,bad,args,d));
        bad=request;bad.slice=1;assert(!planNativeFixed16Resolve(source,bad,args,d));
        bad=request;bad.stack=UINT32_MAX-91;assert(!planNativeFixed16Resolve(source,bad,args,d));
        auto missing=args;missing.stack92.reset();assert(!planNativeFixed16Resolve(source,request,missing,d));
        missing=args;put(*missing.stack100,0,1);assert(!planNativeFixed16Resolve(source,request,missing,d));
        missing=args;put(*missing.rectangle,0,1);assert(!planNativeFixed16Resolve(source,request,missing,d));
        for(unsigned sign:{2u,3u}) {auto words=d.fetch;words[0]=(words[0]&~12u)|(sign<<2);
            assert(!planXenosTexture2D(*XenosTextureDescription::decode(words)));}
        auto words=d.fetch;words[1]=(words[1]&~63u)|6;assert(!planXenosTexture2D(*XenosTextureDescription::decode(words)));
    }
    // Existing shader/native sampler policies remain distinct from storage:
    // a biased sampler can have readable R16_FLOAT bytes but cannot be admitted.
    auto d=texture(256,256,256,true,1);auto biased=d.fetch;biased[4]|=1u<<12;
    assert(planXenosUnpackedMipChain(biased) && !planNativeGeometrySampler2D(biased));
    // Connected state/header ownership: retained cache objects are expected,
    // but active tiling, changed bindings or mismatched color state reject.
    Gta5Fixed16ClearPlan source{0x10300000,256,256,320,64,64,1,4,0,std::nullopt};
    Bytes state(13860),header(40);Gta5BoundSurfaceCapture bindings{};
    put(header,24,0x04000140);put(header,28,0x00040040);put(header,36,0x03FC07F8);
    put(state,10368,0x04000140);put(state,10372,0x00040040);put(state,12816,source.object);
    bindings.surfaces[0].object=source.object;bindings.surfaces[0].header=header;
    for(unsigned i=0;i<5;++i) {bindings.surfaces[i].tileCacheObject=0x10400000+i*128;
        put(state,13104+i*4,bindings.surfaces[i].tileCacheObject);}
    assert(nativeFixed16ResolveSourceMatches(source,state,bindings));
    auto broken=state;broken[11068]=1;assert(!nativeFixed16ResolveSourceMatches(source,broken,bindings));
    broken=state;put(broken,10372,0x00040041);assert(!nativeFixed16ResolveSourceMatches(source,broken,bindings));
    broken=state;put(broken,12820,source.object);assert(!nativeFixed16ResolveSourceMatches(source,broken,bindings));
    auto badBindings=bindings;badBindings.surfaces[0].header.reset();assert(!nativeFixed16ResolveSourceMatches(source,state,badBindings));
    badBindings=bindings;badBindings.surfaces[0].tileCacheObject=0;assert(!nativeFixed16ResolveSourceMatches(source,state,badBindings));
    std::puts("NATIVE_FIXED16_RESOLVE_PASS rounding=65536 layouts=24 padding=1 source_to_texture=1 negative_controls=1 gpu_verified=0");
}
