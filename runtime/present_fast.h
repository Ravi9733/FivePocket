#pragma once
// v227: the presenter thread's scan-out mapping without per-pixel function calls or per-frame allocations (v226 profile:
// NativeDisplayGammaRamp::mapRgba 21% and NativeScalerDisplayState::mapRgba 8.5% of the recomp-present thread, which used
// ~47% of a core). Exact equivalents, checked against the originals by tests/present_fast_test.cpp:
//  - the identity scaler's mapping is rgbaSourceOffset(x,y) = (y*Width+x)*4 for every pixel of a Width x Height frame, so
//    NativeScalerDisplayState::mapRgba equals one copy of the whole frame (same preconditions);
//  - the gamma ramp's pixel() maps each colour byte through (sample(v,c)*255+511)/1023, a pure function of (v,c): three
//    256-entry tables built per frame give the same bytes; alpha is unchanged.
#include "native_display_gamma.h"
#include "native_identity_scaler.h"
#include "native_scaler_display_state.h"
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <vector>

extern "C" std::atomic<bool> recomp_v227_baseline; // defined (weak) in generic_renderer.cpp, set from diag-v227-baseline
namespace recomp::gpu::present {
inline bool baseline() noexcept { return recomp_v227_baseline.load(std::memory_order_relaxed); }
// NativeScalerDisplayState::mapRgba's preconditions and result, into `out` (resized; a reused buffer is not reallocated).
inline bool identityScalerInto(const NativeScalerDisplayState& scaler, const std::uint8_t* input, std::size_t bytes,
                               unsigned width, unsigned height, std::vector<std::uint8_t>& out) {
    using C = NativeIdentityScalerCandidate;
    constexpr std::size_t PixelBytes = std::size_t{C::Width} * C::Height * 4;
    if (!scaler.configured() || !input || bytes != PixelBytes || width != C::Width || height != C::Height ||
        reinterpret_cast<std::uintptr_t>(input) > std::numeric_limits<std::uintptr_t>::max() - bytes) return false;
    try { out.resize(PixelBytes); } catch (...) { return false; }
    std::memcpy(out.data(), input, PixelBytes);
    return true;
}
// NativeDisplayGammaRamp::mapRgba(p, bytes, same vector) in place.
inline bool gammaInPlace(const NativeDisplayGammaRamp& ramp, std::uint8_t* p, std::size_t bytes) {
    if (!p || !bytes || bytes % 4 || bytes > 1280 * 720 * 4) return false;
    std::array<std::array<std::uint8_t, 256>, 3> lut{};
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned v = 0; v < 256; ++v) lut[c][v] = static_cast<std::uint8_t>((ramp.sample(v, c) * 255u + 511u) / 1023u);
    for (std::size_t i = 0; i < bytes; i += 4) { p[i] = lut[0][p[i]]; p[i + 1] = lut[1][p[i + 1]]; p[i + 2] = lut[2][p[i + 2]]; }
    return true;
}
}
