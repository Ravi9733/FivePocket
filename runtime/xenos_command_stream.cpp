#include "xenos_command_stream.h"

#include <new>
#include <utility>

namespace recomp::gpu {
namespace {
constexpr uint32_t HardMaxWords = 4 * 1024 * 1024;
constexpr uint32_t HardMaxPackets = 1024 * 1024;
bool fail(Pm4Error& error, Pm4Failure failure, uint32_t offset = 0, uint32_t opcode = 0) {
    error = {failure, offset, opcode};
    return false;
}
bool validLimits(const Pm4Limits& limits) {
    return limits.maxWords > 0 && limits.maxWords <= HardMaxWords &&
        limits.maxPackets > 0 && limits.maxPackets <= HardMaxPackets;
}
uint32_t big32(const uint8_t* bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
        (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}
bool parse(const std::vector<uint32_t>& words, const Pm4Limits& limits,
           std::vector<Pm4Packet>& output, Pm4Error& error) {
    uint32_t cursor = 0;
    while (cursor < words.size()) {
        if (output.size() == limits.maxPackets) return fail(error, Pm4Failure::PacketLimit, cursor);
        Pm4Packet packet;
        packet.offsetWords = cursor;
        const uint32_t header = words[cursor++];
        packet.kind = static_cast<Pm4Kind>(header >> 30);
        if (header == 0 && limits.zeroWordIsPadding) packet.kind = Pm4Kind::ZeroPadding;
        switch (packet.kind) {
        case Pm4Kind::RegisterSequence:
            packet.payloadWords = ((header >> 16) & 0x3fff) + 1;
            packet.firstRegister = header & 0x7fff;
            packet.repeatRegister = (header & 0x8000) != 0;
            break;
        case Pm4Kind::RegisterPair:
            packet.payloadWords = 2;
            packet.firstRegister = header & 0x7ff;
            packet.secondRegister = (header >> 11) & 0x7ff;
            break;
        case Pm4Kind::Command:
            packet.payloadWords = ((header >> 16) & 0x3fff) + 1;
            packet.opcode = (header >> 8) & 0x7f;
            packet.predicated = (header & 1) != 0;
            break;
        case Pm4Kind::Filler: case Pm4Kind::ZeroPadding: break;
        }
        if (packet.payloadWords > words.size() - cursor)
            return fail(error, Pm4Failure::TruncatedPacket, packet.offsetWords, packet.opcode);
        cursor += packet.payloadWords;
        output.push_back(packet);
    }
    error = {};
    return true;
}
}

bool decodePm4(const uint8_t* bytes, size_t size, const Pm4Limits& limits,
               Pm4Snapshot& output, Pm4Error& error) {
    if ((!bytes && size) || size % 4 || !validLimits(limits)) return fail(error, Pm4Failure::InvalidInput);
    if (size / 4 > limits.maxWords) return fail(error, Pm4Failure::WordLimit);
    try {
        Pm4Snapshot candidate;
        candidate.words_.reserve(size / 4);
        for (size_t offset = 0; offset < size; offset += 4) candidate.words_.push_back(big32(bytes + offset));
        if (!parse(candidate.words_, limits, candidate.packets_, error)) return false;
        output = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) { return fail(error, Pm4Failure::AllocationFailure); }
}

bool decodePm4Ring(const uint8_t* ring, size_t ringBytes, uint32_t readWord,
                   uint32_t writeWord, const Pm4Limits& limits,
                   Pm4Snapshot& output, Pm4Error& error) {
    if (!ring || ringBytes < 4 || ringBytes > size_t(HardMaxWords) * 4 ||
        (ringBytes & (ringBytes - 1)) || readWord >= ringBytes / 4 || writeWord >= ringBytes / 4 ||
        !validLimits(limits)) return fail(error, Pm4Failure::InvalidInput);
    const uint32_t ringWords = static_cast<uint32_t>(ringBytes / 4);
    const uint32_t count = (writeWord - readWord) & (ringWords - 1);
    if (count > limits.maxWords) return fail(error, Pm4Failure::WordLimit);
    try {
        Pm4Snapshot candidate;
        candidate.words_.reserve(count);
        for (uint32_t offset = 0; offset < count; ++offset) {
            const uint32_t index = (readWord + offset) & (ringWords - 1);
            candidate.words_.push_back(big32(ring + size_t(index) * 4));
        }
        if (!parse(candidate.words_, limits, candidate.packets_, error)) return false;
        output = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) { return fail(error, Pm4Failure::AllocationFailure); }
}

bool pm4RingBytes(uint32_t controlSizeLog2, uint32_t& outputBytes) {
    if (controlSizeLog2 > 21) return false;
    outputBytes = uint32_t(1) << (controlSizeLog2 + 3);
    return true;
}

bool inspectPm4ShaderLoads(const Pm4Snapshot& snapshot, Pm4ShaderInspection& output,
                           Pm4Error& error, uint32_t maximumLoads) {
    if (maximumLoads == 0 || maximumLoads > 4096) return fail(error, Pm4Failure::InvalidInput);
    try {
        Pm4ShaderInspection candidate;
        for (const auto& packet : snapshot.packets()) {
            if (packet.kind != Pm4Kind::Command || (packet.opcode != 0x27 && packet.opcode != 0x2b)) {
                ++candidate.otherPackets;
                continue;
            }
            if (packet.predicated)
                return fail(error, Pm4Failure::PredicatedCommand, packet.offsetWords, packet.opcode);
            if (candidate.loads.size() == maximumLoads)
                return fail(error, Pm4Failure::ShaderLoadLimit, packet.offsetWords, packet.opcode);
            if (packet.payloadWords < 2)
                return fail(error, Pm4Failure::ShaderLayout, packet.offsetWords, packet.opcode);
            const auto data = packet.offsetWords + 1;
            const auto source = snapshot.words()[data];
            const auto startSize = snapshot.words()[data + 1];
            Pm4ShaderLoad load;
            load.packetOffsetWords = packet.offsetWords;
            load.immediate = packet.opcode == 0x2b;
            load.stage = load.immediate ? source : source & 3;
            load.instructionWords = startSize & 0xffff;
            if (load.stage > 1 || (startSize >> 16) != 0 || !load.instructionWords || load.instructionWords % 3)
                return fail(error, Pm4Failure::ShaderLayout, packet.offsetWords, packet.opcode);
            if (load.immediate) {
                if (packet.payloadWords != load.instructionWords + 2)
                    return fail(error, Pm4Failure::ShaderLayout, packet.offsetWords, packet.opcode);
                load.inlineBigEndianBytes.reserve(size_t(load.instructionWords) * 4);
                for (uint32_t i = 0; i < load.instructionWords; ++i) {
                    const auto word = snapshot.words()[data + 2 + i];
                    for (unsigned byte = 0; byte < 4; ++byte)
                        load.inlineBigEndianBytes.push_back(uint8_t(word >> ((3 - byte) * 8)));
                }
            } else {
                load.physicalAddress = source & ~3u;
                if (packet.payloadWords != 2 || uint64_t(load.physicalAddress) + uint64_t(load.instructionWords) * 4 >
                        (uint64_t(1) << 32))
                    return fail(error, Pm4Failure::ShaderLayout, packet.offsetWords, packet.opcode);
            }
            candidate.loads.push_back(std::move(load));
        }
        output = std::move(candidate);
        error = {};
        return true;
    } catch (const std::bad_alloc&) { return fail(error, Pm4Failure::AllocationFailure); }
}

bool inspectPm4DrawInitiators(const Pm4Snapshot& snapshot, std::vector<Pm4DrawInitiator>& output,
                             Pm4Error& error, uint32_t maximumDraws) {
    if (!maximumDraws || maximumDraws > 1024) return fail(error, Pm4Failure::InvalidInput);
    try {
        std::vector<Pm4DrawInitiator> candidate;
        for (const auto& packet : snapshot.packets()) {
            if (packet.kind != Pm4Kind::Command || (packet.opcode != 0x22 && packet.opcode != 0x36)) continue;
            const auto prefix = packet.opcode == 0x22 ? 1u : 0u;
            if (packet.payloadWords <= prefix) return fail(error, Pm4Failure::DrawLayout, packet.offsetWords, packet.opcode);
            if (candidate.size() == maximumDraws) return fail(error, Pm4Failure::DrawLimit, packet.offsetWords, packet.opcode);
            const auto control = snapshot.words()[packet.offsetWords + 1 + prefix];
            candidate.push_back({packet.offsetWords, packet.opcode, control >> 16, control & 63,
                (control >> 6) & 3, (control >> 8) & 3, bool(control & 0x800), bool(control & 0x1000),
                prefix && snapshot.words()[packet.offsetWords + 1] != 0, packet.predicated});
        }
        output = std::move(candidate); error = {}; return true;
    } catch (const std::bad_alloc&) { return fail(error, Pm4Failure::AllocationFailure); }
}

bool planPm4RegisterWrites(const Pm4Snapshot& snapshot, uint32_t registerCount,
                           std::vector<RegisterWrite>& output, Pm4Error& error) {
    if (registerCount == 0 || registerCount > 65536) return fail(error, Pm4Failure::InvalidInput);
    try {
        std::vector<RegisterWrite> candidate;
        for (const auto& packet : snapshot.packets()) {
            const uint32_t dataOffset = packet.offsetWords + 1;
            uint32_t count = packet.payloadWords;
            uint32_t base = packet.firstRegister;
            uint32_t skip = 0;
            if (packet.kind == Pm4Kind::Filler || packet.kind == Pm4Kind::ZeroPadding) continue;
            if (packet.kind == Pm4Kind::Command) {
                if (packet.predicated)
                    return fail(error, Pm4Failure::PredicatedCommand, packet.offsetWords, packet.opcode);
                if (packet.opcode == 0x10) continue; // NOP: no payload interpretation.
                if (packet.opcode != 0x2d && packet.opcode != 0x55 && packet.opcode != 0x56)
                    return fail(error, Pm4Failure::UnsupportedOpcode, packet.offsetWords, packet.opcode);
                const uint32_t selector = snapshot.words()[dataOffset];
                if (packet.opcode == 0x2d) {
                    const uint32_t type = (selector >> 16) & 0xff;
                    if ((selector & ~0x00ff07ffu) || type > 4)
                        return fail(error, Pm4Failure::UnsupportedConstantSelector, packet.offsetWords, packet.opcode);
                    constexpr uint32_t bases[]{0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
                    base = bases[type] + (selector & 0x7ff);
                } else {
                    if (selector & 0xffff0000u)
                        return fail(error, Pm4Failure::UnsupportedConstantSelector, packet.offsetWords, packet.opcode);
                    base = selector;
                }
                skip = 1;
                --count;
            }
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t target = packet.kind == Pm4Kind::RegisterPair ?
                    (i == 0 ? packet.firstRegister : packet.secondRegister) :
                    (packet.repeatRegister ? base : base + i);
                if (target >= registerCount)
                    return fail(error, Pm4Failure::RegisterRange, packet.offsetWords, packet.opcode);
                candidate.push_back({packet.offsetWords, target, snapshot.words()[dataOffset + skip + i]});
            }
        }
        output = std::move(candidate);
        error = {};
        return true;
    } catch (const std::bad_alloc&) { return fail(error, Pm4Failure::AllocationFailure); }
}
}
