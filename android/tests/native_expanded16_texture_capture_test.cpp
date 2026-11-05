#include "native_expanded16_texture_capture.h"
#include <cassert>
#include <cstdio>

using namespace recomp::gpu;
int main() {
    // Authored descriptor in the observed storage family; no game payload.
    const std::array<unsigned,6> fetch{0x85004956,0x0100005D,0x2CE27F,0xA80D11,3,0x210};
    constexpr unsigned bytes=1966080,base=0x01000000;
    std::vector<std::uint8_t> owned(bytes);
    for(unsigned i=0;i<bytes;++i)owned[i]=static_cast<std::uint8_t>((i*31u)^(i>>13));
    unsigned calls=0;
    Gta5ResourceReader reader=[&](auto a,auto n)->std::optional<std::vector<std::uint8_t>> {
        ++calls;assert(a==base && n==bytes);return owned;
    };
    const auto result=captureNativeExpanded16Base(5,fetch,reader,bytes);
    assert(result && result->source==owned && result->fetch==fetch && result->slot==5 && !result->literalZero && calls==1);
    const auto refuse=[&](auto changed,unsigned slot=5,std::uint64_t budget=bytes) {
        calls=0;assert(!captureNativeExpanded16Base(slot,changed,reader,budget));assert(!calls);
    };
    refuse(fetch,32);refuse(fetch,5,bytes-1);
    for(auto format:{6u,25u,32u,36u,54u}){auto f=fetch;f[1]=(f[1]&~63u)|format;refuse(f);}
    for(auto address:{0u,0x1FFFF000u,0x20000000u,0xA1000000u}){auto f=fetch;f[1]=(f[1]&4095u)|address;refuse(f);}
    for(auto pair:{std::pair{4u,1u<<6},std::pair{5u,0x1000u},std::pair{5u,2048u},std::pair{3u,1u<<13}}) {
        auto f=fetch;f[pair.first]|=pair.second;refuse(f);
    }
    for(unsigned lane=0;lane<4;++lane)for(unsigned sign:{2u,3u}) {
        auto f=fetch;f[0]=(f[0]&~(3u<<(2+lane*2)))|(sign<<(2+lane*2));refuse(f);
    }
    assert(!captureNativeExpanded16Base(5,fetch,{}));
    for(int delta:{-1,1}) {
        Gta5ResourceReader shortRead=[&](auto,auto)->std::optional<std::vector<std::uint8_t>> {
            return std::vector<std::uint8_t>(bytes+delta);
        };
        assert(!captureNativeExpanded16Base(5,fetch,shortRead));
    }
    Gta5ResourceReader unavailable=[](auto,auto)->std::optional<std::vector<std::uint8_t>> {return std::nullopt;};
    assert(!captureNativeExpanded16Base(5,fetch,unavailable));
    std::puts("EXPANDED16_CAPTURE_PASS full_storage=1 exact_submission=1 budget_address_sign_mip_short_refusals=1 sampling_authority=0");
}
