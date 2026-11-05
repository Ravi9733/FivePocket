#include "shader_microcode_cache.h"
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <utility>

namespace recomp::gpu {
namespace {
uint32_t little32(const uint8_t* data) {
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}
}
bool parseShaderMicrocodeCache(const uint8_t* data, size_t size, ShaderMicrocodeCache& output) {
    if (!data || size < 16 || size > 80 * 1024 * 1024 || std::memcmp(data, "XSCAT1\0\0", 8)) return false;
    const auto count = little32(data + 8), moduleBytes = little32(data + 12);
    if (!count || count > 1024 || moduleBytes < 16 || moduleBytes > 64 * 1024 * 1024 || moduleBytes > size - 16)
        return false;
    try {
        ShaderMicrocodeCache candidate;
        const auto end = size - moduleBytes;
        if (end - 16 > 16 * 1024 * 1024) return false;
        size_t cursor = 16;
        std::set<uint32_t> indices;
        for (uint32_t i = 0; i < count; ++i) {
            if (end - cursor < 12) return false;
            ShaderMicrocodeCache::Key key;
            key.index = little32(data + cursor);
            key.stage = little32(data + cursor + 4);
            const auto bytes = little32(data + cursor + 8);
            cursor += 12;
            if (!key.index || key.index > 1024 || !indices.insert(key.index).second || key.stage > 1 ||
                !bytes || bytes % 12 || bytes > 65535 * 4 || bytes > end - cursor) return false;
            key.code.assign(data + cursor, data + cursor + bytes);
            cursor += bytes;
            candidate.keys_.push_back(std::move(key));
        }
        if (cursor != end || !parseShaderModulePack(data + end, moduleBytes, candidate.modules_) ||
                candidate.modules_.modules.size() != count) return false;
        std::map<uint32_t, size_t> slots;
        for (size_t i = 0; i < count; ++i) slots.emplace(candidate.modules_.modules[i].index, i);
        for (auto& key : candidate.keys_) {
            const auto found = slots.find(key.index);
            if (found == slots.end() || candidate.modules_.modules[found->second].executionModel != (key.stage ? 4u : 0u))
                return false;
            key.moduleSlot = found->second;
        }
        output = std::move(candidate);
        return true;
    } catch (const std::bad_alloc&) { return false; }
}

ShaderLookupResult ShaderMicrocodeCache::lookup(uint32_t stage, const uint8_t* code, size_t size) const {
    if (stage > 1 || !code || !size || size % 12 || size > 65535 * 4) return {};
    ShaderLookupResult result{ShaderLookupStatus::Missing, nullptr, 0};
    bool ambiguous = false;
    for (const auto& key : keys_) {
        if (key.stage != stage || key.code.size() != size || std::memcmp(key.code.data(), code, size)) continue;
        const auto* module = &modules_.modules[key.moduleSlot];
        ++result.matchingRecords;
        if (!result.module) result.module = module;
        else if (result.module->words != module->words) ambiguous = true;
        else if (module->index < result.module->index) result.module = module;
    }
    if (ambiguous) return {ShaderLookupStatus::Ambiguous, nullptr, result.matchingRecords};
    if (result.module) result.status = ShaderLookupStatus::Found;
    return result;
}
ShaderLookupResult ShaderMicrocodeCache::lookupInline(const Pm4ShaderLoad& load) const {
    if (!load.immediate || load.physicalAddress || size_t(load.instructionWords) * 4 != load.inlineBigEndianBytes.size()) return {};
    return lookup(load.stage, load.inlineBigEndianBytes.data(), load.inlineBigEndianBytes.size());
}
}
