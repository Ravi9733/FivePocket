#pragma once
// v207: whether [address, address + bytes) is covered by allocations that follow one another without a gap.
// `allocations` maps an allocation's base address to a value with a `size` member (GuestImportRegistry's
// physicalAllocations_, keyed by the arena addresses the physical range maps to). Returns the base of the allocation
// holding `address`, or nullopt when a byte of the range is outside every allocation, the next allocation does not
// start exactly where the previous one ends, or more than `maximumPieces` allocations would be needed.
// The Xenos fetches physical memory linearly; the registry's single-allocation rule (resolvePhysicalRange) refused a
// driving vertex stream laid over three adjacent owned allocations (v206: 48 KB + 72 KB + 10 KB of 133128 bytes).
#include <cstdint>
#include <iterator>
#include <optional>

namespace recomp::bootstrap {
template <class Map>
std::optional<std::uint32_t> adjacentAllocationCover(const Map& allocations, std::uint32_t address, std::uint64_t bytes,
                                                     unsigned maximumPieces = 16) {
    if (!bytes) return std::nullopt;
    auto found = allocations.upper_bound(address);
    if (found == allocations.begin()) return std::nullopt;
    --found;
    const std::uint32_t first = found->first;
    std::uint64_t cursor = address;
    const std::uint64_t end = std::uint64_t{address} + bytes;
    for (unsigned piece = 0; piece < maximumPieces; ++piece) {
        const std::uint64_t base = found->first, limit = base + found->second.size;
        if (cursor < base || cursor >= limit) return std::nullopt;
        if (end <= limit) return first;
        cursor = limit;
        if (++found == allocations.end() || std::uint64_t{found->first} != cursor) return std::nullopt; // a gap
    }
    return std::nullopt;
}
} // namespace recomp::bootstrap
