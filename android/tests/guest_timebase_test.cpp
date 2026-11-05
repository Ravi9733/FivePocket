#include "guest_timebase.h"
#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace recomp::bootstrap;
int main() {
    static_assert(GuestTimebaseFrequency == 50000000);
    static_assert(guestTimebaseFromNanoseconds(1000000000) == GuestTimebaseFrequency);
    static_assert(guestTimebaseFromNanoseconds(UINT64_MAX) == 922337203685477580ULL);
    for (std::uint64_t n = 0; n < 100000; ++n) {
        const auto ticks = guestTimebaseFromNanoseconds(n);
        assert(ticks * 20 <= n && n - ticks * 20 < 20);
    }
    assert(guestTimebaseReadCount() == 0);
    using Clock = std::chrono::steady_clock;
    using Ns = std::chrono::nanoseconds;
    const auto before = Clock::now();
    const auto start = readGuestTimebase();
    const auto after = Clock::now();
    assert(guestTimebaseReadCount() == 1);
    auto previous = start;
    for (int i = 0; i < 10000; ++i) {
        const auto current = readGuestTimebase();
        assert(current >= previous);
        previous = current;
    }
    // Real elapsed-time bracket, no fragile fixed sleep or speed expectation.
    const auto endBefore = Clock::now();
    const auto end = readGuestTimebase();
    const auto endAfter = Clock::now();
    const auto lower = std::chrono::duration_cast<Ns>(endBefore - after).count();
    const auto upper = std::chrono::duration_cast<Ns>(endAfter - before).count();
    assert(end - start + 1 >= guestTimebaseFromNanoseconds(lower));
    assert(end - start <= guestTimebaseFromNanoseconds(upper) + 1);
    // Every native worker shares the epoch; joined reads cannot go backwards.
    std::array<std::thread, 4> workers;
    for (auto& worker : workers) {
        worker = std::thread([end] {
            auto prior = end;
            for (int i = 0; i < 1000; ++i) {
                const auto current = readGuestTimebase();
                assert(current >= prior);
                prior = current;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    assert(guestTimebaseReadCount() == 14002);
    std::cout << "GUEST_TIMEBASE_V1 frequency=" << GuestTimebaseFrequency
              << " reads=" << guestTimebaseReadCount() << " monotonic_and_rate_bracket=1\n";
}
