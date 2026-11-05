#pragma once
// vk-worker-3 (2026-10-03): helpers of the diag-vkw3-prefetch switch (generic_renderer.cpp, "vk-worker-3" section). Header-only
// and free of renderer types so the unit test (tools/vk_worker3/tests/vkw3_unit_test.cpp) includes it alone.
//  - prefetchLines: software prefetch of a byte range (a hint: no architectural effect, never faults, stores nothing).
//  - prefetchFixed: Lines prefetches at p, p + 64, ... (unrolled at compile time: no loop, no count).
#include <cstddef>
#include <cstdint>
#include <utility>

namespace recomp::gpu::generic::vkw3 {

// Prefetch every 64-byte line of [p, p + bytes) (at most maxLines lines). Locality 2 = keep in L2 (PLDL2KEEP), 3 = L1.
template<int Locality>
inline unsigned prefetchLines(const void* p,std::size_t bytes,unsigned maxLines=64) noexcept {
    if(!p || !bytes)return 0;
    const auto a=reinterpret_cast<std::uintptr_t>(p)&~std::uintptr_t{63};
    const auto e=reinterpret_cast<std::uintptr_t>(p)+bytes;
    unsigned n=0;
#if defined(RECOMP_VKW3_MUTANT_LINES_SHORT) // test mutant: the range's last line is left out
    for(std::uintptr_t x=a;x+64<e && n<maxLines;x+=64,++n)__builtin_prefetch(reinterpret_cast<const void*>(x),0,Locality);
#else
    for(std::uintptr_t x=a;x<e && n<maxLines;x+=64,++n)__builtin_prefetch(reinterpret_cast<const void*>(x),0,Locality);
#endif
    return n;
}

template<int Locality,std::size_t... K>
inline void prefetchFixedImpl(const char* p,std::index_sequence<K...>) noexcept {(__builtin_prefetch(p+64*K,0,Locality),...);}
template<int Locality,std::size_t Lines>
inline void prefetchFixed(const void* p) noexcept {prefetchFixedImpl<Locality>(static_cast<const char*>(p),std::make_index_sequence<Lines>{});}

} // namespace recomp::gpu::generic::vkw3
