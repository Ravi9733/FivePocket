// Attempt v303: the renderer's "images read / written since the last barrier" lists (targeted barriers) as a set with O(1)
// membership. v300 profile: the linear finds over these vectors in draw() and hazards() were ~0.8% of the worker thread.
// The vector keeps the members (emptiness, the same clear points); a 1024-slot open-addressing table whose slots carry the
// set's epoch answers membership (clear() bumps the epoch: no zeroing). Above 512 members the table stops taking entries and
// membership falls back to the vector search until the next clear. contains(i,false) is always the vector search (the
// in-run baseline). Pure; host test: tests/image_set_test.cpp (random operation sequences against std::find on the vector).
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace recomp::gpu::generic::v303 {
template<class Handle> struct ImageSet {
    static constexpr unsigned Slots=1024,MaxTabled=512;
    std::vector<Handle> list;
    struct Slot {std::uint64_t key=0;std::uint32_t epoch=0;};
    std::array<Slot,Slots> slots{};
    std::uint32_t epoch=1;
    static std::uint64_t keyOf(Handle h){std::uint64_t v=0;std::memcpy(&v,&h,sizeof h<sizeof v?sizeof h:sizeof v);return v;}
    static unsigned home(std::uint64_t k){k^=k>>33;k*=0xFF51AFD7ED558CCDull;k^=k>>33;return unsigned(k)&(Slots-1);}
    bool empty() const {return list.empty();}
    std::size_t size() const {return list.size();}
    void clear() {
        list.clear();
        if(++epoch==0){slots.fill({});epoch=1;} // (after 2^32 clears: stale epochs could alias the new one)
    }
    bool contains(Handle h,bool fast=true) const {
        if(!fast || list.size()>MaxTabled)return std::find(list.begin(),list.end(),h)!=list.end();
        const auto k=keyOf(h);
        for(unsigned s=home(k);;s=(s+1)&(Slots-1)) {
            const auto& e=slots[s];
            if(e.epoch!=epoch)return false;
#if !defined(RECOMP_V303_MUTANT_FIRST_SLOT) // test mutant: only the home slot is checked (collisions answer "absent")
            if(e.key==k)return true;
#else
            return e.key==k;
#endif
        }
    }
    // Adds h; the caller has checked !contains(h).
    void insert(Handle h) {
        list.push_back(h);
        if(list.size()>MaxTabled)return;
        const auto k=keyOf(h);
        for(unsigned s=home(k);;s=(s+1)&(Slots-1))if(slots[s].epoch!=epoch){slots[s]={k,epoch};return;}
    }
};
}
