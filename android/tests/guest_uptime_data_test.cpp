#include "guest_execution_guard.h"
#include <cassert>
#include <thread>
#include <chrono>
using namespace recomp::bootstrap;
int main() {
    GuestExecutionGuard guard;
    std::optional<std::uint64_t> milliseconds = 0;
    unsigned samples = 0;
    auto source = [&] { ++samples; return milliseconds; };
    for (auto bad : {0u, 0x4001u, 0xFFFFFFECu, 0x7FC80000u}) assert(!bindGuestScalarUptimeClock(bad, source));
    assert(!bindGuestScalarUptimeClock(0x4000, {}));
    assert(bindGuestScalarUptimeClock(0x4000, source));
    assert(!bindGuestScalarUptimeClock(0x5000, source));
    auto read = [&] { return loadGuestScalar<std::uint32_t>(nullptr, 0x4010); };
    assert(read() == 0);
    milliseconds = 1234; assert(read() == 1234); assert(read() == 1234);
    milliseconds = 0xFFFFFFFFULL; assert(read() == 0xFFFFFFFF);
    milliseconds = 0x100000000ULL; assert(read() == 0);
    milliseconds = 0x100000001ULL; assert(read() == 1);
    milliseconds = 0x100000000ULL;
    assert(guard.run(1, [&] { (void)read(); }).scalarAccess->region == GuestScalarRegion::UnsupportedTimestampAccess);
    milliseconds.reset(); assert(guard.run(1, [&] { (void)read(); }).scalarAccess);
    const auto beforeInvalid = samples;
    for (auto address : {0x3FFDu, 0x4000u, 0x4004u, 0x400Fu, 0x4011u, 0x4014u, 0x4017u}) {
        const auto readResult = guard.run(1, [&] { (void)loadGuestScalar<std::uint32_t>(nullptr, address); });
        assert(readResult.scalarAccess && readResult.scalarAccess->region == GuestScalarRegion::UnsupportedTimestampAccess);
        const auto writeResult = guard.run(1, [&] { storeGuestScalar<std::uint32_t>(nullptr, address, 7); });
        assert(writeResult.scalarAccess && writeResult.scalarAccess->write && writeResult.scalarAccess->value == 7);
    }
    assert(samples == beforeInvalid);
    assert(guard.run(1, [&] { (void)loadGuestScalar<std::uint8_t>(nullptr, 0x4010); }).scalarAccess);
    assert(guard.run(1, [&] { (void)loadGuestScalar<std::uint16_t>(nullptr, 0x4010); }).scalarAccess);
    assert(guard.run(1, [&] { (void)loadGuestScalar<std::uint64_t>(nullptr, 0x4010); }).scalarAccess);
    assert(guard.run(1, [&] { storeGuestScalar<std::uint32_t>(nullptr, 0x4010, 1); }).scalarAccess);
    std::uint64_t value = 456;
    assert(!tryReadGuestScalarUptime(0x3FFC, 4, value));
    assert(!tryReadGuestScalarUptime(0x4018, 4, value));
    assert(value == 456);
    resetGuestScalarUptimeClock();
    assert(!tryReadGuestScalarUptime(0x4010, 4, value));
    milliseconds = 1; assert(bindGuestScalarUptimeClock(0x4000, source));
    // Bind-before-spawn, read on a worker, join-before-reset; no background RAM writer.
    std::thread worker([&] { assert(read() == 1); }); worker.join();
    resetGuestScalarUptimeClock();
    assert(guard.run(1, [&] { assert(!bindGuestScalarUptimeClock(0x4000, source)); }).completed());
    const auto start = std::chrono::steady_clock::now();
    assert(bindGuestScalarUptimeClock(0x4000, [start] {
        return std::optional<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
    }));
    const auto first = read();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(read() >= first + 5); // Advances without imports or writes to guest memory.
    resetGuestScalarUptimeClock();
}
