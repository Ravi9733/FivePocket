#include "xenos_indirect_inspection.h"
#include <cassert>
#include <map>
#include <stdexcept>
using namespace recomp::gpu;
namespace {
std::vector<std::uint8_t> bytes(std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint8_t> out;
    for (auto w : words) for (unsigned i = 0; i < 4; ++i) out.push_back(std::uint8_t(w >> (24 - 8 * i)));
    return out;
}
Pm4Snapshot snapshot(const std::vector<std::uint8_t>& raw) {
    Pm4Snapshot out; Pm4Error e;
    assert(decodePm4(raw.data(), raw.size(), {}, out, e)); return out;
}
}
int main() {
    constexpr auto IB = 0xC0013F00u, PFD = 0xC0013700u, NOP = 0x80000000u;
    std::map<std::uint32_t, std::vector<std::uint8_t>> memory;
    unsigned reads = 0;
    const auto reader = [&](auto at, auto length) -> std::optional<std::vector<std::uint8_t>> {
        ++reads; const auto found = memory.find(at);
        if (found == memory.end() || found->second.size() != length) return std::nullopt;
        return found->second;
    };
    const auto root = snapshot(bytes({IB, 0x1000, 4, PFD, 0x2000, 1}));
    memory[0x1000] = bytes({NOP, IB, 0x2000, 1}); memory[0x2000] = bytes({NOP});
    Pm4IndirectGraph out; Pm4GraphError error;
    assert(inspectPm4IndirectGraph(root, reader, out, error));
    assert(reads == 3 && out.nodes.size() == 3 && out.totalWords == 12 && out.totalPackets == 6);
    assert(out.nodes[0].parent == 0 && out.nodes[0].depth == 1 && out.nodes[0].parentOffsetWords == 0);
    assert(out.nodes[1].parent == 1 && out.nodes[1].depth == 2 && out.nodes[1].parentOffsetWords == 1);
    assert(out.nodes[2].parent == 0 && out.nodes[2].physicalAddress == 0x2000 && out.nodes[2].parentOffsetWords == 3);
    memory[0x2000][0] = 0xC0; assert(out.nodes[1].snapshot.words()[0] == NOP); memory[0x2000][0] = 0x80;
    const auto unchanged = [&] { assert(out.nodes.size() == 3 && out.totalWords == 12 && out.totalPackets == 6); };
    const auto rejects = [&](std::initializer_list<std::uint32_t> words, Pm4GraphFailure why) {
        reads = 0; assert(!inspectPm4IndirectGraph(snapshot(bytes(words)), reader, out, error));
        assert(error.failure == why); unchanged();
    };
    rejects({IB | 1, 0x1000, 4}, Pm4GraphFailure::Predicated); assert(reads == 0);
    rejects({0xC0003F00, 0x1000}, Pm4GraphFailure::IndirectLayout); assert(reads == 0);
    for (auto count : {0u, 0x100000u, 0xFFFFFFFFu}) {
        rejects({IB, 0x1000, count}, Pm4GraphFailure::IndirectLayout); assert(reads == 0);
    }
    for (auto address : {1u, 0x1002u, 0x20000000u, 0xFFFFFFFFu, 0x1FFFFFFCu}) {
        rejects({IB, address, 2}, Pm4GraphFailure::NonPhysicalAddress); assert(reads == 0);
    }
    rejects({IB, 0x3000, 1}, Pm4GraphFailure::ReadRejected); assert(reads == 1);
    rejects({IB, 0x1000, 1}, Pm4GraphFailure::ReadRejected);
    memory[0x3000] = bytes({0xC0001000}); // Truncated packet.
    rejects({IB, 0x3000, 1}, Pm4GraphFailure::DecodeRejected);
    assert(error.decode.failure == Pm4Failure::TruncatedPacket);
    memory[0x3000] = bytes({IB, 0x3000, 3});
    rejects({IB, 0x3000, 3}, Pm4GraphFailure::Cycle); assert(reads == 1 && error.parent == 1);
    memory[0x3000] = bytes({IB, 0x4000, 3}); memory[0x4000] = bytes({IB, 0x3000, 3});
    rejects({IB, 0x3000, 3}, Pm4GraphFailure::Cycle); assert(reads == 2 && error.parent == 2);
    for (auto limits : {Pm4GraphLimits{0,8,1024,64}, {65,8,1024,64}, {64,0,1024,64},
                       {64,9,1024,64}, {64,8,0,64}, {64,8,1048577,64}, {64,8,1024,0}, {64,8,1024,65537}}) {
        reads = 0; assert(!inspectPm4IndirectGraph(root, reader, out, error, limits));
        assert(error.failure == Pm4GraphFailure::InvalidLimits && reads == 0); unchanged();
    }
    for (auto limits : {Pm4GraphLimits{2,8,1024,64}, {64,1,1024,64}, {64,8,11,64},
                       {64,8,1024,5}, {64,8,5,64}, {64,8,1024,1}}) {
        assert(!inspectPm4IndirectGraph(root, reader, out, error, limits));
        assert(error.failure == Pm4GraphFailure::Limit); unchanged();
    }
    assert(inspectPm4IndirectGraph(root, reader, out, error, {3,2,12,6}));
    assert(!inspectPm4IndirectGraph(root, {}, out, error)); unchanged();
    assert(!inspectPm4IndirectGraph(root, [](auto, auto) -> std::optional<std::vector<std::uint8_t>> {
        throw std::runtime_error("fixture"); }, out, error));
    assert(error.failure == Pm4GraphFailure::ReaderException); unchanged();
    assert(!inspectPm4IndirectGraph(root, [](auto, auto) -> std::optional<std::vector<std::uint8_t>> {
        throw std::bad_alloc(); }, out, error));
    assert(error.failure == Pm4GraphFailure::AllocationFailure); unchanged();
    // Exact 29-bit end, physical zero and empty root are valid without aliases.
    memory[0x1FFFFFFC] = bytes({NOP}); memory[0] = bytes({NOP});
    assert(inspectPm4IndirectGraph(snapshot(bytes({IB, 0, 1, IB, 0x1FFFFFFC, 1})), reader, out, error));
    assert(out.nodes.size() == 2);
    reads = 0; assert(inspectPm4IndirectGraph(snapshot({}), reader, out, error));
    assert(out.nodes.empty() && out.totalWords == 0 && reads == 0);
    // Unknown and conditional non-IB packets remain opaque, never executed.
    assert(inspectPm4IndirectGraph(snapshot(bytes({0xC0007F01, 0xDEADBEEF})), reader, out, error));
    assert(out.nodes.empty() && out.totalWords == 2 && reads == 0);
}
