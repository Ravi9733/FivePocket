#include "guest_execution_guard.h"
#include <cassert>
#include <chrono>
#include <thread>

using namespace recomp::bootstrap;
int main() {
    GuestExecutionGuard guard;
    constexpr std::uint32_t Address = 0x7FC8112C;
    auto read = [&] { return loadGuestScalar<std::uint32_t>(nullptr, Address); };
    auto rejected = [&](auto operation) {
        const auto r = guard.run(1, operation);
        assert(r.scalarAccess && r.scalarAccess->region == GuestScalarRegion::GpuRegisters);
    };
    rejected([&] { (void)read(); });
    assert(!bindExperimentalGuestGpuClock({}));
    std::optional<std::uint64_t> source = 0;
    assert(bindExperimentalGuestGpuClock([&] { return source; }));
    assert(!bindExperimentalGuestGpuClock([&] { return source; }));
    assert(read() == 0); source = 3; assert(read() == 1); assert(read() == 1);
    source = 2; rejected([&] { (void)read(); });
    source.reset(); rejected([&] { (void)read(); });
    assert(experimentalGuestGpuClockReads() == 3);
    source = 0x1FFFFFFFEULL; assert(read() == UINT32_MAX);
    source = 0x200000000ULL; assert(read() == 0);
    source = UINT64_MAX; assert(read() == UINT32_MAX);
    rejected([&] { (void)loadGuestScalar<std::uint8_t>(nullptr, Address); });
    rejected([&] { (void)loadGuestScalar<std::uint16_t>(nullptr, Address); });
    rejected([&] { (void)loadGuestScalar<std::uint64_t>(nullptr, Address); });
    for (auto a : {Address - 1, Address + 1, Address + 4, 0x7FC80714U}) {
        rejected([&] { (void)loadGuestScalar<std::uint32_t>(nullptr, a); });
    }
    rejected([&] { storeGuestScalar<std::uint32_t>(nullptr, Address, 0x1234); });
    assert(experimentalGuestGpuClockReads() == 6);
    resetExperimentalGuestGpuClock(); assert(experimentalGuestGpuClockReads() == 0);
    rejected([&] { (void)read(); });
    const auto r = guard.run(1, [&] { assert(!bindExperimentalGuestGpuClock([&] { return source; })); });
    assert(r.reason == GuestExecutionStopReason::Completed);
    const auto start = std::chrono::steady_clock::now();
    assert(bindExperimentalGuestGpuClock([&]() -> std::optional<std::uint64_t> {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    }));
    const auto before = read(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto after = read(); assert(after > before); // No imports or GPU completion events.
    resetExperimentalGuestGpuClock(); rejected([&] { (void)read(); });
}
