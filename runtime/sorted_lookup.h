#pragma once
// v228: the registry snapshot's sorted-allocation search with a caller-held last-hit index (guest_registry_concurrency.inc;
// tests/sorted_lookup_test.cpp). Returns exactly upper_bound(address) - 1 (the last entry whose base is <= address), or
// ~0 when there is none; `cached` is used only when it provably is that entry, otherwise the search refills it.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
namespace recomp::bootstrap {
// The index of the last entry whose base is <= address (upper_bound(address) - 1), or ~0 when none.
inline std::size_t lastAllocationAtOrBelow(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& a, std::uint32_t address,
        std::size_t& cached) noexcept {
    const auto i = cached;
    if (i < a.size() && a[i].first <= address && (i + 1 == a.size() || a[i + 1].first > address)) return i;
    const auto found = std::upper_bound(a.begin(), a.end(), address,
        [](std::uint32_t value, const std::pair<std::uint32_t, std::uint32_t>& e) { return value < e.first; });
    if (found == a.begin()) return ~std::size_t{0};
    cached = static_cast<std::size_t>(found - a.begin()) - 1;
    return cached;
}
}
