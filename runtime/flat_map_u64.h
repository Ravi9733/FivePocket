#pragma once
// v271: an open-addressing hash map with 64-bit keys (linear probing, power-of-two capacity, backward-shift deletion, max
// load 1/2) for the worker's per-draw lookups (v266 outdoor worker profile: std::unordered_map probes ~9% of the thread,
// two dependent cache misses each: the bucket array, then the node). One probe usually touches one cache line.
// Not pointer-stable: an insert may grow the table and an erase may move later entries of the same run, so callers copy
// what they need, or look the key up again, after any other insert or erase. Iteration visits every live slot (erase while
// iterating: use eraseIf). Key 0 is allowed (a separate occupancy byte per slot).
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace recomp::gpu::generic {
template<class V> class FlatMapU64 {
public:
    struct Slot {std::uint64_t key=0;V value{};bool used=false;};
    FlatMapU64() {slots_.resize(64);}
    std::size_t size() const {return size_;}
    V* find(std::uint64_t key) {
        std::size_t i=home(key);
        for(;;){auto& s=slots_[i];if(!s.used)return nullptr;if(s.key==key)return &s.value;i=(i+1)&mask();}
    }
    // The value for `key`, value-initialised when new (as std::unordered_map::operator[]).
    V& operator[](std::uint64_t key) {
        if((size_+1)*2>slots_.size())grow();
        std::size_t i=home(key);
        for(;;){auto& s=slots_[i];if(!s.used){s.used=true;s.key=key;s.value=V{};++size_;return s.value;}if(s.key==key)return s.value;i=(i+1)&mask();}
    }
    bool erase(std::uint64_t key) {
        std::size_t i=home(key);
        for(;;){auto& s=slots_[i];if(!s.used)return false;if(s.key==key)break;i=(i+1)&mask();}
        removeAt(i);return true;
    }
    template<class F> void forEach(F&& f) {for(auto& s:slots_)if(s.used)f(s.key,s.value);}
    template<class F> void forEach(F&& f) const {for(const auto& s:slots_)if(s.used)f(s.key,s.value);}
    // Removes every entry pred(key, value) selects (pred is called once per live entry, before any removal); returns how many.
    template<class P> std::size_t eraseIf(P&& pred) {
        std::vector<std::uint64_t> keys;
        for(auto& s:slots_)if(s.used && pred(s.key,s.value))keys.push_back(s.key);
        for(const auto k:keys)erase(k);
        return keys.size();
    }
    void clear() {for(auto& s:slots_)s=Slot{};size_=0;}
private:
    std::vector<Slot> slots_;std::size_t size_=0;
    std::size_t mask() const {return slots_.size()-1;}
    std::size_t home(std::uint64_t key) const {std::uint64_t h=key*0x9E3779B97F4A7C15ull;h^=h>>32;return std::size_t(h)&mask();}
    void grow() {
        std::vector<Slot> old(slots_.size()*2);old.swap(slots_);size_=0;
        for(auto& s:old)if(s.used){auto& v=(*this)[s.key];v=std::move(s.value);}
    }
    void removeAt(std::size_t hole) { // backward-shift deletion: later entries of the run move up so every probe still finds them
        slots_[hole]=Slot{};--size_;
        std::size_t i=(hole+1)&mask();
        while(slots_[i].used) {
            const std::size_t h=home(slots_[i].key);
            // move slot i into the hole when its home is not in (hole, i] (cyclically)
            const bool between=hole<=i?(h>hole && h<=i):(h>hole || h<=i);
            if(!between){slots_[hole]=std::move(slots_[i]);slots_[i]=Slot{};hole=i;}
            i=(i+1)&mask();
        }
    }
};
} // namespace recomp::gpu::generic
