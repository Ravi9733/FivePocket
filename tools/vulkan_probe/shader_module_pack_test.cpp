#include "shader_module_pack.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {
void word(std::vector<uint8_t>& output, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) output.push_back(uint8_t(value >> shift));
}
void replace(std::vector<uint8_t>& output, size_t offset, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) output[offset + shift / 8] = uint8_t(value >> shift);
}
std::vector<uint8_t> fixture() {
    std::vector<uint8_t> output{'X','S','P','V','P','K','1',0};
    word(output, 2); word(output, 1); word(output, 1); word(output, 0); word(output, 48);
    // Synthetic framing only, never submitted to Vulkan by this unit test.
    for (uint32_t value : {0x07230203u, 0x10300u, 0u, 8u, 0u,
            (2u << 16) | 17u, 1u, (5u << 16) | 15u, 0u, 1u, 0x6e69616du, 0u}) word(output, value);
    return output;
}
}
int main() {
    const auto valid = fixture();
    recomp::gpu::ShaderModulePack pack;
    assert(recomp::gpu::parseShaderModulePack(valid.data(), valid.size(), pack));
    assert(pack.modules.size() == 1 && pack.constantBinding == 2 && !pack.requirements.int64);
    for (size_t size = 0; size < valid.size(); ++size)
        assert(!recomp::gpu::parseShaderModulePack(valid.data(), size, pack));
    assert(pack.modules.size() == 1); // Failed reads preserve existing output.
    for (auto change : std::vector<std::pair<size_t, uint32_t>>{
            {8, 3}, {12, 0}, {12, 1025}, {16, 0}, {20, 5}, {24, 47}, {24, 0xffffffffu},
            {28, 0}, {32, 0x10400}, {40, 0}, {44, 1}, {48, 17}, {52, 12}, {60, 4}}) {
        auto bad = valid;
        replace(bad, change.first, change.second);
        assert(!recomp::gpu::parseShaderModulePack(bad.data(), bad.size(), pack));
    }
    auto wide = valid;
    replace(wide, 52, 11);
    assert(!recomp::gpu::parseShaderModulePack(wide.data(), wide.size(), pack));
    replace(wide, 8, 1);
    assert(recomp::gpu::parseShaderModulePack(wide.data(), wide.size(), pack));
    assert(pack.requirements.int64);
    auto dynamic = valid;
    replace(dynamic, 52, 29);
    assert(recomp::gpu::parseShaderModulePack(dynamic.data(), dynamic.size(), pack));
    assert(pack.requirements.sampledImageArrayDynamicIndexing && !pack.requirements.int64);
    // Unsupported non-uniform indexing is not silently covered by the core flag.
    replace(dynamic, 52, 5307);
    assert(!recomp::gpu::parseShaderModulePack(dynamic.data(), dynamic.size(), pack));
    assert(pack.requirements.sampledImageArrayDynamicIndexing);
    replace(dynamic, 52, 5302);
    assert(recomp::gpu::parseShaderModulePack(dynamic.data(), dynamic.size(), pack));
    assert(pack.requirements.runtimeDescriptorArray && !pack.requirements.sampledImageArrayDynamicIndexing);
    auto trailing = valid;
    trailing.push_back(0);
    assert(!recomp::gpu::parseShaderModulePack(trailing.data(), trailing.size(), pack));
    assert(!recomp::gpu::parseShaderModulePack(nullptr, 16, pack));
    auto vertex = valid;
    for (uint32_t value : {(3u << 16) | 22u, 2u, 32u, (4u << 16) | 23u, 3u, 2u, 4u,
            (4u << 16) | 32u, 4u, 1u, 3u, (4u << 16) | 59u, 4u, 5u, 1u,
            (4u << 16) | 71u, 5u, 30u, 17u}) word(vertex, value);
    replace(vertex, 24, static_cast<uint32_t>(vertex.size() - 28));
    assert(recomp::gpu::parseShaderModulePack(vertex.data(), vertex.size(), pack));
    assert(pack.modules[0].vertexInputs.size() == 1);
    assert(pack.modules[0].vertexInputs[0].location == 17);
    assert(pack.modules[0].vertexInputs[0].components == 4);
    assert(pack.modules[0].vertexInputs[0].kind == 0);
    replace(vertex, vertex.size() - 4, 32);
    assert(!recomp::gpu::parseShaderModulePack(vertex.data(), vertex.size(), pack));
}
