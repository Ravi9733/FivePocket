#pragma once
#include "gta5_driver_textures.h"

namespace recomp::gpu {
// Diagnostic ONLY: these are bounded, individually ownership-checked pages
// starting at submitted fetch base/mip addresses. They are not a texture layout,
// a complete mip chain, a sampling fallback, or a new graphics allocation.
// Capture only slots whose full-layout capture failed, under the executor gate.
struct Gta5SubmittedTexturePages {
    static constexpr unsigned PageBytes=4096,PagesPerPlane=16;
    static constexpr unsigned MaxBytes=32*2*PagesPerPlane*PageBytes;
    struct Plane {
        unsigned slot=0;
        bool mip=false,addressSpaceTruncated=false;
        std::uint32_t address=0;
        std::array<std::optional<std::vector<std::uint8_t>>,PagesPerPlane> pages{};
    };
    std::vector<Plane> planes;
    unsigned capturedBytes=0,unavailablePages=0;
};

inline Gta5SubmittedTexturePages captureGta5SubmittedTexturePages(
        const Gta5SubmittedTextureSnapshot& submitted,std::uint32_t completeSlots,
        const Gta5ResourceReader& ownedGraphics) {
    Gta5SubmittedTexturePages result;
    if(!ownedGraphics)return result;
    for(unsigned slot=0;slot<submitted.size();++slot) {
        if((completeSlots&(1u<<slot)) || !submitted[slot])continue;
        const auto description=XenosTextureDescription::decode(*submitted[slot]);
        if(!description)continue;
        for(unsigned mip=0;mip<2;++mip) {
            const auto address=mip?description->mipAddress:description->baseAddress;
            if(!address)continue; // Null is evidence, never a page-zero read.
            auto& plane=result.planes.emplace_back();plane.slot=slot;
            plane.mip=mip!=0;plane.address=address;
            for(unsigned i=0;i<plane.pages.size();++i) {
                const auto at=std::uint64_t{address}+i*Gta5SubmittedTexturePages::PageBytes;
                if(at+Gta5SubmittedTexturePages::PageBytes>0x100000000ULL) {
                    plane.addressSpaceTruncated=true;break;
                }
                auto bytes=ownedGraphics(static_cast<std::uint32_t>(at),Gta5SubmittedTexturePages::PageBytes);
                if(!bytes || bytes->size()!=Gta5SubmittedTexturePages::PageBytes) {
                    ++result.unavailablePages;continue;
                }
                result.capturedBytes+=bytes->size();plane.pages[i]=std::move(bytes);
            }
        }
    }
    return result;
}
}
