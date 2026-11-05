#include "native_color8_resolve.h"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <string>
using namespace recomp::gpu;
using Bytes=std::vector<std::uint8_t>;
void put(Bytes& b,unsigned at,unsigned v){for(unsigned i=0;i<4;++i)b[at+i]=v>>(24-8*i);}
unsigned word(const Bytes& b,unsigned at){return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];}
Bytes read(const std::string& p) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);assert(f);const auto n=f.tellg();assert(n>=0 && n<=1024*1024);f.seekg(0);
    Bytes b(n);if(n)assert(f.read(reinterpret_cast<char*>(b.data()),n));return b;
}
int main(int argc,char** argv) {
    assert(argc==1 || argc==2);
    Bytes pairs(256*256*2*4);unsigned at=0;
    for(unsigned a=0;a<256;++a)for(unsigned b=0;b<256;++b)for(unsigned s:{a,b})for(unsigned c=0;c<4;++c)pairs[at++]=s;
    // Independent double-precision equation, with ties decided mathematically.
    // Both orderings appear, including every alpha tie and all PWL segments.
    unsigned encodedDifferences=0;
    for(unsigned format:{0u,1u}) {
        const auto out=resolveNativeColor8LinearAverage(128,512,2,format,pairs);assert(out);
        const auto before=pairs;
        for(unsigned a=0;a<256;++a)for(unsigned b=0;b<256;++b)for(unsigned c=0;c<4;++c) {
            const auto decode=[](unsigned x) {
                double y=x;if(x>=192){y=8*x-1024;y+=std::trunc(y/128);}
                else if(x>=96){y=4*x-256;y+=std::trunc(y/256);}
                else if(x>=64){y=2*x-64;y+=std::trunc(y/512);}
                return y/1023.;
            };
            const double value=format && c<3?(decode(a)+decode(b))*(255./2.):(a+b)/2.;
            const auto floor=unsigned(std::floor(value));const auto fraction=value-floor;
            const bool tie=std::abs(fraction-.5)<1e-10;
            const unsigned expected=floor+(tie?(floor&1):fraction>.5);
            const auto actual=(*out)[(a*256+b)*4+c];assert(actual==expected);
            if(format && c==0)encodedDifferences+=actual!=((a+b)/2+((a+b)%2 && (((a+b)/2)&1)));
        }
        assert(pairs==before);
    }
    for(unsigned samples:{1u,2u,4u}) {
        const Bytes b(13*7*samples*4,255);const auto out=resolveNativeColor8LinearAverage(13,7,samples,1,b);
        assert(out && *out==Bytes(13*7*4,255));
    }
    for(unsigned bad:{0u,3u,8u})assert(!resolveNativeColor8LinearAverage(1,1,bad,1,Bytes(4*bad)));
    assert(!resolveNativeColor8LinearAverage(1,1,2,2,Bytes(8)));
    assert(!resolveNativeColor8LinearAverage(0,1,2,1,{}));
    assert(!resolveNativeColor8LinearAverage(128,512,2,1,std::span<const std::uint8_t>(pairs).first(pairs.size()-1)));
    assert(!resolveNativeColor8LinearAverage(UINT32_MAX,UINT32_MAX,2,1,{}));
    // Authored descriptor: gamma-sampled cube, mip0/face0, tiled BGRA/endian2.
    auto d=*XenosTextureDescription::decode({0x810000FE,0x00100086,0x140FE07F,0xC14,0,0x600});
    const auto destination=planNativeColor8ResolveFace0(d);assert(destination);
    assert(destination->original.dimension==XenosTextureDescription::Dimension::Cube && destination->original.depthOrLayers==6);
    assert((destination->storage.sourceBytes==65536 && destination->storage.description.sign==std::array<unsigned,4>{}));
    assert(!planXenosTexture2D(d)); // Sampling support has NOT been widened.
    Gta5Color8ClearPlan p{0x1000,128,128,160,0,32,2,1,{0,0,0,255},{}};
    p.depth=Gta5Depth24ClearPlan{0x2000,128,128,160,32,32,2,1,*planExactXenosFloat24Clear(0,255)};
    Bytes state(13860);Gta5BoundSurfaceCapture bindings;
    put(state,10368,0x20100A0);put(state,10372,0x10000);put(state,10376,0x10020);
    for(unsigned slot:{0u,4u}) {
        auto& b=bindings.surfaces[slot];b.object=slot?p.depth->object:p.object;b.header=Bytes(40);
        put(*b.header,24,0x20100A0);put(*b.header,28,slot?0x10020:0x10000);put(*b.header,36,(127u<<18)|(127u<<3));
        put(state,12816+4*slot,b.object);
    }
    assert(nativeColor8ResolveSourceMatches(p,state,bindings));
    // Historical clear depth is irrelevant to color resolve/clear.
    p.depth->value=*planExactXenosFloat24Clear(.75,17);assert(nativeColor8ResolveSourceMatches(p,state,bindings));
    for(unsigned offset:{10368u,10372u,10376u,12816u,12832u,13104u,13120u}) {
        state[offset+3]^=1;assert(!nativeColor8ResolveSourceMatches(p,state,bindings));state[offset+3]^=1;
    }
    state[11068]=0x10;assert(!nativeColor8ResolveSourceMatches(p,state,bindings));state[11068]=0;
    bindings.surfaces[4].header->pop_back();assert(!nativeColor8ResolveSourceMatches(p,state,bindings));bindings.surfaces[4].header->push_back(0xF8);
    const auto good=p;p.depth->baseTile=0;assert(!nativeColor8ResolveSourceMatches(p,state,bindings));p=good;
    auto changed=d;changed.width=1;assert(planNativeColor8ResolveFace0(changed)->original.width==128);
    for(const auto& bad:std::array<std::array<unsigned,2>,10>{{{0,4},{0,0x100},{1,1024},{3,1},{3,0x2000},
            {3,0x80000000},{4,4},{4,64},{5,0x800},{5,0x1000}}}) {
        changed=d;changed.fetch[bad[0]]^=bad[1];assert(!planNativeColor8ResolveFace0(changed));
    }
    changed=d;changed.fetch[5]=0x400;assert(!planNativeColor8ResolveFace0(changed));
    changed=d;changed.fetch[2]^=1;assert(!planNativeColor8ResolveFace0(changed));
    changed=d;changed.fetch[2]&=0x03FFFFFF;assert(!planNativeColor8ResolveFace0(changed));
    Bytes sample(p.width*p.height*8);for(unsigned i=0;i<sample.size();++i)sample[i]=(i*73+i/517)%256;
    const auto sampleBefore=sample;const auto originalDescription=d.fetch;
    for(unsigned endian=0;endian<4;++endian)for(unsigned swap=0;swap<2;++swap) {
        changed=d;changed.fetch[1]=(changed.fetch[1]&~0xC0u)|(endian<<6);changed.fetch[3]=swap?0xC14:0xD10;
        const auto view=*planNativeColor8ResolveFace0(changed);
        Bytes allocation(6*65536,0xA5),previous(allocation.begin(),allocation.begin()+65536);
        const auto packed=packNativeColor8ResolveFace0(p,changed,sample,previous);assert(packed);
        assert(previous==Bytes(65536,0xA5));std::copy(packed->begin(),packed->end(),allocation.begin());
        assert(std::all_of(allocation.begin()+65536,allocation.end(),[](auto b){return b==0xA5;}));
        auto decoded=*linearizeXenosTexture2D(view.storage.description,packed->data(),packed->size());
        if(swap)for(unsigned i=0;i<decoded.size();i+=4)std::swap(decoded[i],decoded[i+2]);
        assert(decoded==*resolveNativeColor8LinearAverage(p.width,p.height,2,1,sample));
        assert(!packNativeColor8ResolveFace0(p,changed,sample,Bytes(65535)));
    }
    assert(sample==sampleBefore && d.fetch==originalDescription);
    // This bounded layout also agrees with the isolated original82343AD8
    // fixture (30cases). Authored storage is a packing/ownership test, not an
    // Xbox rendered-output oracle. Every face must have its own address/range.
    unsigned faceCases=0;
    for(unsigned size:{32u,64u,128u,256u,512u}) {
        auto cube=d;cube.fetch[0]=(cube.fetch[0]&~0x7FC00000u)|((size/32)<<22);
        cube.fetch[2]=(5u<<26)|((size-1)<<13)|(size-1);
        const auto faceBytes=size*size*4;
        auto source=p;source.width=source.height=size;source.pitch=((size+79)/80)*80;
        Bytes values(size*size*8);for(unsigned i=0;i<values.size();++i)values[i]=(i*73+i/517)%256;
        const auto linear=*resolveNativeColor8LinearAverage(size,size,2,1,values);
        for(unsigned face=0;face<6;++face) {
            const auto view=planNativeColor8ResolveFace(cube,face);assert(view);
            assert(view->storage.description.baseAddress==d.baseAddress+face*faceBytes);
            assert(view->storage.sourceBytes==faceBytes && view->original.baseAddress==d.baseAddress);
            Bytes allocation(6*faceBytes,0xA5),previous(faceBytes,0xA5);
            const auto packed=packNativeColor8ResolveFace(source,cube,values,previous,face);assert(packed);
            const auto start=allocation.begin()+face*faceBytes;
            std::copy(packed->begin(),packed->end(),start);
            assert(std::all_of(allocation.begin(),start,[](auto b){return b==0xA5;}));
            assert(std::all_of(start+faceBytes,allocation.end(),[](auto b){return b==0xA5;}));
            auto actual=*linearizeXenosTexture2D(view->storage.description,packed->data(),packed->size());
            for(unsigned i=0;i<actual.size();i+=4)std::swap(actual[i],actual[i+2]);
            assert(actual==linear && previous==Bytes(faceBytes,0xA5));++faceCases;
        }
        assert(!planNativeColor8ResolveFace(cube,6) && !planNativeColor8ResolveFace(cube,UINT32_MAX));
    }
    changed=d;changed.fetch[1]=0xFFFF0086;
    assert(planNativeColor8ResolveFace(changed,0)); // Last exact64KiB range fits.
    assert(!planNativeColor8ResolveFace(changed,1)); // Next face wraps4GiB.
    changed=d;changed.fetch[0]+=(1u<<22);assert(!planNativeColor8ResolveFace(changed,1)); // Padded pitch not admitted.
    for(unsigned size:{16u,96u,1024u}) {
        changed=d;changed.fetch[0]=(changed.fetch[0]&~0x7FC00000u)|((size/32)<<22);
        changed.fetch[2]=(5u<<26)|((size-1)<<13)|(size-1);
        assert(!planNativeColor8ResolveFace(changed,1));
    }
    auto ordinary=d;ordinary.fetch[2]&=0x03FFFFFFu;ordinary.fetch[5]=0x200;
    assert(planNativeColor8ResolveFace(ordinary,0) && !planNativeColor8ResolveFace(ordinary,1));
    NativeOffscreenResolveRequest request{0x10000,0x100,0,0,0,0,0x10100};
    NativeOffscreenResolveArguments args;args.color=Bytes(16);args.stack92=args.stack100=Bytes(4);
    const float half=.5f;unsigned bits;std::memcpy(&bits,&half,4);for(unsigned c=0;c<4;++c)put(*args.color,4*c,bits);
    const auto audit=auditNativeColor8ResolveArguments(p,request,args);assert(audit.plan);
    assert((audit.plan->clearAfterCopy==std::array<std::uint8_t,4>{192,192,192,128}));
    request.slice=1;assert(!auditNativeColor8ResolveArguments(p,request,args).plan);request.slice=0;
    for(unsigned face=0;face<6;++face) {
        request.slice=face;assert(auditNativeColor8ResolveArguments(p,request,args,&d).plan);
    }
    request.slice=6;assert(!auditNativeColor8ResolveArguments(p,request,args,&d).plan);
    request.slice=1;assert(!auditNativeColor8ResolveArguments(p,request,args,&ordinary).plan);
    request.mip=1;assert(!auditNativeColor8ResolveArguments(p,request,args,&d).plan);request.slice=0;
    request.mip=1;assert(!auditNativeColor8ResolveArguments(p,request,args).plan);request.mip=0;
    request.flags|=0x200;assert(!auditNativeColor8ResolveArguments(p,request,args).plan);
    std::printf("NATIVE_COLOR8_RESOLVE_PASS exhaustive_pairs=65536 formats=2 channels=4 differing_encoded_means=%u face0_endian_swap=8 cube_face_cases=%u other_faces_preserved=1 xbox_oracle=0\n",encodedDifferences,faceCases);
    if(argc==2) {
        const std::string dir=argv[1];const auto header=read(dir+"/destination-resource.bin");
        const auto actual=describeGta5TextureHeader(header);assert(actual);
        const auto view=planNativeColor8ResolveFace0(*actual);assert(view);
        auto samples=read(dir+"/source-samples-rgba8-part-0.bin");const auto tail=read(dir+"/source-samples-rgba8-part-1.bin");
        samples.insert(samples.end(),tail.begin(),tail.end());const auto liveState=read(dir+"/device-state.bin");
        Gta5BoundSurfaceCapture liveBindings;
        for(unsigned i=0;i<5;++i) {
            auto& b=liveBindings.surfaces[i];b.object=word(liveState,12816+4*i);b.tileCacheObject=word(liveState,13104+4*i);
            const auto h=read(dir+"/bound-surface-"+std::to_string(i)+"-resource.bin");if(!h.empty())b.header=h;
        }
        p.object=liveBindings.surfaces[0].object;p.depth->object=liveBindings.surfaces[4].object;
        assert(nativeColor8ResolveSourceMatches(p,liveState,liveBindings));
        const auto resolved=resolveNativeColor8LinearAverage(128,128,2,1,samples);assert(resolved);
        const auto packed=packNativeColor8ResolveFace0(p,*actual,samples,Bytes(view->storage.sourceBytes,0xA5));assert(packed);
        std::printf("CAPTURED_COLOR8_RESOLVE_PASS dimensions=%ux%u layers=%u format=%u signs=%u,%u,%u,%u base=%08x face0_bytes=%u source_samples=%zu packed=%zu original_destination_storage_available=%u\n",
            actual->width,actual->height,actual->depthOrLayers,actual->format,actual->sign[0],actual->sign[1],actual->sign[2],actual->sign[3],
            actual->baseAddress,view->storage.sourceBytes,samples.size(),packed->size(),unsigned(!read(dir+"/destination-storage.bin").empty()));
    }
}
