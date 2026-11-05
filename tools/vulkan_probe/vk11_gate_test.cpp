// Host test of vk11_gate.h (clang++ -std=c++17). Shows: which driver versions and extension sets are accepted and what
// is enabled by name. Does not show: that a real 1.1 driver draws the game correctly with these extensions; that is seen
// on a device (the Galaxy Z Fold4 run of this attempt).
#include "vk11_gate.h"
#include <cstdio>
using namespace recomp::vk11;
static int fails = 0;
static bool same(Plan a, Plan b) {
    return a.accepted == b.accepted && a.legacy == b.legacy && a.enableDescriptorIndexing == b.enableDescriptorIndexing &&
        a.enableScalarBlockLayout == b.enableScalarBlockLayout && a.enableMirrorClampToEdge == b.enableMirrorClampToEdge &&
        a.enableBufferDeviceAddress == b.enableBufferDeviceAddress;
}
static void check(bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails; }
int main() {
    const std::uint32_t v10 = (1u << 22), v11 = Api11, v11p = Api11 | 128, v12 = Api12, v13 = (1u << 22) | (3u << 12) | 250;
    const Offered all{true, true, true, true}, none{false, false, false, false};
    const Plan core{true, false, false, false, false, false}, refused{false, true, false, false, false, false};
    check(same(plan(v12, none, true, true), core) && same(plan(v13, none, false, false), core) && same(plan(v13, all, true, true), core),
        "1.2 and newer: accepted, nothing added, whatever is offered or wanted");
    check(same(plan(v12 - 1, none, false, false), refused), "the last 1.1 patch below 1.2 without descriptor indexing: refused");
    check(same(plan(v11p, all, true, false), Plan{true, true, true, true, true, false}),
        "1.1.128 with everything offered (the Fold): accepted; descriptor indexing, scalar layout, mirror clamp enabled; no buffer address");
    check(same(plan(v11, Offered{true, false, false, false}, true, false), Plan{true, true, true, false, false, false}),
        "1.1 with descriptor indexing only: accepted, only that enabled, scalar layout not enabled although wanted");
    check(same(plan(v11, Offered{true, true, true, false}, false, false), Plan{true, true, true, false, true, false}),
        "scalar layout offered but not wanted: not enabled");
    check(same(plan(v11, Offered{false, true, true, true}, true, false), refused), "1.1 without descriptor indexing: refused");
    check(same(plan(v10, all, true, false), refused) && same(plan(v11 - 1, all, false, false), refused), "1.0 with everything offered: refused");
    check(same(plan(v11, Offered{true, true, true, false}, true, true), refused), "buffer addresses required but not offered: refused");
    check(same(plan(v11, all, false, true), Plan{true, true, true, false, true, true}), "buffer addresses required and offered: enabled");
    // the module transform: array lengths compared with constants, through a bitcast and directly, in both operand orders
    const std::uint32_t H0 = 0x07230203u, H1 = 0x00010300u;
    const std::uint32_t m[] = {H0, H1, 0, 40, 0,
        (2u << 16) | 17, 1,                        // OpCapability Shader
        (5u << 16) | 68, 2, 10, 20, 0,             // %10 = OpArrayLength %uint %20 0
        (4u << 16) | 124, 3, 11, 10,               // %11 = OpBitcast %int %10
        (5u << 16) | 171, 4, 12, 11, 30,           // %12 = OpINotEqual %bool %11 %30      -> %30 %30
        (5u << 16) | 68, 2, 13, 21, 0,             // %13 = OpArrayLength %uint %21 0
        (5u << 16) | 170, 4, 14, 31, 13,           // %14 = OpIEqual %bool %31 %13         -> %31 %31
        (5u << 16) | 171, 4, 15, 32, 33,           // %15 = OpINotEqual %bool %32 %33      (no length: kept)
        (5u << 16) | 171, 4, 16, 10, 13,           // %16 = OpINotEqual %bool %10 %13      (two lengths: kept)
        (5u << 16) | 128, 2, 17, 10, 32};          // %17 = OpIAdd %uint %10 %32           (not a comparison: kept)
    const std::size_t n = sizeof m / sizeof m[0];
    const auto out = withArrayLengthChecksSatisfied(m, n);
    bool same = out.size() == n;
    std::size_t changed = 0;
    for (std::size_t i = 0; same && i < n; ++i) if (out[i] != m[i]) ++changed;
    check(same && changed == 2 && out[19] == 30 && out[20] == 30 && out[29] == 31 && out[30] == 31,
        "module transform: the two length comparisons compare the other operand with itself; nothing else changes");
    check(out[34] == 32 && out[35] == 33 && out[39] == 10 && out[40] == 13 && out[44] == 10,
        "a comparison without a length, one of two lengths, and an addition are left alone");
    std::uint32_t bad[] = {H0, H1, 0, 10, 0, (9u << 16) | 68, 5};      // an instruction longer than the module
    check(withArrayLengthChecksSatisfied(bad, 7).size() == 7 && withArrayLengthChecksSatisfied(bad, 7)[5] == bad[5], "a malformed module comes back unchanged");
    std::uint32_t notSpirv[] = {1, 2, 3, 4, 5, (5u << 16) | 68, 2, 10, 20, 0, (5u << 16) | 171, 4, 12, 10, 30};
    check(withArrayLengthChecksSatisfied(notSpirv, 15)[13] == 10, "something that is not SPIR-V comes back unchanged");
    // a good comparison followed by an instruction that claims more words than are left: the WHOLE module comes back as it was
    const std::uint32_t late[] = {H0, H1, 0, 40, 0, (5u << 16) | 68, 2, 10, 20, 0, (5u << 16) | 171, 4, 12, 10, 30, (9u << 16) | 72, 5};
    const auto lateOut = withArrayLengthChecksSatisfied(late, sizeof late / sizeof late[0]);
    check(lateOut.size() == sizeof late / sizeof late[0] && lateOut[13] == 10 && lateOut[14] == 30,
        "a module that turns out malformed later comes back without the earlier rewrite");
    check(withArrayLengthChecksSatisfied(nullptr, 0).empty(), "nothing in, nothing out");
    std::printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
    return fails ? 1 : 0;
}
