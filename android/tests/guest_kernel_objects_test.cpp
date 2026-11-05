#include "guest_import_registry.h"

#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace recomp::bootstrap;

namespace {
constexpr std::uint32_t Success = 0;
constexpr std::uint32_t InvalidHandle = 0xC0000008U;
constexpr std::uint32_t InvalidParameter = 0xC000000DU;
constexpr std::uint32_t TypeMismatch = 0xC0000024U;
constexpr std::uint32_t LimitExceeded = 0xC0000047U;
constexpr std::uint32_t Timeout = 0x102U;
constexpr std::uint32_t Output = 0x4000U;
constexpr std::uint32_t Previous = 0x4010U;
constexpr std::uint32_t TimeoutAddress = 0x4020U;

std::uint32_t read(const std::uint8_t* memory, std::uint32_t address) {
    return (std::uint32_t{memory[address]} << 24U)
            | (std::uint32_t{memory[address + 1]} << 16U)
            | (std::uint32_t{memory[address + 2]} << 8U) | memory[address + 3];
}
void write(std::uint8_t* memory, std::uint32_t address, std::uint32_t value) {
    for (std::uint32_t i = 0; i < 4; ++i) {
        memory[address + i] = static_cast<std::uint8_t>(value >> (24U - i * 8U));
    }
}
GuestImportCall arguments(std::initializer_list<std::uint32_t> values) {
    GuestImportCall call;
    std::size_t index = 0;
    for (auto value : values) call.arguments[index++] = value;
    return call;
}
} // namespace

int main() {
    GuestAddressSpace space;
    assert(space.reserve());
    auto* memory = space.base();
    GuestImportRegistry registry;
    assert(registry.initialize(space, 0x10000000U, 0x10000U));
    assert(registry.bindGuestThreadToken(0x2000U));
    const auto expect = [&](const char* name, std::initializer_list<std::uint32_t> values,
                            std::uint32_t status) {
        auto call = arguments(values);
        assert(registry.dispatch(name, call).handled());
        assert(call.returnValue == status);
    };
    const auto reject = [&](const char* name, std::initializer_list<std::uint32_t> values,
                            GuestImportDispatchState state) {
        auto call = arguments(values);
        assert(registry.dispatch(name, call).state == state);
    };
    const auto poll = [&](std::uint32_t handle, std::uint32_t status) {
        expect("__imp__NtWaitForSingleObjectEx", {handle, 1, 0, TimeoutAddress}, status);
    };
    write(memory, TimeoutAddress, 0);
    write(memory, TimeoutAddress + 4, 0);
    write(memory, Previous, 0xAABBCCDDU);
    reject("__imp__NtCreateSemaphore", {0, 0, 0, 1}, GuestImportDispatchState::GuestMemoryFault);
    reject("__imp__NtCreateSemaphore", {Output + 1, 0, 0, 1}, GuestImportDispatchState::GuestMemoryFault);
    write(memory, Output, 0x12345678U);
    reject("__imp__NtCreateSemaphore", {Output, 0x5000U, 0, 1}, GuestImportDispatchState::Rejected);
    assert(read(memory, Output) == 0x12345678U);
    for (const auto& values : {arguments({Output, 0, 0, 0}),
                              arguments({Output, 0, 2, 1}),
                              arguments({Output, 0, 0xFFFFFFFFU, 1}),
                              arguments({Output, 0, 0, 0x80000000U})}) {
        auto call = values;
        assert(registry.dispatch("__imp__NtCreateSemaphore", call).handled());
        assert(call.returnValue == InvalidParameter && read(memory, Output) == 0);
    }
    expect("__imp__NtCreateSemaphore", {Output, 0, 0, 3}, Success);
    const auto semaphore = read(memory, Output);
    assert(semaphore != 0 && (semaphore & 3U) == 0);
    poll(semaphore, Timeout);
    reject("__imp__NtWaitForSingleObjectEx", {semaphore, 1, 0, 0}, GuestImportDispatchState::Rejected);
    write(memory, TimeoutAddress, 0xFFFFFFFFU);
    write(memory, TimeoutAddress + 4, 0xFFFFD8F0U);
    reject("__imp__NtWaitForSingleObjectEx", {semaphore, 1, 0, TimeoutAddress}, GuestImportDispatchState::Rejected);
    write(memory, TimeoutAddress, 0);
    write(memory, TimeoutAddress + 4, 0);
    expect("__imp__NtReleaseSemaphore", {semaphore, 2, Previous}, Success);
    assert(read(memory, Previous) == 0);
    write(memory, Previous, 0xAABBCCDDU);
    expect("__imp__NtReleaseSemaphore", {semaphore, 2, Previous}, LimitExceeded);
    expect("__imp__NtReleaseSemaphore", {semaphore, 0, Previous}, InvalidParameter);
    expect("__imp__NtReleaseSemaphore", {semaphore, 0xFFFFFFFFU, Previous}, InvalidParameter);
    assert(read(memory, Previous) == 0xAABBCCDDU);
    reject("__imp__NtReleaseSemaphore", {semaphore, 1, Previous + 1}, GuestImportDispatchState::GuestMemoryFault);
    reject("__imp__NtWaitForSingleObjectEx", {semaphore, 1, 0, 0xFFFFFFFCU}, GuestImportDispatchState::GuestMemoryFault);
    reject("__imp__NtWaitForSingleObjectEx", {semaphore, 1, 1, TimeoutAddress}, GuestImportDispatchState::Rejected);
    expect("__imp__NtWaitForSingleObjectEx", {semaphore, 2, 0, TimeoutAddress}, InvalidParameter);
    registry.unbindGuestThreadToken();
    reject("__imp__NtWaitForSingleObjectEx", {semaphore, 1, 0, TimeoutAddress}, GuestImportDispatchState::Rejected);
    assert(registry.bindGuestThreadToken(0x3000U));
    poll(semaphore, Success);
    poll(semaphore, Success);
    poll(semaphore, Timeout); // Rejected operations did not consume or add tokens.
    expect("__imp__NtReleaseSemaphore", {semaphore, 1, Previous}, Success);
    assert(read(memory, Previous) == 0);
    expect("__imp__NtReleaseSemaphore", {semaphore, 1, Previous}, Success);
    assert(read(memory, Previous) == 1);

    expect("__imp__NtCreateEvent", {Output, 0, 0, 1}, Success);
    const auto manual = read(memory, Output);
    poll(manual, Success);
    poll(manual, Success); // Notification events stay signaled after waiting.
    expect("__imp__NtSetEvent", {manual, Previous}, Success);
    assert(read(memory, Previous) == 1);
    expect("__imp__NtClearEvent", {manual}, Success);
    poll(manual, Timeout);
    expect("__imp__NtSetEvent", {manual, Previous}, Success);
    assert(read(memory, Previous) == 0);
    poll(manual, Success);

    expect("__imp__NtCreateEvent", {Output, 0, 1, 0}, Success);
    const auto automatic = read(memory, Output);
    poll(automatic, Timeout);
    reject("__imp__NtSetEvent", {automatic, Previous + 1}, GuestImportDispatchState::GuestMemoryFault);
    poll(automatic, Timeout);
    expect("__imp__NtSetEvent", {automatic, Previous}, Success);
    assert(read(memory, Previous) == 0);
    expect("__imp__NtSetEvent", {automatic, Previous}, Success);
    assert(read(memory, Previous) == 1);
    poll(automatic, Success);
    poll(automatic, Timeout); // Synchronization events release one waiter only.
    expect("__imp__NtCreateEvent", {Output, 0, 2, 0}, InvalidParameter);
    reject("__imp__NtCreateEvent", {Output, 0x5000U, 0, 0}, GuestImportDispatchState::Rejected);
    expect("__imp__NtReleaseSemaphore", {manual, 1, Previous}, TypeMismatch);
    expect("__imp__NtSetEvent", {semaphore, Previous}, TypeMismatch);
    expect("__imp__NtClearEvent", {semaphore}, TypeMismatch);
    for (auto handle : {0U, 0xFFFFFFFFU, semaphore + 1U}) {
        expect("__imp__NtClose", {handle}, InvalidHandle);
        expect("__imp__NtSetEvent", {handle, Previous}, InvalidHandle);
        expect("__imp__NtReleaseSemaphore", {handle, 1, Previous}, InvalidHandle);
        poll(handle, InvalidHandle);
    }
    expect("__imp__NtClose", {semaphore}, Success);
    expect("__imp__NtClose", {semaphore}, InvalidHandle);
    expect("__imp__NtReleaseSemaphore", {semaphore, 1, Previous}, InvalidHandle);
    poll(semaphore, InvalidHandle);
    expect("__imp__NtCreateSemaphore", {Output, 0, 1, 1}, Success);
    assert(read(memory, Output) != semaphore); // A stale handle cannot alias a new object.
    const auto beforeReset = read(memory, Output);
    registry.reset();
    assert(registry.initialize(space, 0x10000000U, 0x10000U));
    assert(registry.bindGuestThreadToken(0x2000U));
    expect("__imp__NtCreateSemaphore", {0xFFFFFFFCU, 0, 1, 1}, Success);
    const auto afterReset = read(memory, 0xFFFFFFFCU);
    assert(afterReset != beforeReset);
    poll(beforeReset, InvalidHandle);
    poll(afterReset, Success);
    expect("__imp__NtClose", {afterReset}, Success);

    std::vector<std::uint32_t> handles;
    for (std::size_t i = 0; i < GuestImportRegistry::MaximumKernelHandles; ++i) {
        expect("__imp__NtCreateSemaphore", {Output, 0, 0, 32767}, Success);
        handles.push_back(read(memory, Output));
    }
    expect("__imp__NtCreateEvent", {Output, 0, 0, 0}, 0xC0000017U);
    assert(read(memory, Output) == 0);
    expect("__imp__NtClose", {handles.back()}, Success);
    expect("__imp__NtCreateEvent", {Output, 0, 0, 0}, Success);
    assert(read(memory, Output) != handles.back());
    registry.reset();
}
