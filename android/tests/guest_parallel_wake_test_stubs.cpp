#include "guest_parallel_wake_test_stubs.h"

#include <mutex>
#include <utility>

namespace recomp::test {
namespace {
std::mutex& stubMutex() { static std::mutex mutex; return mutex; }
std::vector<WakeRecord>& records() { static std::vector<WakeRecord> list; return list; }
WakeObserver& observer() { static WakeObserver current; return current; }
void record(std::uint32_t token, bool all) noexcept {
    const WakeRecord entry{token, all, std::this_thread::get_id()};
    WakeObserver copy;
    {
        std::lock_guard lock(stubMutex());
        records().push_back(entry);
        copy = observer();
    }
    if (copy) copy(entry);
}
} // namespace

void setWakeObserver(WakeObserver next) {
    std::lock_guard lock(stubMutex());
    observer() = std::move(next);
}

std::vector<WakeRecord> takeWakeRecords() {
    std::lock_guard lock(stubMutex());
    auto taken = std::move(records());
    records().clear();
    return taken;
}

} // namespace recomp::test

namespace recomp::bootstrap {
__attribute__((weak)) void wakeGuestWaiters(std::uint32_t token) noexcept { recomp::test::record(token, false); }
__attribute__((weak)) void wakeAllGuestWaiters() noexcept { recomp::test::record(0, true); }
__attribute__((weak)) std::uint32_t currentGuestThreadToken() noexcept { return 0; }
__attribute__((weak)) void setGuestThreadDomainPolicy(GuestThreadDomainPolicy) {}
} // namespace recomp::bootstrap
