#include "guest_address_space.h"
#include "guest_import_registry.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

using recomp::bootstrap::GuestAddressSpace;
using recomp::bootstrap::GuestImportCall;
using recomp::bootstrap::GuestImportCategory;
using recomp::bootstrap::GuestImportDispatchState;
using recomp::bootstrap::GuestImportRegistry;
using recomp::bootstrap::guestImportCategoryName;
using recomp::bootstrap::guestImportDispatchStateName;

namespace {

void writeU16(std::uint8_t* memory, std::uint32_t address, std::uint16_t value) {
    memory[address] = static_cast<std::uint8_t>(value >> 8U);
    memory[address + 1] = static_cast<std::uint8_t>(value);
}

void writeU32(std::uint8_t* memory, std::uint32_t address, std::uint32_t value) {
    memory[address] = static_cast<std::uint8_t>(value >> 24U);
    memory[address + 1] = static_cast<std::uint8_t>(value >> 16U);
    memory[address + 2] = static_cast<std::uint8_t>(value >> 8U);
    memory[address + 3] = static_cast<std::uint8_t>(value);
}

std::uint16_t readU16(const std::uint8_t* memory, std::uint32_t address) {
    return static_cast<std::uint16_t>(memory[address] << 8U) | memory[address + 1];
}

std::uint32_t readU32(const std::uint8_t* memory, std::uint32_t address) {
    return (static_cast<std::uint32_t>(memory[address]) << 24U)
            | (static_cast<std::uint32_t>(memory[address + 1]) << 16U)
            | (static_cast<std::uint32_t>(memory[address + 2]) << 8U)
            | static_cast<std::uint32_t>(memory[address + 3]);
}

void writeAnsi(std::uint8_t* memory, std::uint32_t address, std::string_view value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        memory[address + index] = static_cast<std::uint8_t>(value[index]);
    }
    memory[address + value.size()] = 0;
}

GuestImportCall callWith(std::initializer_list<std::uint32_t> arguments) {
    GuestImportCall call;
    std::size_t index = 0;
    for (const auto argument : arguments) {
        call.arguments[index++] = argument;
    }
    return call;
}

void testGuestVisibleTlsStorage() {
    GuestAddressSpace space;
    assert(space.reserve());
    auto* memory = space.base();
    GuestImportRegistry registry;
    constexpr std::uint32_t Heap = 0x01000000U;
    constexpr std::uint32_t Physical = 0x02000000U;
    constexpr std::uint32_t First = 0x1000U;
    constexpr std::uint32_t Second = 0x2000U;
    constexpr std::uint32_t StaticBase = 0x00100000U;
    constexpr std::uint32_t StaticLength = 0x158U;
    constexpr std::uint32_t Slots = StaticBase + StaticLength;
    constexpr std::uint32_t OtherSlots = 0x00200158U;
    constexpr std::uint32_t SlotCount = 64;
    constexpr std::uint32_t OutOfIndexes = 0xFFFFFFFFU;
    const auto expect = [&registry](std::string_view name,
            std::initializer_list<std::uint32_t> arguments,
            std::uint32_t expected) {
        auto call = callWith(arguments);
        assert(registry.dispatch(name, call).handled());
        assert(call.returnValue == expected);
    };
    assert(!registry.bindGuestThreadTlsStorage(First, Slots, SlotCount));
    assert(registry.initialize(space, Heap, 0x10000U));
    assert(!registry.bindGuestThreadTlsStorage(First, Slots, SlotCount));
    assert(registry.bindGuestThreadToken(First));
    // Token-only users retain the full 2048 process slots. A new backing may
    // not hide any live process allocation behind a smaller capacity.
    for (std::uint32_t index = 0; index < 2048; ++index) {
        expect("__imp__KeTlsAlloc", {}, index);
    }
    expect("__imp__KeTlsAlloc", {}, OutOfIndexes);
    assert(!registry.bindGuestThreadTlsStorage(First, Slots, SlotCount));
    for (std::uint32_t index = 1; index < 2048; ++index) {
        expect("__imp__KeTlsFree", {index}, 1);
    }
    expect("__imp__KeTlsSetValue", {0, 0x12345678U}, 1);

    std::fill_n(memory + StaticBase, StaticLength + SlotCount * 4U + 1U, 0xA5U);
    assert(!registry.bindGuestThreadTlsStorage(0, Slots, SlotCount));
    assert(!registry.bindGuestThreadTlsStorage(First, 0, SlotCount));
    assert(!registry.bindGuestThreadTlsStorage(First, Slots + 1U, SlotCount));
    assert(!registry.bindGuestThreadTlsStorage(First, Slots, 0));
    assert(!registry.bindGuestThreadTlsStorage(First, Slots, 2049));
    assert(!registry.bindGuestThreadTlsStorage(First, 0xFFFFFFFCU, 2));
    assert(!registry.bindGuestThreadTlsStorage(First, Heap - 4U, 2));
    assert(registry.bindPhysicalMemoryArena(Physical, 0x10000U, 0));
    assert(!registry.bindGuestThreadTlsStorage(First, Physical - 4U, 2));
    assert(registry.bindGuestThreadTlsStorage(First, Slots, SlotCount));
    assert(readU32(memory, Slots) == 0x12345678U);
    assert(memory[Slots] == 0x12U && memory[Slots + 3U] == 0x78U);
    assert(readU32(memory, Slots + 4U) == 0);
    const auto staticUntouched = [&] {
        assert(std::all_of(memory + StaticBase, memory + Slots,
                [](std::uint8_t value) { return value == 0xA5U; }));
        assert(memory[Slots + SlotCount * 4U] == 0xA5U);
    };
    staticUntouched();
    assert(!registry.bindGuestThreadTlsStorage(First, OtherSlots, SlotCount));
    assert(!registry.bindGuestThreadTlsStorage(First, Slots, SlotCount + 1U));
    writeU32(memory, Slots, 0x89ABCDEFU);
    assert(registry.bindGuestThreadTlsStorage(First, Slots, SlotCount));
    expect("__imp__KeTlsGetValue", {0}, 0x89ABCDEFU);
    // Binding the same storage twice must not replace direct guest writes
    // with stale values from the earlier token-only representation.
    assert(readU32(memory, Slots) == 0x89ABCDEFU);
    auto overlappingLock = callWith({Slots - 4U});
    assert(registry.dispatch("__imp__RtlInitializeCriticalSection", overlappingLock).state
            == GuestImportDispatchState::Rejected);
    assert(!registry.bindPhysicalMemoryArena(StaticBase, 0x1000U, 0));

    assert(registry.bindGuestThreadToken(Second));
    assert(!registry.bindGuestThreadTlsStorage(Second, Slots + 4U, SlotCount));
    assert(!registry.bindGuestThreadTlsStorage(Second, Slots - 4U, SlotCount));
    assert(registry.bindGuestThreadTlsStorage(Second, OtherSlots, SlotCount * 2U));
    expect("__imp__KeTlsGetValue", {0}, 0);
    expect("__imp__KeTlsSetValue", {0, 0x76543210U}, 1);
    assert(readU32(memory, OtherSlots) == 0x76543210U);
    assert(readU32(memory, Slots) == 0x89ABCDEFU);
    writeU32(memory, Slots + 4U, 0xFFFFFFFFU);
    writeU32(memory, OtherSlots + 4U, 0xFFFFFFFFU);
    // Allocation clears the newly allocated slot in every bound thread, even
    // if unallocated storage had previously been written directly by a guest.
    expect("__imp__KeTlsAlloc", {}, 1);
    assert(readU32(memory, Slots + 4U) == 0);
    assert(readU32(memory, OtherSlots + 4U) == 0);
    for (std::uint32_t index = 2; index < SlotCount; ++index) {
        expect("__imp__KeTlsAlloc", {}, index);
    }
    expect("__imp__KeTlsAlloc", {}, OutOfIndexes);
    expect("__imp__KeTlsGetValue", {SlotCount}, 0);
    expect("__imp__KeTlsSetValue", {SlotCount, 0xFFFFFFFFU}, 0);
    expect("__imp__KeTlsSetValue", {SlotCount - 1U, 0x01020304U}, 1);
    assert(readU32(memory, OtherSlots + (SlotCount - 1U) * 4U) == 0x01020304U);
    expect("__imp__KeTlsFree", {0}, 1);
    assert(readU32(memory, Slots) == 0);
    assert(readU32(memory, OtherSlots) == 0);
    expect("__imp__KeTlsAlloc", {}, 0);

    assert(registry.bindGuestThreadToken(First));
    writeU32(memory, Slots, 0xABCDEF01U);
    writeU32(memory, Slots + (SlotCount - 1U) * 4U, 0xFEDCBA98U);
    registry.unbindGuestThreadToken();
    assert(readU32(memory, Slots) == 0xABCDEF01U);
    registry.unbindGuestThreadTlsStorage(First);
    assert(std::all_of(memory + Slots, memory + Slots + SlotCount * 4U,
            [](std::uint8_t value) { return value == 0; }));
    assert(registry.bindGuestThreadToken(First));
    expect("__imp__KeTlsGetValue", {0}, 0xABCDEF01U);
    expect("__imp__KeTlsGetValue", {SlotCount - 1U}, 0xFEDCBA98U);
    assert(registry.bindGuestThreadTlsStorage(First, Slots, SlotCount));
    assert(readU32(memory, Slots) == 0xABCDEF01U);
    assert(readU32(memory, Slots + (SlotCount - 1U) * 4U) == 0xFEDCBA98U);
    staticUntouched();
    // Teardown cannot discard an owning thread while a registered lock is
    // held. Once released, it scrubs storage and clears the active identity.
    auto lock = callWith({0x3000U});
    assert(registry.dispatch("__imp__RtlInitializeCriticalSection", lock).handled());
    assert(registry.dispatch("__imp__RtlEnterCriticalSection", lock).handled());
    assert(!registry.teardownGuestThread(First));
    assert(readU32(memory, Slots) == 0xABCDEF01U);
    assert(registry.dispatch("__imp__RtlLeaveCriticalSection", lock).handled());
    assert(registry.teardownGuestThread(First));
    assert(!registry.teardownGuestThread(First));
    assert(std::all_of(memory + Slots, memory + Slots + SlotCount * 4U,
            [](std::uint8_t value) { return value == 0; }));
    auto unbound = callWith({0});
    assert(registry.dispatch("__imp__KeTlsGetValue", unbound).state
            == GuestImportDispatchState::Rejected);
    assert(registry.bindGuestThreadToken(First));
    expect("__imp__KeTlsGetValue", {0}, 0);
    // Remaining 128-slot backing controls allocation, including for a token-
    // only thread. Rebinding a 64-slot range requires first freeing slot 64.
    expect("__imp__KeTlsAlloc", {}, SlotCount);
    assert(!registry.bindGuestThreadTlsStorage(First, Slots, SlotCount));
    expect("__imp__KeTlsFree", {SlotCount}, 1);
    assert(!registry.bindGuestThreadTlsStorage(First, 0x3000U, SlotCount));
    registry.unbindGuestThreadTlsStorage(Second);
    for (std::uint32_t index = 0; index < SlotCount; ++index) {
        expect("__imp__KeTlsFree", {index}, 1);
    }
    // The last aligned word in the 32-bit address space is valid; one more
    // slot would wrap. No allocation or scrub may touch beyond that word.
    assert(registry.bindGuestThreadTlsStorage(First, 0xFFFFFFFCU, 1));
    expect("__imp__KeTlsAlloc", {}, 0);
    expect("__imp__KeTlsSetValue", {0, 0xF00DBAAAU}, 1);
    assert(readU32(memory, 0xFFFFFFFCU) == 0xF00DBAAAU);
    registry.unbindGuestThreadTlsStorage(First);
    assert(readU32(memory, 0xFFFFFFFCU) == 0);
    assert(registry.bindGuestThreadTlsStorage(First, Slots, SlotCount));
    assert(readU32(memory, Slots) == 0xF00DBAAAU);
    registry.reset();
    assert(readU32(memory, Slots) == 0);
    staticUntouched();
    assert(registry.initialize(space, Heap, 0x10000U));
    assert(registry.bindGuestThreadToken(First));
    assert(registry.bindGuestThreadTlsStorage(First, Slots, SlotCount));
    expect("__imp__KeTlsAlloc", {}, 0);
    expect("__imp__KeTlsSetValue", {0, 0x12345678U}, 1);
    assert(!registry.initialize(space, Heap + 1U, 0x10000U));
    assert(readU32(memory, Slots) == 0);
    staticUntouched();
}

} // namespace

int main() {
    const auto& imports = GuestImportRegistry::implementedBootImports();
    assert(imports.size() == 131);
    assert(imports.front().name == "__imp__DbgPrint");
    assert(imports.front().minimumDirectCallDistanceFromEntry == 1);
    assert(imports.front().category == GuestImportCategory::Logging);
    assert(imports.front().writesReturnValue);
    assert(std::string(guestImportCategoryName(imports.front().category)) == "logging");
    assert(GuestImportRegistry::isImplemented("__imp__NtAllocateVirtualMemory"));
    assert(GuestImportRegistry::isImplemented("__imp__RtlInitAnsiString"));
    assert(GuestImportRegistry::isImplemented("__imp__KeGetCurrentProcessType"));
    assert(GuestImportRegistry::isImplemented("__imp__KeTlsAlloc"));
    assert(GuestImportRegistry::isImplemented("__imp__KeTlsFree"));
    assert(GuestImportRegistry::isImplemented("__imp__KeTlsGetValue"));
    assert(GuestImportRegistry::isImplemented("__imp__KeTlsSetValue"));
    // The convenience dispatcher is implemented for direct callers, but GTA V
    // does not import it, so it is intentionally absent from bridge descriptors.
    assert(!GuestImportRegistry::isImplemented("__imp__MmAllocatePhysicalMemory"));
    assert(GuestImportRegistry::isImplemented("__imp__MmAllocatePhysicalMemoryEx"));
    assert(GuestImportRegistry::isImplemented("__imp__MmFreePhysicalMemory"));
    assert(GuestImportRegistry::isImplemented("__imp__MmGetPhysicalAddress"));
    assert(GuestImportRegistry::isImplemented("__imp__MmQueryAddressProtect"));
    assert(GuestImportRegistry::isImplemented("__imp__MmQueryAllocationSize"));
    assert(GuestImportRegistry::isImplemented("__imp__MmQueryStatistics"));
    assert(GuestImportRegistry::isImplemented("__imp__RtlImageXexHeaderField"));
    assert(GuestImportRegistry::isImplemented("__imp__RtlInitializeCriticalSection"));
    assert(GuestImportRegistry::isImplemented(
            "__imp__RtlInitializeCriticalSectionAndSpinCount"));
    assert(GuestImportRegistry::isImplemented("__imp__RtlEnterCriticalSection"));
    assert(GuestImportRegistry::isImplemented("__imp__RtlLeaveCriticalSection"));
    assert(GuestImportRegistry::isImplemented("__imp__XexCheckExecutablePrivilege"));
    assert(GuestImportRegistry::isImplemented("__imp__VdSwap"));
    const auto swap = std::find_if(imports.begin(),imports.end(),[](const auto& entry){return entry.name=="__imp__VdSwap";});
    assert(swap!=imports.end() && !swap->writesReturnValue);
    assert(!GuestImportRegistry::isImplemented("__imp__NetDll_socket"));
    assert(GuestImportRegistry::isImplemented("__imp__ExCreateThread"));
    const auto dereference = std::find_if(imports.begin(), imports.end(), [](const auto& entry) {
        return entry.name == "__imp__ObDereferenceObject";
    });
    assert(dereference != imports.end() && !dereference->writesReturnValue);
    assert(GuestImportRegistry::isImplemented("__imp__KeQuerySystemTime"));
    const auto processImport = std::find_if(
            imports.begin(),
            imports.end(),
            [](const auto& descriptor) {
                return descriptor.name == "__imp__KeGetCurrentProcessType";
            });
    assert(processImport != imports.end());
    assert(processImport->minimumDirectCallDistanceFromEntry == 4);
    assert(processImport->category == GuestImportCategory::Process);
    assert(processImport->writesReturnValue);
    assert(std::string(guestImportCategoryName(processImport->category)) == "process");
    const auto titleImport = std::find_if(
            imports.begin(),
            imports.end(),
            [](const auto& descriptor) {
                return descriptor.name == "__imp__RtlImageXexHeaderField";
            });
    assert(titleImport != imports.end());
    assert(titleImport->minimumDirectCallDistanceFromEntry == 3);
    assert(titleImport->category == GuestImportCategory::Title);
    assert(titleImport->writesReturnValue);
    assert(std::string(guestImportCategoryName(titleImport->category)) == "title");
    const auto privilegeImport = std::find_if(
            imports.begin(),
            imports.end(),
            [](const auto& descriptor) {
                return descriptor.name == "__imp__XexCheckExecutablePrivilege";
            });
    assert(privilegeImport != imports.end());
    assert(privilegeImport->minimumDirectCallDistanceFromEntry == 2);
    assert(privilegeImport->category == GuestImportCategory::Title);
    assert(privilegeImport->writesReturnValue);
    const auto synchronizationImport = std::find_if(
            imports.begin(),
            imports.end(),
            [](const auto& descriptor) {
                return descriptor.name == "__imp__RtlInitializeCriticalSection";
            });
    assert(synchronizationImport != imports.end());
    assert(synchronizationImport->minimumDirectCallDistanceFromEntry == 4);
    assert(synchronizationImport->category == GuestImportCategory::Synchronization);
    assert(!synchronizationImport->writesReturnValue);
    assert(std::string(guestImportCategoryName(synchronizationImport->category))
            == "synchronization");
    const auto enterCriticalSectionImport = std::find_if(
            imports.begin(),
            imports.end(),
            [](const auto& descriptor) {
                return descriptor.name == "__imp__RtlEnterCriticalSection";
            });
    assert(enterCriticalSectionImport != imports.end());
    assert(enterCriticalSectionImport->minimumDirectCallDistanceFromEntry == 4);
    assert(enterCriticalSectionImport->category == GuestImportCategory::Synchronization);
    assert(!enterCriticalSectionImport->writesReturnValue);
    const auto leaveCriticalSectionImport = std::find_if(
            imports.begin(),
            imports.end(),
            [](const auto& descriptor) {
                return descriptor.name == "__imp__RtlLeaveCriticalSection";
            });
    assert(leaveCriticalSectionImport != imports.end());
    assert(leaveCriticalSectionImport->minimumDirectCallDistanceFromEntry == 4);
    assert(leaveCriticalSectionImport->category == GuestImportCategory::Synchronization);
    assert(!leaveCriticalSectionImport->writesReturnValue);
    const auto ansiImport = std::find_if(
            imports.begin(),
            imports.end(),
            [](const auto& descriptor) {
                return descriptor.name == "__imp__RtlInitAnsiString";
            });
    assert(ansiImport != imports.end());
    assert(!ansiImport->writesReturnValue);
    const auto physicalAllocationImport = std::find_if(
            imports.begin(),
            imports.end(),
            [](const auto& descriptor) {
                return descriptor.name == "__imp__MmAllocatePhysicalMemoryEx";
            });
    assert(physicalAllocationImport != imports.end());
    assert(physicalAllocationImport->minimumDirectCallDistanceFromEntry
            == std::numeric_limits<std::uint32_t>::max());
    assert(physicalAllocationImport->category == GuestImportCategory::Memory);
    assert(physicalAllocationImport->writesReturnValue);
    const auto physicalFreeImport = std::find_if(
            imports.begin(),
            imports.end(),
            [](const auto& descriptor) {
                return descriptor.name == "__imp__MmFreePhysicalMemory";
            });
    assert(physicalFreeImport != imports.end());
    assert(!physicalFreeImport->writesReturnValue);

    GuestImportRegistry registry;
    assert(!registry.bindGuestThreadToken(0x1000U));
    assert(!registry.bindExecutableXexHeader(0x4000U));
    assert(!registry.bindPhysicalMemoryArena(0x02000000U, 0x04000000U, 0x10000000U));
    GuestImportCall unavailableCall;
    auto result = registry.dispatch("__imp__DbgPrint", unavailableCall);
    assert(result.state == GuestImportDispatchState::RegistryUnavailable);

    GuestAddressSpace addressSpace;
    assert(addressSpace.reserve());
    constexpr std::uint32_t HeapBase = 0x01000000U;
    constexpr std::uint32_t HeapSize = 0x00040000U;
    assert(!registry.initialize(addressSpace, HeapBase + 1, HeapSize));

    std::string loggedMessage;
    assert(registry.initialize(
            addressSpace,
            HeapBase,
            HeapSize,
            [&loggedMessage](std::string_view message) {
                loggedMessage.assign(message);
            }));
    assert(registry.isReady());

    auto unboundPhysicalAllocateCall = callWith({0, 1, 0x04U});
    result = registry.dispatch(
            "__imp__MmAllocatePhysicalMemory",
            unboundPhysicalAllocateCall);
    assert(result.state == GuestImportDispatchState::Rejected);
    assert(unboundPhysicalAllocateCall.returnValue == 0);

    constexpr std::uint32_t PhysicalArenaBase = 0x02000000U;
    constexpr std::uint32_t PhysicalArenaSize = 0x04000000U;
    constexpr std::uint32_t PhysicalAddressOffset = 0x10000000U;
    assert(!registry.bindPhysicalMemoryArena(HeapBase, HeapSize, PhysicalAddressOffset));
    assert(!registry.bindPhysicalMemoryArena(
            PhysicalArenaBase + 1U,
            PhysicalArenaSize,
            PhysicalAddressOffset));
    assert(!registry.bindPhysicalMemoryArena(
            PhysicalArenaBase,
            PhysicalArenaSize - 1U,
            PhysicalAddressOffset));
    assert(!registry.bindPhysicalMemoryArena(
            PhysicalArenaBase,
            PhysicalArenaSize,
            PhysicalAddressOffset + 1U));
    assert(!registry.bindPhysicalMemoryArena(0xFFFFF000U, 0x2000U, 0));
    assert(!registry.bindPhysicalMemoryArena(
            PhysicalArenaBase,
            PhysicalArenaSize,
            0xFE000000U));
    assert(registry.bindPhysicalMemoryArena(
            PhysicalArenaBase,
            PhysicalArenaSize,
            PhysicalAddressOffset));

    auto invalidPhysicalTypeCall = callWith({3, 1, 0x04U, 0, 0xFFFFFFFFU, 0});
    result = registry.dispatch(
            "__imp__MmAllocatePhysicalMemoryEx",
            invalidPhysicalTypeCall);
    assert(result.state == GuestImportDispatchState::Rejected);
    auto invalidPhysicalProtectionCall = callWith({0, 1, 0x01U, 0, 0xFFFFFFFFU, 0});
    result = registry.dispatch(
            "__imp__MmAllocatePhysicalMemoryEx",
            invalidPhysicalProtectionCall);
    assert(result.state == GuestImportDispatchState::Rejected);
    auto conflictingProtectionCall = callWith({
            0, 1, 0x06U, 0, 0xFFFFFFFFU, 0,
    });
    result = registry.dispatch(
            "__imp__MmAllocatePhysicalMemoryEx",
            conflictingProtectionCall);
    assert(result.state == GuestImportDispatchState::Rejected);
    auto conflictingPageSizeCall = callWith({
            0, 1, 0xA0000004U, 0, 0xFFFFFFFFU, 0,
    });
    result = registry.dispatch(
            "__imp__MmAllocatePhysicalMemoryEx",
            conflictingPageSizeCall);
    assert(result.state == GuestImportDispatchState::Rejected);

    auto physicalAllocateCall = callWith({1, 1, 0x04U});
    result = registry.dispatch(
            "__imp__MmAllocatePhysicalMemory",
            physicalAllocateCall);
    assert(result.handled());
    assert(physicalAllocateCall.returnValue == 0x05FFF000U);
    const auto physicalAllocation = static_cast<std::uint32_t>(
            physicalAllocateCall.returnValue);

    auto getPhysicalAddressCall = callWith({physicalAllocation + 0x123U});
    result = registry.dispatch("__imp__MmGetPhysicalAddress", getPhysicalAddressCall);
    assert(result.handled());
    assert(getPhysicalAddressCall.returnValue == 0x13FFF123U);
    auto unownedPhysicalAddressCall = callWith({PhysicalArenaBase});
    result = registry.dispatch(
            "__imp__MmGetPhysicalAddress",
            unownedPhysicalAddressCall);
    assert(result.state == GuestImportDispatchState::Rejected);

    auto wrongTypePhysicalFreeCall = callWith({0, physicalAllocation});
    result = registry.dispatch(
            "__imp__MmFreePhysicalMemory",
            wrongTypePhysicalFreeCall);
    assert(result.state == GuestImportDispatchState::Rejected);
    auto interiorPhysicalFreeCall = callWith({1, physicalAllocation + 0x20U});
    result = registry.dispatch(
            "__imp__MmFreePhysicalMemory",
            interiorPhysicalFreeCall);
    assert(result.state == GuestImportDispatchState::Rejected);

    auto* memory = addressSpace.base();
    std::fill(memory + physicalAllocation, memory + physicalAllocation + 0x1000U, 0xA5);
    auto physicalFreeCall = callWith({1, physicalAllocation});
    physicalFreeCall.returnValue = 0x12345678U;
    result = registry.dispatch("__imp__MmFreePhysicalMemory", physicalFreeCall);
    assert(result.handled());
    assert(physicalFreeCall.returnValue == 0x12345678U);
    assert(std::all_of(
            memory + physicalAllocation,
            memory + physicalAllocation + 0x1000U,
            [](std::uint8_t value) { return value == 0; }));
    result = registry.dispatch("__imp__MmFreePhysicalMemory", physicalFreeCall);
    assert(result.state == GuestImportDispatchState::Rejected);

    physicalAllocateCall = callWith({1, 1, 0x04U});
    result = registry.dispatch(
            "__imp__MmAllocatePhysicalMemory",
            physicalAllocateCall);
    assert(result.handled());
    assert(physicalAllocateCall.returnValue == physicalAllocation);

    auto rangedPhysicalCall = callWith({
            2,
            0x1001U,
            0x202U,
            PhysicalAddressOffset + 0x10000U,
            PhysicalAddressOffset + 0x1FFFFU,
            0,
    });
    result = registry.dispatch("__imp__MmAllocatePhysicalMemoryEx", rangedPhysicalCall);
    assert(result.handled());
    assert(rangedPhysicalCall.returnValue == PhysicalArenaBase + 0x1E000U);

    auto alignedPhysicalCall = callWith({
            0, 0x1000U, 0x04U, 0, 0xFFFFFFFFU, 0x1001U,
    });
    result = registry.dispatch("__imp__MmAllocatePhysicalMemoryEx", alignedPhysicalCall);
    assert(result.handled());
    assert(alignedPhysicalCall.returnValue != 0);
    assert((alignedPhysicalCall.returnValue % 0x2000U) == 0);

    auto largePhysicalCall = callWith({
            0, 1, 0x20000404U, 0, 0xFFFFFFFFU, 0,
    });
    result = registry.dispatch("__imp__MmAllocatePhysicalMemoryEx", largePhysicalCall);
    assert(result.handled());
    assert(largePhysicalCall.returnValue != 0);
    assert((largePhysicalCall.returnValue % 0x10000U) == 0);

    auto sixteenMbPhysicalCall = callWith({
            0, 1, 0x80000004U, 0, 0xFFFFFFFFU, 0,
    });
    result = registry.dispatch(
            "__imp__MmAllocatePhysicalMemoryEx",
            sixteenMbPhysicalCall);
    assert(result.handled());
    assert(sixteenMbPhysicalCall.returnValue == 0x04000000U);

    auto invalidPhysicalRangeCall = callWith({
            0, 1, 0x04U, PhysicalAddressOffset + 0x2000U,
            PhysicalAddressOffset + 0x1000U, 0,
    });
    result = registry.dispatch(
            "__imp__MmAllocatePhysicalMemoryEx",
            invalidPhysicalRangeCall);
    assert(result.handled());
    assert(invalidPhysicalRangeCall.returnValue == 0);
    auto overflowingPhysicalSizeCall = callWith({
            0, 0xFFFFFFFFU, 0x04U, 0, 0xFFFFFFFFU, 0,
    });
    result = registry.dispatch(
            "__imp__MmAllocatePhysicalMemoryEx",
            overflowingPhysicalSizeCall);
    assert(result.handled());
    assert(overflowingPhysicalSizeCall.returnValue == 0);
    auto overflowingPhysicalAlignmentCall = callWith({
            0, 1, 0x04U, 0, 0xFFFFFFFFU, 0xFFFFFFFFU,
    });
    result = registry.dispatch(
            "__imp__MmAllocatePhysicalMemoryEx",
            overflowingPhysicalAlignmentCall);
    assert(result.handled());
    assert(overflowingPhysicalAlignmentCall.returnValue == 0);

    assert(registry.bindPhysicalMemoryArena(
            PhysicalArenaBase,
            PhysicalArenaSize,
            PhysicalAddressOffset));
    assert(!registry.bindPhysicalMemoryArena(
            PhysicalArenaBase + PhysicalArenaSize,
            0x10000U,
            PhysicalAddressOffset));
    memory[physicalAllocation] = 0xA5;
    registry.unbindPhysicalMemoryArena();
    assert(memory[physicalAllocation] == 0);
    result = registry.dispatch(
            "__imp__MmGetPhysicalAddress",
            getPhysicalAddressCall);
    assert(result.state == GuestImportDispatchState::Rejected);

    auto unboundTlsAllocCall = callWith({});
    result = registry.dispatch("__imp__KeTlsAlloc", unboundTlsAllocCall);
    assert(result.state == GuestImportDispatchState::Rejected);
    assert(unboundTlsAllocCall.returnValue == 0xFFFFFFFFU);

    auto processTypeCall = callWith({});
    processTypeCall.returnValue = 0xFFFFFFFFFFFFFFFFULL;
    result = registry.dispatch("__imp__KeGetCurrentProcessType", processTypeCall);
    assert(result.handled());
    assert(processTypeCall.returnValue == 1);
    assert(result.diagnostic.find("user-process") != std::string::npos);

    constexpr std::uint32_t XexHeader = 0x00004000U;
    memory[XexHeader] = 'X';
    memory[XexHeader + 1] = 'E';
    memory[XexHeader + 2] = 'X';
    memory[XexHeader + 3] = '2';
    writeU32(memory, XexHeader + 8, 40);
    writeU32(memory, XexHeader + 20, 2);
    writeU32(memory, XexHeader + 24, 0x00020401U);
    writeU32(memory, XexHeader + 28, 0x00100000U);
    writeU32(memory, XexHeader + 32, 0x00030000U);
    writeU32(memory, XexHeader + 36, 1U << 10U);
    auto xexHeaderCall = callWith({XexHeader, 0x00020401U});
    result = registry.dispatch("__imp__RtlImageXexHeaderField", xexHeaderCall);
    assert(result.handled());
    assert(xexHeaderCall.returnValue == XexHeader + 28);

    auto absentXexHeaderCall = callWith({XexHeader, 0x00040006U});
    absentXexHeaderCall.returnValue = 0xFFFFFFFFU;
    result = registry.dispatch(
            "__imp__RtlImageXexHeaderField",
            absentXexHeaderCall);
    assert(result.handled());
    assert(absentXexHeaderCall.returnValue == 0);

    auto invalidXexHeaderCall = callWith({0xFFFFFFFCU, 0x00020401U});
    result = registry.dispatch(
            "__imp__RtlImageXexHeaderField",
            invalidXexHeaderCall);
    assert(result.state == GuestImportDispatchState::GuestMemoryFault);

    assert(!registry.bindExecutableXexHeader(0));
    assert(registry.bindExecutableXexHeader(XexHeader));
    auto presentPrivilegeCall = callWith({10});
    result = registry.dispatch(
            "__imp__XexCheckExecutablePrivilege",
            presentPrivilegeCall);
    assert(result.handled());
    assert(presentPrivilegeCall.returnValue == 1);
    auto absentPrivilegeCall = callWith({9});
    result = registry.dispatch(
            "__imp__XexCheckExecutablePrivilege",
            absentPrivilegeCall);
    assert(result.handled());
    assert(absentPrivilegeCall.returnValue == 0);
    auto invalidPrivilegeCall = callWith({32});
    result = registry.dispatch(
            "__imp__XexCheckExecutablePrivilege",
            invalidPrivilegeCall);
    assert(result.state == GuestImportDispatchState::Rejected);
    registry.unbindExecutableXexHeader();
    result = registry.dispatch(
            "__imp__XexCheckExecutablePrivilege",
            absentPrivilegeCall);
    assert(result.state == GuestImportDispatchState::Rejected);
    assert(registry.bindExecutableXexHeader(XexHeader));

    constexpr std::uint32_t CriticalSection = 0x00005000U;
    std::fill(memory + CriticalSection, memory + CriticalSection + 28, 0xA5);
    auto criticalSectionCall = callWith({CriticalSection});
    criticalSectionCall.returnValue = 0x12345678U;
    result = registry.dispatch(
            "__imp__RtlInitializeCriticalSection",
            criticalSectionCall);
    assert(result.handled());
    assert(criticalSectionCall.returnValue == 0x12345678U);
    assert(memory[CriticalSection] == 1);
    for (std::uint32_t offset = 1; offset < 16; ++offset) {
        assert(memory[CriticalSection + offset] == 0);
    }
    assert(readU32(memory, CriticalSection + 16) == 0xFFFFFFFFU);
    assert(readU32(memory, CriticalSection + 20) == 0);
    assert(readU32(memory, CriticalSection + 24) == 0);

    constexpr std::uint32_t SpinCriticalSection = 0x00005200U;
    auto spinCriticalSectionCall = callWith({SpinCriticalSection, 1000});
    result = registry.dispatch(
            "__imp__RtlInitializeCriticalSectionAndSpinCount",
            spinCriticalSectionCall);
    assert(result.handled());
    assert(spinCriticalSectionCall.returnValue == 0);
    assert(memory[SpinCriticalSection] == 1);
    assert(memory[SpinCriticalSection + 1] == 4);
    assert(readU32(memory, SpinCriticalSection + 16) == 0xFFFFFFFFU);
    assert(readU32(memory, SpinCriticalSection + 20) == 0);
    assert(readU32(memory, SpinCriticalSection + 24) == 0);

    auto unboundEnterCall = callWith({CriticalSection});
    unboundEnterCall.returnValue = 0x12345678U;
    result = registry.dispatch("__imp__RtlEnterCriticalSection", unboundEnterCall);
    assert(result.state == GuestImportDispatchState::Rejected);
    assert(std::string(guestImportDispatchStateName(result.state)) == "rejected");
    assert(unboundEnterCall.returnValue == 0x12345678U);
    assert(readU32(memory, CriticalSection + 16) == 0xFFFFFFFFU);
    assert(readU32(memory, CriticalSection + 20) == 0);
    assert(readU32(memory, CriticalSection + 24) == 0);

    constexpr std::uint32_t FirstGuestThreadToken = 0x00008FF0U;
    constexpr std::uint32_t SecondGuestThreadToken = 0x00009FF0U;
    assert(!registry.bindGuestThreadToken(0));
    assert(registry.bindGuestThreadToken(FirstGuestThreadToken));

    auto tlsAllocCall = callWith({});
    result = registry.dispatch("__imp__KeTlsAlloc", tlsAllocCall);
    assert(result.handled());
    assert(tlsAllocCall.returnValue == 0);
    auto tlsGetCall = callWith({0});
    result = registry.dispatch("__imp__KeTlsGetValue", tlsGetCall);
    assert(result.handled());
    assert(tlsGetCall.returnValue == 0);
    auto tlsSetCall = callWith({0, 0x12345678U});
    result = registry.dispatch("__imp__KeTlsSetValue", tlsSetCall);
    assert(result.handled());
    assert(tlsSetCall.returnValue == 1);
    result = registry.dispatch("__imp__KeTlsGetValue", tlsGetCall);
    assert(result.handled());
    assert(tlsGetCall.returnValue == 0x12345678U);

    auto enterCriticalSectionCall = callWith({CriticalSection});
    enterCriticalSectionCall.returnValue = 0x87654321U;
    result = registry.dispatch(
            "__imp__RtlEnterCriticalSection",
            enterCriticalSectionCall);
    assert(result.handled());
    assert(enterCriticalSectionCall.returnValue == 0x87654321U);
    assert(readU32(memory, CriticalSection + 16) == 0);
    assert(readU32(memory, CriticalSection + 20) == 1);
    assert(readU32(memory, CriticalSection + 24) == FirstGuestThreadToken);

    result = registry.dispatch(
            "__imp__RtlEnterCriticalSection",
            enterCriticalSectionCall);
    assert(result.handled());
    assert(readU32(memory, CriticalSection + 16) == 1);
    assert(readU32(memory, CriticalSection + 20) == 2);
    assert(readU32(memory, CriticalSection + 24) == FirstGuestThreadToken);

    assert(registry.bindGuestThreadToken(SecondGuestThreadToken));
    result = registry.dispatch("__imp__KeTlsGetValue", tlsGetCall);
    assert(result.handled());
    assert(tlsGetCall.returnValue == 0);
    tlsSetCall = callWith({0, 0x87654321U});
    result = registry.dispatch("__imp__KeTlsSetValue", tlsSetCall);
    assert(result.handled());
    assert(tlsSetCall.returnValue == 1);
    const auto contendedLockCount = readU32(memory, CriticalSection + 16);
    const auto contendedRecursionCount = readU32(memory, CriticalSection + 20);
    auto contendedEnterCall = callWith({CriticalSection});
    result = registry.dispatch("__imp__RtlEnterCriticalSection", contendedEnterCall);
    assert(result.state == GuestImportDispatchState::Rejected);
    assert(result.diagnostic.find("unsupported guest waits") != std::string::npos);
    assert(readU32(memory, CriticalSection + 16) == contendedLockCount);
    assert(readU32(memory, CriticalSection + 20) == contendedRecursionCount);
    assert(readU32(memory, CriticalSection + 24) == FirstGuestThreadToken);

    auto wrongOwnerLeaveCall = callWith({CriticalSection});
    result = registry.dispatch("__imp__RtlLeaveCriticalSection", wrongOwnerLeaveCall);
    assert(result.state == GuestImportDispatchState::Rejected);
    assert(readU32(memory, CriticalSection + 16) == contendedLockCount);
    assert(readU32(memory, CriticalSection + 20) == contendedRecursionCount);
    assert(readU32(memory, CriticalSection + 24) == FirstGuestThreadToken);

    assert(registry.bindGuestThreadToken(FirstGuestThreadToken));
    result = registry.dispatch("__imp__KeTlsGetValue", tlsGetCall);
    assert(result.handled());
    assert(tlsGetCall.returnValue == 0x12345678U);
    auto tlsFreeCall = callWith({0});
    result = registry.dispatch("__imp__KeTlsFree", tlsFreeCall);
    assert(result.handled());
    assert(tlsFreeCall.returnValue == 1);
    result = registry.dispatch("__imp__KeTlsFree", tlsFreeCall);
    assert(result.handled());
    assert(tlsFreeCall.returnValue == 0);
    result = registry.dispatch("__imp__KeTlsSetValue", tlsSetCall);
    assert(result.handled());
    assert(tlsSetCall.returnValue == 0);
    auto leaveCriticalSectionCall = callWith({CriticalSection});
    result = registry.dispatch("__imp__RtlLeaveCriticalSection", leaveCriticalSectionCall);
    assert(result.handled());
    assert(readU32(memory, CriticalSection + 16) == 0);
    assert(readU32(memory, CriticalSection + 20) == 1);
    assert(readU32(memory, CriticalSection + 24) == FirstGuestThreadToken);

    result = registry.dispatch("__imp__RtlLeaveCriticalSection", leaveCriticalSectionCall);
    assert(result.handled());
    assert(readU32(memory, CriticalSection + 16) == 0xFFFFFFFFU);
    assert(readU32(memory, CriticalSection + 20) == 0);
    assert(readU32(memory, CriticalSection + 24) == 0);

    result = registry.dispatch("__imp__RtlLeaveCriticalSection", leaveCriticalSectionCall);
    assert(result.state == GuestImportDispatchState::Rejected);

    registry.unbindGuestThreadToken();
    result = registry.dispatch("__imp__RtlEnterCriticalSection", enterCriticalSectionCall);
    assert(result.state == GuestImportDispatchState::Rejected);
    assert(registry.bindGuestThreadToken(FirstGuestThreadToken));

    constexpr std::uint32_t UninitializedCriticalSection = CriticalSection + 0x100U;
    auto uninitializedEnterCall = callWith({UninitializedCriticalSection});
    result = registry.dispatch("__imp__RtlEnterCriticalSection", uninitializedEnterCall);
    assert(result.state == GuestImportDispatchState::Rejected);

    constexpr std::uint32_t StaticCriticalSection = CriticalSection + 0x300U;
    std::fill(memory + StaticCriticalSection, memory + StaticCriticalSection + 28, 0);
    memory[StaticCriticalSection] = 1;
    memory[StaticCriticalSection + 2] = 4;
    writeU32(memory, StaticCriticalSection + 8, StaticCriticalSection + 8);
    writeU32(memory, StaticCriticalSection + 12, StaticCriticalSection + 8);
    writeU32(memory, StaticCriticalSection + 16, 0xFFFFFFFFU);
    const std::array<std::uint8_t, 16> staticHeader{
            1, 0, 4, 0,
            0, 0, 0, 0,
            0, 0, 0x53, 0x08,
            0, 0, 0x53, 0x08,
    };
    assert(std::equal(
            staticHeader.begin(),
            staticHeader.end(),
            memory + StaticCriticalSection));
    auto staticEnterCall = callWith({StaticCriticalSection});
    result = registry.dispatch("__imp__RtlEnterCriticalSection", staticEnterCall);
    assert(result.handled());
    assert(std::equal(
            staticHeader.begin(),
            staticHeader.end(),
            memory + StaticCriticalSection));
    assert(readU32(memory, StaticCriticalSection + 16) == 0);
    assert(readU32(memory, StaticCriticalSection + 20) == 1);
    assert(readU32(memory, StaticCriticalSection + 24) == FirstGuestThreadToken);
    auto staticLeaveCall = callWith({StaticCriticalSection});
    result = registry.dispatch("__imp__RtlLeaveCriticalSection", staticLeaveCall);
    assert(result.handled());
    assert(std::equal(
            staticHeader.begin(),
            staticHeader.end(),
            memory + StaticCriticalSection));
    assert(readU32(memory, StaticCriticalSection + 16) == 0xFFFFFFFFU);
    assert(readU32(memory, StaticCriticalSection + 20) == 0);
    assert(readU32(memory, StaticCriticalSection + 24) == 0);
    result = registry.dispatch("__imp__RtlEnterCriticalSection", staticEnterCall);
    assert(result.handled());
    memory[StaticCriticalSection + 4] = 1;
    result = registry.dispatch("__imp__RtlEnterCriticalSection", staticEnterCall);
    assert(result.state == GuestImportDispatchState::Rejected);

    auto overlappingInitializeCall = callWith({CriticalSection + 4U});
    result = registry.dispatch(
            "__imp__RtlInitializeCriticalSection",
            overlappingInitializeCall);
    // Explicit initialization retires an idle shadow identity when storage is
    // reused; Xbox titles do not issue a critical-section delete import.
    assert(result.handled());

    constexpr std::uint32_t CorruptedCriticalSection = CriticalSection + 0x200U;
    auto corruptedInitializeCall = callWith({CorruptedCriticalSection});
    result = registry.dispatch(
            "__imp__RtlInitializeCriticalSection",
            corruptedInitializeCall);
    assert(result.handled());
    writeU32(memory, CorruptedCriticalSection + 16, 0);
    auto corruptedEnterCall = callWith({CorruptedCriticalSection});
    result = registry.dispatch("__imp__RtlEnterCriticalSection", corruptedEnterCall);
    assert(result.state == GuestImportDispatchState::Rejected);

    auto invalidCriticalSectionCall = callWith({0xFFFFFFF0U});
    result = registry.dispatch(
            "__imp__RtlInitializeCriticalSection",
            invalidCriticalSectionCall);
    assert(result.state == GuestImportDispatchState::GuestMemoryFault);

    constexpr std::uint32_t DebugText = 0x00002000U;
    writeAnsi(memory, DebugText, "boot value=%u");
    auto debugCall = callWith({DebugText, 42});
    result = registry.dispatch("__imp__DbgPrint", debugCall);
    assert(result.handled());
    assert(loggedMessage == "boot value=%u");
    assert(debugCall.returnValue == loggedMessage.size());
    assert(result.diagnostic.find("without interpreting varargs") != std::string::npos);

    auto unknownCall = callWith({});
    result = registry.dispatch("__imp__VdSwap", unknownCall);
    assert(result.state == GuestImportDispatchState::NotRegistered);
    assert(std::string(guestImportDispatchStateName(result.state)) == "not-registered");

    constexpr std::uint32_t AnsiSource = 0x00002100U;
    constexpr std::uint32_t AnsiDescriptor = 0x00002200U;
    writeAnsi(memory, AnsiSource, "hello");
    auto ansiCall = callWith({AnsiDescriptor, AnsiSource});
    result = registry.dispatch("__imp__RtlInitAnsiString", ansiCall);
    assert(result.handled());
    assert(readU16(memory, AnsiDescriptor) == 5);
    assert(readU16(memory, AnsiDescriptor + 2) == 6);
    assert(readU32(memory, AnsiDescriptor + 4) == AnsiSource);

    auto nullAnsiCall = callWith({AnsiDescriptor, 0});
    result = registry.dispatch("__imp__RtlInitAnsiString", nullAnsiCall);
    assert(result.handled());
    assert(readU16(memory, AnsiDescriptor) == 0);
    assert(readU16(memory, AnsiDescriptor + 2) == 0);
    assert(readU32(memory, AnsiDescriptor + 4) == 0);

    constexpr std::uint32_t UnicodeSource = 0x00002300U;
    constexpr std::uint32_t UnicodeDescriptor = 0x00002400U;
    writeU16(memory, UnicodeSource, 'A');
    writeU16(memory, UnicodeSource + 2, 'B');
    writeU16(memory, UnicodeSource + 4, 0);
    auto unicodeCall = callWith({UnicodeDescriptor, UnicodeSource});
    result = registry.dispatch("__imp__RtlInitUnicodeString", unicodeCall);
    assert(result.handled());
    assert(readU16(memory, UnicodeDescriptor) == 4);
    assert(readU16(memory, UnicodeDescriptor + 2) == 6);
    assert(readU32(memory, UnicodeDescriptor + 4) == UnicodeSource);

    constexpr std::uint32_t CompareSource = 0x00002500U;
    constexpr std::uint32_t Pattern = 0x1234ABCDU;
    writeU32(memory, CompareSource, Pattern);
    writeU32(memory, CompareSource + 4, Pattern);
    writeU32(memory, CompareSource + 8, 0xFFFFFFFFU);
    auto compareCall = callWith({CompareSource, 12, Pattern});
    result = registry.dispatch("__imp__RtlCompareMemoryUlong", compareCall);
    assert(result.handled());
    assert(compareCall.returnValue == 8);

    auto unalignedCompareCall = callWith({CompareSource + 1, 8, Pattern});
    result = registry.dispatch("__imp__RtlCompareMemoryUlong", unalignedCompareCall);
    assert(result.handled());
    assert(unalignedCompareCall.returnValue == 0);

    auto invalidDescriptorCall = callWith({0xFFFFFFFCU, AnsiSource});
    result = registry.dispatch("__imp__RtlInitAnsiString", invalidDescriptorCall);
    assert(result.state == GuestImportDispatchState::GuestMemoryFault);
    assert(std::string(guestImportDispatchStateName(result.state)) == "guest-memory-fault");

    auto nullDescriptorCall = callWith({0, AnsiSource});
    result = registry.dispatch("__imp__RtlInitAnsiString", nullDescriptorCall);
    assert(result.state == GuestImportDispatchState::GuestMemoryFault);

    auto nullCompareCall = callWith({0, 4, Pattern});
    result = registry.dispatch("__imp__RtlCompareMemoryUlong", nullCompareCall);
    assert(result.state == GuestImportDispatchState::GuestMemoryFault);

    constexpr std::uint32_t BasePointer = 0x00003000U;
    constexpr std::uint32_t SizePointer = 0x00003004U;
    for (std::uint32_t offset = 0; offset < HeapSize; ++offset) {
        memory[HeapBase + offset] = 0xA5;
    }
    writeU32(memory, BasePointer, 0);
    writeU32(memory, SizePointer, 0x1001U);
    auto allocateCall = callWith({BasePointer, SizePointer, 0x3000U, 0x04U, 0});
    result = registry.dispatch("__imp__NtAllocateVirtualMemory", allocateCall);
    assert(result.handled());
    assert(allocateCall.returnValue == 0);
    assert(readU32(memory, BasePointer) == HeapBase);

    constexpr std::uint32_t LargeReservationBasePointer = 0x00003100U;
    constexpr std::uint32_t LargeReservationSizePointer = 0x00003104U;
    writeU32(memory, LargeReservationBasePointer, 0);
    writeU32(memory, LargeReservationSizePointer, 0x10001U);
    auto reserveLargeHeapCall = callWith({
            LargeReservationBasePointer,
            LargeReservationSizePointer,
            0x60002000U,
            0x04U,
            0,
    });
    result = registry.dispatch(
            "__imp__NtAllocateVirtualMemory",
            reserveLargeHeapCall);
    assert(result.handled());
    assert(reserveLargeHeapCall.returnValue == 0);
    const auto largeReservation = readU32(memory, LargeReservationBasePointer);
    assert(largeReservation == HeapBase + 0x10000U);
    assert((largeReservation % 0x10000U) == 0);
    assert(readU32(memory, LargeReservationSizePointer) == 0x20000U);
    assert(memory[largeReservation] == 0xA5);

    writeU32(memory, LargeReservationBasePointer, largeReservation);
    writeU32(memory, LargeReservationSizePointer, 0x10000U);
    auto commitLargeHeapCall = callWith({
            LargeReservationBasePointer,
            LargeReservationSizePointer,
            0x60001000U,
            0x04U,
            0,
    });
    result = registry.dispatch(
            "__imp__NtAllocateVirtualMemory",
            commitLargeHeapCall);
    assert(result.handled());
    assert(commitLargeHeapCall.returnValue == 0);
    assert(readU32(memory, LargeReservationBasePointer) == largeReservation);
    assert(readU32(memory, LargeReservationSizePointer) == 0x10000U);
    assert(memory[largeReservation] == 0);
    assert(readU32(memory, SizePointer) == 0x2000U);
    for (std::uint32_t offset = 0; offset < 0x2000U; ++offset) {
        assert(memory[HeapBase + offset] == 0);
    }

    writeU32(memory, BasePointer + 8, 0);
    writeU32(memory, SizePointer + 8, 1);
    auto secondAllocateCall = callWith(
            {BasePointer + 8, SizePointer + 8, 0x3000U, 0x04U, 0});
    result = registry.dispatch("__imp__NtAllocateVirtualMemory", secondAllocateCall);
    assert(result.handled());
    assert(secondAllocateCall.returnValue == 0);
    assert(readU32(memory, BasePointer + 8) == HeapBase + 0x2000U);
    assert(readU32(memory, SizePointer + 8) == 0x1000U);

    writeU32(memory, BasePointer + 16, 0);
    writeU32(memory, SizePointer + 16, 0x20000U);
    auto exhaustedCall = callWith(
            {BasePointer + 16, SizePointer + 16, 0x3000U, 0x04U, 0});
    result = registry.dispatch("__imp__NtAllocateVirtualMemory", exhaustedCall);
    assert(result.handled());
    assert(exhaustedCall.returnValue == 0xC0000017U);

    writeU32(memory, SizePointer, 0);
    auto freeCall = callWith({BasePointer, SizePointer, 0x8000U, 0});
    result = registry.dispatch("__imp__NtFreeVirtualMemory", freeCall);
    assert(result.handled());
    assert(freeCall.returnValue == 0);
    assert(readU32(memory, BasePointer) == HeapBase);
    assert(readU32(memory, SizePointer) == 0x2000U);

    writeU32(memory, BasePointer, 0);
    writeU32(memory, SizePointer, 0x2000U);
    auto reusedCall = callWith({BasePointer, SizePointer, 0x3000U, 0x04U, 0});
    result = registry.dispatch("__imp__NtAllocateVirtualMemory", reusedCall);
    assert(result.handled());
    assert(reusedCall.returnValue == 0);
    assert(readU32(memory, BasePointer) == HeapBase);

    writeU32(memory, BasePointer + 24, 0x02000000U);
    writeU32(memory, SizePointer + 24, 0x1000U);
    auto fixedAddressCall = callWith(
            {BasePointer + 24, SizePointer + 24, 0x3000U, 0x04U, 0});
    result = registry.dispatch("__imp__NtAllocateVirtualMemory", fixedAddressCall);
    assert(result.handled());
    assert(fixedAddressCall.returnValue == 0xC00000BBU);

    writeU32(memory, BasePointer + 24, 0);
    auto unsupportedReserveCall = callWith(
            {BasePointer + 24, SizePointer + 24, 0x00102000U, 0x04U, 0});
    result = registry.dispatch(
            "__imp__NtAllocateVirtualMemory",
            unsupportedReserveCall);
    assert(result.handled());
    assert(unsupportedReserveCall.returnValue == 0xC00000BBU);

    registry.reset();
    assert(!registry.isReady());
    result = registry.dispatch("__imp__DbgPrint", debugCall);
    assert(result.state == GuestImportDispatchState::RegistryUnavailable);
    testGuestVisibleTlsStorage();
}
