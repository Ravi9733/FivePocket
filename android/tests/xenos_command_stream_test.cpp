#include "xenos_command_stream.h"
#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

using namespace recomp::gpu;
namespace {
std::vector<uint8_t> bytes(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> result;
    for (const uint32_t word : words)
        for (int shift = 24; shift >= 0; shift -= 8) result.push_back(uint8_t(word >> shift));
    return result;
}
Pm4Snapshot decode(const std::vector<uint32_t>& words, Pm4Limits limits = {}) {
    const auto data = bytes(words);
    Pm4Snapshot result;
    Pm4Error error;
    assert(decodePm4(data.data(), data.size(), limits, result, error));
    assert(error.failure == Pm4Failure::None);
    return result;
}
uint32_t type3(uint32_t opcode, uint32_t count, bool predicated = false) {
    return 0xc0000000u | ((count - 1) << 16) | (opcode << 8) | uint32_t(predicated);
}

void testShaderLoadInspection() {
    Pm4Error error;
    Pm4ShaderInspection inspection;
    const auto inlineCode = bytes({0x01234567, 0x89ABCDEF, 0x10203040});
    auto snapshot = decode({type3(0x27, 2), 0x10000000, 6,
        type3(0x2b, 5), 1, 3, 0x01234567, 0x89ABCDEF, 0x10203040,
        type3(0x3f, 1), 0xDEADBEEF, 0x80000000});
    assert(inspectPm4ShaderLoads(snapshot, inspection, error));
    assert(inspection.loads.size() == 2 && inspection.otherPackets == 2);
    assert(inspection.loads[0].stage == 0 && !inspection.loads[0].immediate);
    assert(inspection.loads[0].instructionWords == 6 && inspection.loads[0].physicalAddress == 0x10000000);
    assert(inspection.loads[0].inlineBigEndianBytes.empty());
    assert(inspection.loads[1].stage == 1 && inspection.loads[1].immediate);
    assert(inspection.loads[1].packetOffsetWords == 3 && inspection.loads[1].physicalAddress == 0);
    assert(inspection.loads[1].inlineBigEndianBytes == inlineCode);
    const auto original = inspection;
    assert(!inspectPm4ShaderLoads(snapshot, inspection, error, 1));
    assert(error.failure == Pm4Failure::ShaderLoadLimit);
    for (uint32_t limit : {0u, 4097u, 0xFFFFFFFFu})
        assert(!inspectPm4ShaderLoads(snapshot, inspection, error, limit));
    for (auto bad : std::vector<std::vector<uint32_t>>{
        {type3(0x27, 1), 0}, {type3(0x27, 3), 0, 3, 0},
        {type3(0x27, 2), 2, 3}, {type3(0x27, 2), 3, 3},
        {type3(0x27, 2), 0, 0x10003}, {type3(0x27, 2), 0, 0},
        {type3(0x27, 2), 0, 4}, {type3(0x27, 2), 0xFFFFFFF8, 3},
        {type3(0x2b, 5), 4, 3, 0, 0, 0}, {type3(0x2b, 5), 0, 0x10003, 0, 0, 0},
        {type3(0x2b, 4), 0, 3, 0, 0}, {type3(0x2b, 6), 0, 3, 0, 0, 0, 0},
        {type3(0x2b, 2), 0, 0}, {type3(0x27, 2, true), 0, 3}}) {
        assert(!inspectPm4ShaderLoads(decode(bad), inspection, error));
        assert(inspection.loads.size() == original.loads.size() && inspection.otherPackets == original.otherPackets);
        assert(inspection.loads[1].inlineBigEndianBytes == inlineCode);
    }
    // Both stages and the largest complete-instruction external extent. This
    // validates arithmetic only; it never attempts to map physical memory.
    for (uint32_t stage : {0u, 1u}) {
        assert(inspectPm4ShaderLoads(decode({type3(0x27, 2), 0x10000000 | stage, 65535}), inspection, error));
        assert(inspection.loads[0].stage == stage && inspection.loads[0].instructionWords == 65535);
    }
    // Inline bytes can cross a captured ring boundary; they remain independently owned.
    const auto ring = bytes({1, 3, 0x01234567, 0x89ABCDEF, 0x10203040, 0, 0, type3(0x2b, 5)});
    assert(decodePm4Ring(ring.data(), ring.size(), 7, 5, {}, snapshot, error));
    assert(inspectPm4ShaderLoads(snapshot, inspection, error));
    snapshot = {};
    assert(inspection.loads[0].inlineBigEndianBytes == inlineCode);
}
}
int main() {
    testShaderLoadInspection();
    Pm4Error error;
    std::vector<RegisterWrite> writes;
    auto snapshot = decode({0x00010010, 0x12345678, 0xabcdef01,
        0x00018020, 5, 6, 0x40000000u | 3u | (9u << 11), 7, 8, 0x80000000});
    assert(snapshot.packets().size() == 4);
    assert(planPm4RegisterWrites(snapshot, 0x5003, writes, error));
    assert(writes.size() == 6);
    assert(writes[0].index == 0x10 && writes[0].value == 0x12345678);
    assert(writes[1].index == 0x11 && writes[1].value == 0xabcdef01);
    assert(writes[2].index == 0x20 && writes[3].index == 0x20);
    assert(writes[4].index == 3 && writes[5].index == 9);
    assert(writes[5].value == 8);
    auto source = bytes({0x00000012, 99});
    assert(decodePm4(source.data(), source.size(), {}, snapshot, error));
    source[7] = 0;
    assert(snapshot.words()[1] == 99); // Owns input instead of borrowing mutable guest bytes.

    for (uint32_t type = 0; type < 5; ++type) {
        snapshot = decode({type3(0x2d, 3), (type << 16) | 2, 11, 12});
        assert(planPm4RegisterWrites(snapshot, 0x5003, writes, error));
        constexpr uint32_t bases[]{0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
        assert(writes.size() == 2 && writes[0].index == bases[type] + 2);
        assert(writes[1].index == bases[type] + 3 && writes[1].value == 12);
    }
    for (uint32_t opcode : {0x55u, 0x56u}) {
        snapshot = decode({type3(opcode, 2), 0x4908, 0x11223344});
        assert(planPm4RegisterWrites(snapshot, 0x5003, writes, error));
        assert(writes.size() == 1 && writes[0].index == 0x4908 && writes[0].value == 0x11223344);
    }
    snapshot = decode({0x00000020, 8, type3(0x3f, 2), 0x10000000, 0xffffffff});
    const auto original = writes;
    assert(!planPm4RegisterWrites(snapshot, 0x5003, writes, error));
    assert(error.failure == Pm4Failure::UnsupportedOpcode && error.offsetWords == 2 && error.opcode == 0x3f);
    assert(writes.size() == original.size() && writes[0].value == original[0].value);
    // Every unsupported command is opaque to decoding, but rejected by the pure planner.
    for (uint32_t opcode = 0; opcode < 128; ++opcode) {
        if (opcode == 0x10 || opcode == 0x2d || opcode == 0x55 || opcode == 0x56) continue;
        snapshot = decode({type3(opcode, 1), 0xdeadbeef});
        assert(!planPm4RegisterWrites(snapshot, 0x5003, writes, error));
        assert(error.failure == Pm4Failure::UnsupportedOpcode);
    }
    snapshot = decode({type3(0x55, 2, true), 4, 42});
    assert(!planPm4RegisterWrites(snapshot, 0x5003, writes, error));
    assert(error.failure == Pm4Failure::PredicatedCommand);
    for (uint32_t selector : {0x50000u, 0x800u, 0x80000000u}) {
        snapshot = decode({type3(0x2d, 2), selector, 42});
        assert(!planPm4RegisterWrites(snapshot, 0x5003, writes, error));
        assert(error.failure == Pm4Failure::UnsupportedConstantSelector);
    }
    snapshot = decode({type3(0x55, 3), 0x5002, 1, 2});
    assert(!planPm4RegisterWrites(snapshot, 0x5003, writes, error));
    assert(error.failure == Pm4Failure::RegisterRange);

    // Boundary payload counts, including the full 14-bit count field.
    for (uint32_t count : {1u, 2u, 16383u, 16384u}) {
        std::vector<uint32_t> commands(count + 1, 0x80000000);
        commands[0] = type3(0x10, count);
        snapshot = decode(commands);
        assert(snapshot.packets().size() == 1 && snapshot.packets()[0].payloadWords == count);
        const auto data = bytes(commands);
        Pm4Snapshot unchanged = snapshot;
        assert(!decodePm4(data.data(), data.size() - 4, {}, unchanged, error));
        assert(error.failure == Pm4Failure::TruncatedPacket);
        assert(unchanged.words().size() == snapshot.words().size());
    }
    auto malformed = bytes({0x40000000, 1});
    assert(!decodePm4(malformed.data(), malformed.size(), {}, snapshot, error));
    assert(error.failure == Pm4Failure::TruncatedPacket);
    assert(!decodePm4(nullptr, 4, {}, snapshot, error));
    assert(!decodePm4(malformed.data(), 3, {}, snapshot, error));
    assert(!decodePm4(malformed.data(), malformed.size(), {1, 1, false}, snapshot, error));
    assert(error.failure == Pm4Failure::WordLimit);
    malformed = bytes({0x80000000, 0x80000000});
    assert(!decodePm4(malformed.data(), malformed.size(), {2, 1, false}, snapshot, error));
    assert(error.failure == Pm4Failure::PacketLimit);
    snapshot = decode({0, 41});
    assert(snapshot.packets()[0].kind == Pm4Kind::RegisterSequence);
    snapshot = decode({0, 0x80000000}, {100, 100, true});
    assert(snapshot.packets()[0].kind == Pm4Kind::ZeroPadding && snapshot.packets().size() == 2);

    // A packet crosses the physical end of an eight-word ring.
    const auto ring = bytes({0x76543210, 0xabcdef01, 0x80000000, 0, 0, 0, 0, 0x00010020});
    assert(decodePm4Ring(ring.data(), ring.size(), 7, 3, {}, snapshot, error));
    assert(snapshot.packets().size() == 2 && snapshot.words().size() == 4);
    assert(planPm4RegisterWrites(snapshot, 0x5003, writes, error));
    assert(writes.size() == 2 && writes[0].value == 0x76543210 && writes[1].value == 0xabcdef01);
    assert(!decodePm4Ring(ring.data(), ring.size(), 7, 1, {}, snapshot, error));
    assert(error.failure == Pm4Failure::TruncatedPacket);
    assert(!decodePm4Ring(ring.data(), 28, 0, 1, {}, snapshot, error));
    assert(!decodePm4Ring(ring.data(), ring.size(), 8, 1, {}, snapshot, error));
    assert(decodePm4Ring(ring.data(), ring.size(), 7, 7, {}, snapshot, error));
    assert(snapshot.words().empty());
    uint32_t ringSize = 0;
    for (uint32_t field = 0; field <= 21; ++field) {
        assert(pm4RingBytes(field, ringSize));
        assert(ringSize == (uint32_t(1) << (field + 3)));
    }
    ringSize = 77;
    for (uint32_t field : {22u, 31u, std::numeric_limits<uint32_t>::max()}) {
        assert(!pm4RingBytes(field, ringSize));
        assert(ringSize == 77);
    }
    // Deterministic malformed-stream property sweep, with bounded allocations.
    uint32_t random = 0x8f3416a5;
    auto next = [&]() {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        return random;
    };
    for (size_t iteration = 0; iteration < 10000; ++iteration) {
        std::vector<uint32_t> stream;
        for (uint32_t n = 0, count = next() & 31; n < count; ++n) stream.push_back(next());
        const auto input = bytes(stream);
        const auto before = snapshot.words();
        if (!decodePm4(input.data(), input.size(), {32, 32, false}, snapshot, error)) {
            assert(snapshot.words() == before);
            continue;
        }
        size_t consumed = 0;
        for (const auto& packet : snapshot.packets()) {
            assert(packet.offsetWords == consumed);
            consumed += 1 + packet.payloadWords;
            assert(consumed <= snapshot.words().size());
        }
        assert(consumed == snapshot.words().size());
        if (planPm4RegisterWrites(snapshot, 0x5003, writes, error))
            for (const auto& write : writes) assert(write.index < 0x5003);
    }
}
