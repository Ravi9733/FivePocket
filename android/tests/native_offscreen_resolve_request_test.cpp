#include "native_offscreen_resolve_request.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <map>
using namespace recomp::gpu;
using Bytes=std::vector<std::uint8_t>;
void word(Bytes& b,unsigned at,std::uint32_t v){for(unsigned c=0;c<4;++c)b[at+c]=v>>(24-8*c);}
void scalar(Bytes& b,unsigned at,float f){std::uint32_t bits;std::memcpy(&bits,&f,4);word(b,at,bits);}
int main() {
    NativeOffscreenDrawPlan source{0x1000,512,512,560,1601,224};
    NativeOffscreenResolveRequest request{0x64000000,0x100,0x64000090,0,0,0,0x64000080};
    std::map<std::uint32_t,Bytes> memory{{request.stack+92,Bytes(4)},
        {request.stack+100,Bytes(4)},{request.rectangle,Bytes(16)},{request.clearColor,Bytes(16)}};
    word(memory[request.rectangle],8,512);word(memory[request.rectangle],12,512);
    unsigned reads=0;
    const Gta5ResourceReader reader=[&](auto at,auto count)->std::optional<Bytes> {
        ++reads;const auto i=memory.find(at);
        return i!=memory.end() && i->second.size()==count?std::optional{i->second}:std::nullopt;
    };
    const auto check=[&](auto r) {
        return auditNativeOffscreenResolve(source,r,captureNativeOffscreenResolveArguments(r,reader));
    };
    auto good=check(request);assert(good.plan && !good.issues && good.plan->clearAfterCopy);
    assert((*good.plan->clearAfterCopy==std::array<std::uint8_t,4>{0,0,0,0}));
    auto simple=request;simple.flags=0;simple.rectangle=simple.clearColor=0;
    assert(check(simple).plan && !check(simple).plan->clearAfterCopy);
    // With no depth-clear flag there is deliberately no f1 constraint in this
    // contract. Likewise the color pointer is not used without color-clear.
    simple.clearColor=UINT32_MAX;assert(check(simple).plan);
    for(unsigned i=0;i<256;++i) {
        for(unsigned c=0;c<4;++c)scalar(memory[request.clearColor],4*c,float((i+17*c)%256)/255.f);
        const auto a=check(request);assert(a.plan && a.plan->clearAfterCopy);
        for(unsigned c=0;c<4;++c)assert((*a.plan->clearAfterCopy)[c]==(i+17*c)%256);
    }
    for(unsigned i=0;i<255;++i) {
        const float f=(float(i)+.5f)/255.f;const volatile float scaled=f*255.f;
        // Test exact ties that survive the original single-precision multiply.
        if(scaled!=float(i)+.5f)continue;
        for(unsigned c=0;c<4;++c)scalar(memory[request.clearColor],4*c,f);
        const auto a=check(request);assert(a.plan && (*a.plan->clearAfterCopy)[0]==i+(i&1));
    }
    for(float bad:{-.1f,1.1f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
        scalar(memory[request.clearColor],0,bad);const auto a=check(request);
        assert(!a.plan && a.issues==ResolveClearColor);
    }
    memory[request.clearColor]=Bytes(16);
    for(unsigned bit=0;bit<32;++bit)if(bit!=8) {
        auto r=request;r.flags|=std::uint32_t{1}<<bit;const auto a=check(r);
        assert(!a.plan && a.issues==ResolveFlags);
    }
    for(unsigned field=0;field<4;++field) {
        word(memory[request.rectangle],field*4,1);assert(check(request).issues==ResolveRectangle);
        word(memory[request.rectangle],field*4,field<2?0:512);
    }
    auto point=request;point.destinationPoint=0x640000B0;memory[point.destinationPoint]=Bytes(8);
    assert(check(point).plan);word(memory[point.destinationPoint],4,1);
    assert(check(point).issues==ResolveDestinationPoint);
    auto bad=request;bad.flags|=0x200;bad.rectangle=0x64000100;bad.clearColor=0;
    bad.mip=1;bad.slice=2;bad.destinationPoint=point.destinationPoint;
    word(memory[request.stack+92],0,1);word(memory[request.stack+100],0,2);
    reads=0;auto all=check(bad);
    assert(!all.plan && all.issues==(ResolveFlags|ResolveRectangle|ResolveDestinationPoint|
        ResolveSubresource|ResolveStack92|ResolveStack100|ResolveClearColor));
    assert(reads==4); // Not stopped after flags or first missing pointer.
    memory[request.stack+92]=Bytes(4);memory[request.stack+100]=Bytes(4);
    for(unsigned n:{0u,3u,15u,17u}) {
        auto args=captureNativeOffscreenResolveArguments(request,reader);
        args.color=Bytes(n);assert(auditNativeOffscreenResolve(source,request,args).issues==ResolveClearColor);
        args=captureNativeOffscreenResolveArguments(request,reader);
        args.rectangle=Bytes(n);assert(auditNativeOffscreenResolve(source,request,args).issues==ResolveRectangle);
    }
    bad=request;bad.stack=UINT32_MAX;reads=0;all=check(bad);
    assert(!all.plan && (all.issues&(ResolveStack92|ResolveStack100))==(ResolveStack92|ResolveStack100));
    assert(reads==2); // No overflowing stack read was attempted.
    bad=request;bad.clearColor+=4;assert(check(bad).issues==ResolveClearColor);
    auto zeroSource=source;zeroSource.width=0;
    assert(auditNativeOffscreenResolve(zeroSource,simple,captureNativeOffscreenResolveArguments(simple,reader)).issues==ResolveExtent);
    unsigned heapReads=0,graphicsReads=0;bool available=true;
    Gta5ResourceReader heap=[&](auto a,auto n)->std::optional<Bytes>{assert(a<0x20000000 && n==52);++heapReads;return Bytes(52);};
    Gta5ResourceReader graphics=[&](auto a,auto n)->std::optional<Bytes>{assert(a>=0x20000000 && n==52);++graphicsReads;return available?std::optional{Bytes(52)}:std::nullopt;};
    assert(captureNativeResolveTextureHeader(0x10001000,heap,graphics));
    assert(captureNativeResolveTextureHeader(0xABFBABB4,heap,graphics));
    available=false;assert(!captureNativeResolveTextureHeader(0xABFBABB4,heap,graphics));
    assert(heapReads==1 && graphicsReads==2); // Never fall back to unowned heap.
    for(auto a:{0u,0xABFBABB5u,0xFFFFFFFCu})assert(!captureNativeResolveTextureHeader(a,heap,graphics));
    assert(heapReads==1 && graphicsReads==2);
    std::puts("OFFSCREEN_RESOLVE_REQUEST_PASS full/default/explicit rectangles, all UNORM codes/ties, multi-failure audit, owned namespaces, overflow and malformed arguments");
}
