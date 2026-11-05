#pragma once
// v571 (fixer, 2026-10-21): the guest bytes of an index range that need not start on a 4-byte boundary.
// The registry's graphics-data view (GuestImportRegistry::viewSerializedGraphicsDataLocked) refuses an address that is not
// a multiple of 4. A draw of 16-bit indices whose start index is ODD begins 2 bytes past such a boundary, so every such draw
// was skipped whole ("index-data": in the v569 Prologue census 22 of 22 skipped draws, all vfx_decal pieces of one index
// buffer, had an odd start; none with an even one). The Xenos reads the index buffer at its own address. Here the view is
// taken from the 4-byte boundary at or below the address (the same allocation: allocations are page aligned and an index
// buffer starts on its element size) and the pointer is advanced by the difference. Nothing else changes: an aligned
// address reads exactly as before (pad 0).
#include <cstdint>

namespace recomp::gpu::index_view {
// view(address, bytes) -> pointer or nullptr (the registry's view). Returns the bytes of [address, address + bytes).
template<class View>
inline const std::uint8_t* at(const View& view,std::uint32_t address,std::uint32_t bytes) noexcept {
    const std::uint32_t pad=address&3u;
    if(!pad)return view(address,bytes);
    if(bytes>0xFFFFFFFFu-pad)return nullptr;
    const std::uint8_t* p=view(address-pad,bytes+pad);
    return p?p+pad:nullptr;
}
}
