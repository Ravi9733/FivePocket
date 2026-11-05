#pragma once
// vk-worker-4 (2026-10-03): helpers of the diag-vkw4-* switches (generic_renderer.cpp, "vk-worker-4" section). Header-only and
// free of renderer types so the unit test (tools/vk_worker4/tests/vkw4_unit_test.cpp) includes it with guest_write_watch.h alone.
//
// Write-watch proofs (diag-vkw4-watch). WriteWatch::watch(physical, bytes) reads three per-page arrays (lastWrite_, gen_,
// protected_: ~1.1 MB for the 512 MB of physical memory) for every page of the range, every time; on the device those reads are
// mostly cache misses (v427 drive profile of recomp-render-w: watch() 1.76% of the thread, from the texture spans 0.80, the index
// cache 0.55 and the vertex arena 0.39). guest_write_watch.h now also keeps, per 64 KB region of physical memory (a 32 KB array),
// a generation that touch() bumps after it changed a page of the region (unprotect, generation, last-write frame), and the
// watch's frame at that touch. A proof is what one real watch() call returned for a range, with the sum of the range's region
// generations read BEFORE that call, taken only when the call returned a stamp (no page dynamic) and no region of the range was
// touched in the current frame. While the region sum is unchanged no page of the range was touched since, so every page is still
// protected with the same generation and its last write is as old as that call saw it (at least 8 frames, or never): watch()
// would return the same stamp again and change nothing (no protect call, no counter). The current-frame condition covers
// watch()'s frame test reading lastWrite = frame|1, one ahead of an even frame (such a page passes the test in its own frame and
// turns dynamic in the next). Proofs are only used while the watch's frame counter is below 2^31 and within 2^30 frames of the
// proof (no unsigned wrap in that test).
#include <cstdint>

namespace recomp::gpu::generic::vkw4 {

struct WatchProof {std::uint64_t regions=0,stamp=0;std::uint32_t physical=0,bytes=0,frame=0;bool valid=false;};

// The stamp a proof answers for [physical, physical + bytes), or false (the caller then calls watchRecord).
template<class W> inline bool proofHolds(const W& w,const WatchProof& p,std::uint32_t physical,std::uint32_t bytes,std::uint64_t& stamp) noexcept {
    if(!p.valid || p.physical!=physical || p.bytes!=bytes)return false;
    const std::uint32_t now=w.frameNow();
#if !defined(RECOMP_VKW4_MUTANT_NO_FRAME_GUARD) // (test mutant: no frame guard; only a 2^30-frame run can tell)
    if(now>=0x80000000u || now-p.frame>=0x40000000u)return false;
#endif
#if defined(RECOMP_VKW4_MUTANT_PROOF_NO_REGIONS) // test mutant: a proof answers without its region check (a written range keeps its stamp)
    stamp=p.stamp;return true;
#else
    if(w.regionSum(physical,bytes)!=p.regions)return false;
    stamp=p.stamp;return true;
#endif
}
// watch() itself, recording a proof of its answer in `p` when it may (see above); p.valid=false otherwise.
template<class W> inline std::uint64_t watchRecord(W& w,WatchProof& p,std::uint32_t physical,std::uint32_t bytes) {
    const std::uint64_t g=w.regionSum(physical,bytes); // (before watch() reads the pages)
    const std::uint32_t now=w.frameNow();
#if defined(RECOMP_VKW4_MUTANT_PROOF_NOT_QUIET) // test mutant: a proof also from a range touched in the current frame
    const bool quiet=true;
#else
    const bool quiet=w.regionsQuiet(physical,bytes,now);
#endif
    const std::uint64_t st=w.watch(physical,bytes);
    if(st!=~0ull && quiet)p={g,st,physical,bytes,now,true};else p.valid=false;
    return st;
}

// A texture binding's vk-worker-4 data (in its V363TexMemo, so it survives the binding's re-verification as the v363 memos do):
//  proof     bit 0: the watch proofs of its spans (base, then mip, in textureSpansStamp's order); any proof is a statement about
//            its own range only, so a binding can never use one wrongly
//  planned.. bit 2: the plan's spans (as textureSpansStamp got them) while planGen == Renderer::v363PlanGen, and whether the fetch
//            is a 2D / cube texture (texture() then checks `resolved` at physical address `at` first)
struct TexMemo {WatchProof proof[2]{};std::uint64_t planGen=0,baseBytes=0,mipBytes=0;std::uint32_t base=0,mip=0,at=0;bool planned=false,resolvable=false;};

// Attribute-setup memo (diag-vkw4-attrs): the replayed side effects of one setupAttributes() run, in its order (element by element:
// the vfetch-swizzle note, then the converted-kind check of the element's attribute).
enum : std::uint8_t {EventUnrepresentable=1,EventNoContainerEntry=2,EventConverted=3};
struct AttrEvent {std::uint8_t kind=0;std::uint8_t attribute=0;}; // (attribute: the index in the memo's attribute list, for EventConverted)

} // namespace recomp::gpu::generic::vkw4
