#pragma once
// Host-test support for guest_parallel_wake.h (multicore plan WP2).
// guest_parallel_wake_test_stubs.cpp provides WEAK definitions of the
// executor-owned wake API: an executor translation unit that implements it (WP1)
// overrides them at link time. The stubs record every call, then invoke an
// optional observer synchronously on the calling thread, outside the stubs' own lock.
#include "guest_parallel_wake.h"

#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace recomp::test {

struct WakeRecord {
    std::uint32_t token{0}; // 0 when `all` is set.
    bool all{false};        // wakeAllGuestWaiters.
    std::thread::id thread{};
};
using WakeObserver = std::function<void(const WakeRecord&)>;

void setWakeObserver(WakeObserver observer);
// Returns every record since the previous call and clears the list.
std::vector<WakeRecord> takeWakeRecords();

} // namespace recomp::test
