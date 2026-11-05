#include "shader_module_pack.h"

#include <cstring>
#include <algorithm>
#include <map>
#include <new>
#include <set>
#include <string>
#include <utility>

namespace recomp::gpu {
namespace {
uint32_t little32(const uint8_t* data) {
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
        (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}
bool inspect(ShaderModule& module, ShaderRequirements& requirements) {
    const auto& words = module.words;
    if (words.size() < 5 || words[0] != 0x07230203 ||
        words[1] < 0x10000 || words[1] > 0x10300 || (words[1] & 0xff) != 0 ||
        words[3] == 0 || words[3] > 1'000'000 || words[4] != 0) return false;
    size_t entries = 0;
    struct Type { uint32_t opcode, first, second; };
    std::map<uint32_t, Type> types;
    std::map<uint32_t, uint32_t> locations;
    std::set<uint32_t> builtins;
    std::vector<std::pair<uint32_t, uint32_t>> inputs;
    for (size_t cursor = 5; cursor < words.size();) {
        const uint32_t count = words[cursor] >> 16;
        const uint32_t opcode = words[cursor] & 0xffff;
        if (count == 0 || count > words.size() - cursor) return false;
        if (opcode == 17) {
            if (count != 2) return false;
            switch (words[cursor + 1]) {
            case 1: case 50: break; // Shader / ImageQuery, core graphics profile.
            case 11: requirements.int64 = true; break;
            case 29: requirements.sampledImageArrayDynamicIndexing = true; break;
            case 32: requirements.clipDistance = true; break;
            case 5302: requirements.runtimeDescriptorArray = true; break;
            case 5347: requirements.bufferDeviceAddress = true; break;
            default: return false;
            }
        } else if (opcode == 15) {
            if (count < 4 || words[cursor + 1] != module.executionModel) return false;
            ++entries;
        } else if (opcode == 10) {
            if (count < 2 || count > 65) return false;
            std::string name;
            bool terminated = false;
            for (size_t i = cursor + 1; i < cursor + count && !terminated; ++i) {
                for (unsigned shift = 0; shift < 32; shift += 8) {
                    const char value = char((words[i] >> shift) & 0xff);
                    if (value == 0) { terminated = true; break; }
                    name.push_back(value);
                }
            }
            if (!terminated || (name != "SPV_EXT_descriptor_indexing" &&
                name != "SPV_KHR_physical_storage_buffer")) return false;
        } else if (module.executionModel == 0) {
            if (opcode == 71 && count >= 3) { // OpDecorate
                if (words[cursor + 2] == 30) {
                    if (count != 4 || !locations.emplace(words[cursor + 1], words[cursor + 3]).second)
                        return false;
                } else if (words[cursor + 2] == 11) builtins.insert(words[cursor + 1]);
            } else if (opcode == 59 && count >= 4 && words[cursor + 3] == 1) {
                inputs.emplace_back(words[cursor + 2], words[cursor + 1]);
            } else if (opcode == 21 || opcode == 22 || opcode == 23 || opcode == 32) {
                if (count != (opcode == 22 ? 3 : 4)) return false;
                if (!types.emplace(words[cursor + 1], Type{opcode, words[cursor + 2],
                    count == 4 ? words[cursor + 3] : 0}).second) return false;
            }
        }
        cursor += count;
    }
    if (entries != 1) return false;
    std::set<uint32_t> usedLocations;
    for (const auto& [id, typeId] : inputs) {
        if (builtins.count(id)) continue;
        auto location = locations.find(id);
        auto pointer = types.find(typeId);
        if (location == locations.end() || location->second >= 32 ||
            !usedLocations.insert(location->second).second || pointer == types.end() ||
            pointer->second.opcode != 32 || pointer->second.first != 1) return false;
        auto scalar = types.find(pointer->second.second);
        if (scalar == types.end()) return false;
        uint32_t components = 1;
        if (scalar->second.opcode == 23) {
            components = scalar->second.second;
            scalar = types.find(scalar->second.first);
        }
        if (components < 1 || components > 4 || scalar == types.end() || scalar->second.first != 32)
            return false;
        uint32_t kind = 0;
        if (scalar->second.opcode == 21 && scalar->second.second <= 1)
            kind = scalar->second.second ? 2 : 1;
        else if (scalar->second.opcode != 22) return false;
        module.vertexInputs.push_back({location->second, components, kind});
    }
    std::sort(module.vertexInputs.begin(), module.vertexInputs.end(),
        [](const auto& a, const auto& b) { return a.location < b.location; });
    return true;
}
}

bool parseShaderModulePack(const uint8_t* data, size_t size, ShaderModulePack& output) {
    if (!data || size < 16 || size > 64 * 1024 * 1024 ||
        std::memcmp(data, "XSPVPK1\0", 8) != 0) return false;
    try {
        ShaderModulePack candidate;
        candidate.constantBinding = little32(data + 8);
        const uint32_t count = little32(data + 12);
        if ((candidate.constantBinding != 1 && candidate.constantBinding != 2) ||
            count == 0 || count > 1024) return false;
        size_t cursor = 16;
        std::set<uint32_t> indices;
        for (uint32_t i = 0; i < count; ++i) {
            if (size - cursor < 12) return false;
            ShaderModule module;
            module.index = little32(data + cursor);
            module.executionModel = little32(data + cursor + 4);
            const uint32_t bytes = little32(data + cursor + 8);
            cursor += 12;
            if (module.index == 0 || module.index > 1024 || !indices.insert(module.index).second ||
                (module.executionModel != 0 && module.executionModel != 4) ||
                bytes < 20 || bytes % 4 || bytes > 16 * 1024 * 1024 || bytes > size - cursor)
                return false;
            module.words.reserve(bytes / 4);
            for (uint32_t j = 0; j < bytes; j += 4)
                module.words.push_back(little32(data + cursor + j));
            cursor += bytes;
            if (!inspect(module, candidate.requirements)) return false;
            candidate.modules.push_back(std::move(module));
        }
        if (cursor != size || (candidate.constantBinding == 2 &&
            (candidate.requirements.int64 || candidate.requirements.bufferDeviceAddress))) return false;
        output = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}
}
