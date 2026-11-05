#pragma once
// white-floor (2026-10-06, local-analysis/agents/ship-week/white-floor/RESULT.md): bound indexed draws WITHOUT a pixel shader.
//
// GTA V draws its opaque sun-shadow casters as bound indexed draws with no pixel shader bound (device +13068 == 0: a
// depth-only draw) and only its alpha-tested casters with one. The bound-indexed hook (genericBoundIndexed, attempt v16)
// refused every such draw before the renderer saw it ("bound-objects"), and would have refused it again for its missing
// pixel code ("shader-code"): 605 to 830 draws per frame in Franklin's hall and 1028 in a street frame, 85 to 98 % of each
// shadow cascade's indices (census K lines "why=bound-objects ... ps=00000000", all between the shadow atlas's clear and
// its resolve). The renderer itself has drawn null-pixel-shader draws with no fragment stage since v41 (inline draws and
// DrawVertices reach it that way); only this entry still kept them out.
//
// The rule, as two pure functions (the hook's two tests read them through renderer_boundary_trace.cpp's never-inlined
// wrappers boundPixelGate / boundPixelCodeMissing, which keep the hook's own control flow as it was):
#include <cstdint>

namespace recomp::gpu::boundnull {
// The "bound objects" test's pixel term (0 = refuse): the pixel shader object when one is bound; for none bound, non-zero
// (the draw goes on, depth-only) unless `skipNull` asks for the old refusal (files/private-trace/diag-bound-skip-null-ps).
inline std::uint32_t pixelGate(std::uint32_t pixelShaderObject,bool skipNull) noexcept {
#if defined(RECOMP_BOUND_NULL_PS_MUTANT) // host test mutant: the v470 rule (a draw without a pixel shader is refused)
    (void)skipNull;return pixelShaderObject;
#else
    return pixelShaderObject?pixelShaderObject:(skipNull?0u:1u);
#endif
}
// The "shader code" test's pixel term: a bound pixel shader needs its code (unreadable code is still a refusal); a draw
// with none bound needs none.
inline bool pixelCodeUsable(bool havePixelCode,std::uint32_t pixelShaderObject) noexcept {
#if defined(RECOMP_BOUND_NULL_PS_MUTANT)
    (void)pixelShaderObject;return havePixelCode;
#else
    return havePixelCode || !pixelShaderObject;
#endif
}
}
