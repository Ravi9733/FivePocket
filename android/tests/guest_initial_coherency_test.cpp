#include "guest_initial_coherency.h"
#include <cassert>
using namespace recomp::bootstrap;
int main() {
    std::vector<std::uint32_t> root(22);
    root[0] = 0xC0114800; root[1] = 0x3FF;
    for (unsigned i = 0; i < 8; ++i) root[i + 4] = i * 0x80;
    root[12] = 0x10800; root[13] = 7;
    root[19] = 0xC0013F00; root[20] = 0x1000; root[21] = 11;
    std::vector<std::uint32_t> child{0xA31, 0x01000000, 0x10A2F, 0x10000, 0x20000,
        0xC0043C00, 3, 0xA31, 0, 0x80000000, 8};
    NativeInitialCoherency result{7,8,9};
    const auto reject = [&](const auto& a, const auto& b) {
        result = {7,8,9}; assert(!parseNativeInitialCoherency(a, b, result));
        assert(result.physicalBase == 7 && result.byteCount == 8 && result.cacheFlags == 9);
    };
    assert(parseNativeInitialCoherency(root, child, result));
    assert(result.physicalBase == 0x20000 && result.byteCount == 0x10000 && result.cacheFlags == 0x01000000);
    for (unsigned i = 0; i < root.size(); ++i) {
        auto invalid = root; invalid[i] ^= 1; reject(invalid, child);
    }
    for (unsigned i = 0; i < child.size(); ++i) {
        auto invalid = child; invalid[i] ^= 1; reject(root, invalid);
    }
    auto shortRoot = root; shortRoot.pop_back(); reject(shortRoot, child);
    auto longChild = child; longChild.push_back(0x80000000); reject(root, longChild);
    for (auto flags : {0u, 0x02000000u, 0x81000000u, 0xFFFFFFFFu}) {
        auto invalid = child; invalid[1] = flags; reject(root, invalid);
    }
    for (auto pair : {std::pair{0u,0u}, {0x1FFFF000u,0x2000u}, {0x20000000u,0x1000u}, {0u,0xFFFFFFFFu}}) {
        auto invalid = child; invalid[4] = pair.first; invalid[3] = pair.second; reject(root, invalid);
    }
    for (auto flags : {0x01000000u, 0x03000000u}) {
        child[1] = flags; child[4] = 0; child[3] = 0x20000000;
        assert(parseNativeInitialCoherency(root, child, result));
        assert(result.physicalBase == 0 && result.byteCount == 0x20000000 && result.cacheFlags == flags);
    }
    root[20] = 0x1FFFFFD4;
    assert(parseNativeInitialCoherency(root, child, result));
    root[20] += 4; reject(root, child);
    // The later empty-cache profile has no ME_INIT and consumes exactly one IB.
    // Shared child validation must remain equally strict in both entry points.
    const std::vector<std::uint32_t> later{0xC0013F00,0x1000,11};
    const auto rejectLater = [&](const auto& a, const auto& b) {
        result = {7,8,9}; assert(!parseNativeEmptyCacheCoherency(a,b,result));
        assert(result.physicalBase == 7 && result.byteCount == 8 && result.cacheFlags == 9);
    };
    for (auto flags : {0x01000000u,0x03000000u}) {
        child[1] = flags;
        assert(parseNativeEmptyCacheCoherency(later,child,result));
        assert(result.physicalBase == 0 && result.byteCount == 0x20000000 && result.cacheFlags == flags);
    }
    for (unsigned i=0;i<later.size();++i) {
        auto bad=later; bad[i]^=1; rejectLater(bad,child);
    }
    for (unsigned i=0;i<child.size();++i) {
        auto bad=child; bad[i]^=1; rejectLater(later,bad);
    }
    auto shortLater=later; shortLater.pop_back(); rejectLater(shortLater,child);
    auto longLater=later; longLater.push_back(0); rejectLater(longLater,child);
    auto shortChild=child; shortChild.pop_back(); rejectLater(later,shortChild);
    rejectLater(later,longChild); rejectLater(root,child);
    auto edge=later; edge[1]=0x1FFFFFD4;
    assert(parseNativeEmptyCacheCoherency(edge,child,result));
    edge[1]+=4; rejectLater(edge,child);
}
