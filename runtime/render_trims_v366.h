#pragma once
// v366 (exact; diag-v366-render-trims2): the hook-layer halves of the second round of render-thread trims, kept here so that host
// tests (tests/render_trims_v366_test.cpp) drive the same code as renderer_boundary_trace.cpp, which is not host-buildable.
#include <cstdint>
#include <vector>

namespace recomp::v366 {
// (d) The selected vertex binding's content version (renderer_boundary_trace.cpp's genericShaderCode). ContentVersions::version(key,
// bytes) compares the binding's whole patched copy with the bytes it last saw for `key` on every draw. The caller advances `serial`
// at every write of a binding (select / erase / clear: the only ways its bytes change). A call whose key, binding vector and serial
// equal those of the previous version() call, with no trim of the versions since, gets that call's answer without the compare: the
// entry still holds those bytes and that version (version() is the only writer of its key's entry; a trim is counted), and the bytes
// it would compare are the same bytes. Counted as version()'s own hit.
struct EffectiveVersionMemo {std::uint64_t key=0,serial=0,trims=0,version=0;const std::vector<std::uint8_t>* bytes=nullptr;bool valid=false;};
template<class Versions>
std::uint64_t effectiveVersion(Versions& versions,EffectiveVersionMemo& memo,bool on,std::uint64_t serial,std::uint64_t key,
        const std::vector<std::uint8_t>* bytes) {
    if(on && memo.valid && memo.key==key && memo.bytes==bytes &&
#if !defined(RECOMP_V366_MUTANT_MEMO_NO_SERIAL) // (test mutant: a re-selection of the same binding object is not noticed)
       memo.serial==serial &&
#endif
       memo.trims==versions.trims) {
        ++versions.hits;return memo.version;
    }
    const auto version=versions.version(key,*bytes);
    memo={key,serial,versions.trims,version,bytes,true};
    return version;
}
// (e) A binned scissor's tile-recording capture (renderer_boundary_trace.cpp's captureDriverScissor: the bound surfaces and a copy of
// the device state) is read only by Gta5TileRecordingDiagnostic::begin() / recordScissor(). With no recording pending the code calls
// begin() only for caller 823ABBC8 without diag-generic-first-scene; otherwise neither runs and the capture is dropped unused.
constexpr std::uint32_t FirstSceneCaller=0x823ABBC8u;
inline bool scissorCaptureUnused(bool on,bool noRecordingPending,bool genericFirstScene,std::uint32_t caller) {
#if defined(RECOMP_V366_MUTANT_SCISSOR_ANY_CALLER) // (test mutant: the first-scene caller's capture is skipped too)
    (void)caller;return on && noRecordingPending;
#else
    return on && noRecordingPending && (genericFirstScene || caller!=FirstSceneCaller);
#endif
}
}
