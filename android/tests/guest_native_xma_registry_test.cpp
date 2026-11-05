#include "guest_import_registry.h"
#include "native_xma_context_pool.h"
#include <cassert>
#include <cstdio>
using namespace recomp::bootstrap;
int main() {
    GuestAddressSpace space; assert(space.reserve());
    GuestImportRegistry registry; assert(registry.initialize(space,0x10000000,0x10000));
    GuestImportCall call;
    const auto dispatch=[&](const char* name,std::initializer_list<std::uint32_t> arguments) {
        call={};std::copy(arguments.begin(),arguments.end(),call.arguments.begin());
        return registry.dispatch(name,call).handled();
    };
    assert(!dispatch("__imp__XMACreateContext",{0x30000000}));
    assert(registry.bindPhysicalMemoryArena(0xA0000000,0x10000,0x1000000));
    assert(dispatch("__imp__MmAllocatePhysicalMemoryEx",{0,GuestXmaBackend::PoolBytes,0x204,0,UINT32_MAX,256}));
    const auto v=static_cast<std::uint32_t>(call.returnValue); assert(v && !(v&255));
    assert(dispatch("__imp__MmGetPhysicalAddress",{v}));
    const auto p=static_cast<std::uint32_t>(call.returnValue);
    auto pool=std::make_shared<recomp::audio::NativeXmaContextPool>(v,p,space.translate(v),
        [&](auto a,auto n,bool w){return registry.mapNativeAudioPhysical(a,n,w);});
    assert(pool->ready() && registry.bindNativeXma(pool));
    assert(!registry.bindNativeXma(pool));
    for(auto output:{0U,0x30000001U,0x7FEA1800U,0xFFFFFFFEU})
        assert(!dispatch("__imp__XMACreateContext",{output}));
    const auto read=[&](std::uint32_t a){const auto* b=space.translate(a);return
        (std::uint32_t(b[0])<<24)|(std::uint32_t(b[1])<<16)|(std::uint32_t(b[2])<<8)|b[3];};
    for(unsigned i=0;i<320;++i) {
        assert(dispatch("__imp__XMACreateContext",{0x30000000}) && call.returnValue==0);
        const auto c=read(0x30000000);assert(c==v+i*64);
        assert(dispatch("__imp__MmGetPhysicalAddress",{c}) && call.returnValue==p+i*64);
    }
    assert(dispatch("__imp__XMACreateContext",{0x30000000}) && call.returnValue==0xC0000017);
    assert(read(0x30000000)==0);
    assert(!dispatch("__imp__MmFreePhysicalMemory",{0,v}));
    assert(dispatch("__imp__XMAReleaseContext",{v+64}) && call.returnValue==0);
    assert(!dispatch("__imp__XMAReleaseContext",{v+64}));
    assert(dispatch("__imp__XMACreateContext",{0x30000000}) && read(0x30000000)==v+64);
    assert(!registry.mapNativeAudioPhysical(p+GuestXmaBackend::PoolBytes,4,false));
    registry.reset();
    assert(!dispatch("__imp__XMACreateContext",{0x30000000}));
    std::puts("NATIVE_XMA_REGISTRY_PASS: actual imported ABI, 320 allocations/physical indices, exhaustion/release/reuse, pool pinning and output bounds");
}
