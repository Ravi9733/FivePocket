// Compile against a patched public PPC runtime header (or its private generated
// copy) plus SIMDe. Contains no title code/data. See host_guest_trace/README.md.
#include "guest_execution_guard.h"
#define PPC_CONFIG_H_INCLUDED
#define PPC_RUNTIME_TRAP() recomp::bootstrap::GuestExecutionGuard::stopForGuestTrap()
#include "ppc_context.h"

#include <array>
#include <cassert>
#include <cstring>

using namespace recomp::bootstrap;
template <typename U, typename S> void check() {
    constexpr unsigned bits = sizeof(U) * 8;
    const std::array<U, 7> values{0, 1, 2, U(-1), U(-2), U(1) << (bits-1), (U(1) << (bits-1))-1};
    GuestExecutionGuard guard;
    for (unsigned to = 0; to < 32; ++to) for (const auto a : values) for (const auto b : values) {
        S sa{}, sb{};
        std::memcpy(&sa, &a, sizeof(a)); std::memcpy(&sb, &b, sizeof(b));
        const bool expected = ((to&16) && sa<sb) || ((to&8) && sa>sb) || ((to&4) && a==b)
                || ((to&2) && a<b) || ((to&1) && a>b);
        const auto result = guard.run(1, [&] {
            GuestExecutionGuard::checkpoint("synthetic_trap");
            if (PPCTrapCondition<U>(to, a, b)) PPC_RUNTIME_TRAP();
        });
        assert(result.reason == (expected ? GuestExecutionStopReason::GuestTrap : GuestExecutionStopReason::Completed));
        assert(result.completedFunctionEntries == 1);
    }
}
int main() {
    check<std::uint32_t, std::int32_t>();
    check<std::uint64_t, std::int64_t>();
    PPCContext context{};
    context.r0.u64 = 0x100000000ULL;
    assert(PPCTrapCondition<std::uint32_t>(4, context.r0.u32, 0));
    assert(!PPCTrapCondition<std::uint64_t>(4, context.r0.u64, 0));
    assert(context.r0.u64 == 0x100000000ULL);
}
