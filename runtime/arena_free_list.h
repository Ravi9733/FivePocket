// Attempt v308: the stream arena's free list (v260) with a size index. Free blocks are kept by offset (coalescing on
// release) and, in the same state, by (size, offset). alloc(n, false) is v260's first fit (the lowest-offset block that is
// large enough: a linear scan); alloc(n, true) is best fit (the smallest large-enough block, lowest offset among equals: one
// set lookup). Both take n from the block's start and keep the remainder free. The two modes place blocks differently; the
// contents a caller writes are its own, so only the arena's retention changes, never what a draw reads.
// Pure; host test: tests/arena_free_list_test.cpp (random alloc/release against a byte map; RECOMP_V308_MUTANT_STALE_SIZE).
#pragma once
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace recomp::gpu::generic::v308 {
class ArenaFreeList {
public:
    using Size = std::uint64_t;
    void reset(Size capacity) {byOffset.clear();bySize.clear();insert(0,capacity);used=0;}
    std::optional<Size> alloc(Size n,bool bestFit) {
        if(!n)return std::nullopt;
        if(bestFit) {
            const auto it=bySize.lower_bound({n,0});
            if(it==bySize.end())return std::nullopt;
            const Size at=it->second,size=it->first;
            erase(at,size);if(size>n)insert(at+n,size-n);
            used+=n;return at;
        }
        for(auto it=byOffset.begin();it!=byOffset.end();++it)if(it->second>=n) {
            const Size at=it->first,size=it->second;
            erase(at,size);if(size>n)insert(at+n,size-n);
            used+=n;return at;
        }
        return std::nullopt;
    }
    void release(Size at,Size n) {
        used-=std::min(used,n);
        auto next=byOffset.lower_bound(at);
        if(next!=byOffset.end() && at+n==next->first) {const Size s=next->second;const Size o=next->first;++next;erase(o,s);n+=s;}
        if(next!=byOffset.begin()) {
            auto prev=std::prev(next);
            if(prev->first+prev->second==at) {
                const Size o=prev->first,s=prev->second;
#if defined(RECOMP_V308_MUTANT_STALE_SIZE) // test mutant: the merged block's old size stays in the size index
                byOffset.erase(o);
#else
                erase(o,s);
#endif
                insert(o,s+n);return;
            }
        }
        insert(at,n);
    }
    Size usedBytes()const{return used;}
    std::size_t blocks()const{return byOffset.size();}
    Size largest()const{return bySize.empty()?0:std::prev(bySize.end())->first;}
    const std::map<Size,Size>& offsets()const{return byOffset;}
private:
    void insert(Size at,Size n){byOffset[at]=n;bySize.insert({n,at});}
    void erase(Size at,Size n){byOffset.erase(at);bySize.erase({n,at});}
    std::map<Size,Size> byOffset;             // offset -> size
    std::set<std::pair<Size,Size>> bySize;    // (size, offset)
    Size used=0;
};
}
