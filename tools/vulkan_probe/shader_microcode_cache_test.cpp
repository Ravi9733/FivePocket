#include "shader_microcode_cache.h"
#include <cassert>
#include <cstring>

using namespace recomp::gpu;
namespace {
void word(std::vector<uint8_t>& data, uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte) data.push_back(uint8_t(value >> (byte * 8)));
}
void replace(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte) data[offset + byte] = uint8_t(value >> (byte * 8));
}
std::vector<uint8_t> fixture(bool differentTranslation = false) {
    std::vector<uint8_t> modules{'X','S','P','V','P','K','1',0};
    word(modules, 2); word(modules, 2);
    for (uint32_t index = 1; index <= 2; ++index) {
        word(modules, index); word(modules, 0); word(modules, 48);
        // Framing fixtures only; no full validation and never passed to Vulkan.
        for (uint32_t value : {0x07230203u, 0x10300u, (differentTranslation ? index : 0u), 8u, 0u,
                (2u << 16) | 17u, 1u, (5u << 16) | 15u, 0u, 1u, 0x6e69616du, 0u}) word(modules, value);
    }
    std::vector<uint8_t> output{'X','S','C','A','T','1',0,0};
    word(output, 2); word(output, static_cast<uint32_t>(modules.size()));
    for (uint32_t index = 1; index <= 2; ++index) {
        word(output, index); word(output, 0); word(output, 12);
        for (uint8_t byte = 0; byte < 12; ++byte) output.push_back(byte);
    }
    output.insert(output.end(), modules.begin(), modules.end());
    return output;
}
}
int main() {
    ShaderMicrocodeCache cache;
    auto data = fixture();
    assert(parseShaderMicrocodeCache(data.data(), data.size(), cache));
    assert(cache.keys().size() == 2 && cache.modules().constantBinding == 2);
    auto code = cache.keys()[0].code;
    const auto found = cache.lookup(0, code.data(), code.size());
    assert(found.status == ShaderLookupStatus::Found && found.module && found.module->index == 1 && found.matchingRecords == 2);
    assert(cache.lookup(1, code.data(), code.size()).status == ShaderLookupStatus::Missing);
    assert(cache.lookup(2, code.data(), code.size()).status == ShaderLookupStatus::InvalidInput);
    assert(cache.lookup(0, nullptr, 12).status == ShaderLookupStatus::InvalidInput);
    assert(cache.lookup(0, code.data(), 11).status == ShaderLookupStatus::InvalidInput);
    code[11] ^= 1;
    assert(cache.lookup(0, code.data(), code.size()).status == ShaderLookupStatus::Missing);
    code[11] ^= 1;
    for (size_t size = 0; size < data.size(); ++size) {
        assert(!parseShaderMicrocodeCache(data.data(), size, cache));
        assert(cache.keys().size() == 2);
    }
    for (const auto& [offset, value] : std::vector<std::pair<size_t, uint32_t>>{
        {0, 0}, {8, 0}, {8, 1025}, {12, 0}, {12, 0xFFFFFFFF},
        {16, 0}, {16, 1025}, {20, 1}, {20, 2}, {24, 0}, {24, 11}, {24, 262144},
        {40, 1}, {40, 3}, {48, 0xFFFFFFFF}, {64 + 8, 3}}) {
        auto bad = data; replace(bad, offset, value);
        assert(!parseShaderMicrocodeCache(bad.data(), bad.size(), cache));
        assert(cache.lookup(0, code.data(), code.size()).status == ShaderLookupStatus::Found);
    }
    data.push_back(0);
    assert(!parseShaderMicrocodeCache(data.data(), data.size(), cache));
    data = fixture(true);
    assert(parseShaderMicrocodeCache(data.data(), data.size(), cache));
    const auto ambiguous = cache.lookup(0, code.data(), code.size());
    assert(ambiguous.status == ShaderLookupStatus::Ambiguous && !ambiguous.module && ambiguous.matchingRecords == 2);

    // Resolve exact BE bytes emitted by a real PM4 inline-load inspection.
    data = fixture();
    assert(parseShaderMicrocodeCache(data.data(), data.size(), cache));
    const std::vector<uint8_t> command{0xC0,0x04,0x2B,0x00, 0,0,0,0, 0,0,0,3,
        0,1,2,3, 4,5,6,7, 8,9,10,11};
    Pm4Snapshot snapshot;
    Pm4ShaderInspection inspection;
    Pm4Error error;
    assert(decodePm4(command.data(), command.size(), {}, snapshot, error));
    assert(inspectPm4ShaderLoads(snapshot, inspection, error) && inspection.otherPackets == 0);
    auto load = inspection.loads[0];
    assert(cache.lookupInline(load).status == ShaderLookupStatus::Found);
    load.instructionWords = 6;
    assert(cache.lookupInline(load).status == ShaderLookupStatus::InvalidInput);
    load = inspection.loads[0]; load.immediate = false; load.physicalAddress = 0x10000000;
    assert(cache.lookupInline(load).status == ShaderLookupStatus::InvalidInput); // No physical reads.
}
