#pragma once
#include "shader_module_pack.h"
#include "xenos_command_stream.h"

namespace recomp::gpu {
enum class ShaderLookupStatus { Found, Missing, Ambiguous, InvalidInput };
struct ShaderLookupResult {
    ShaderLookupStatus status = ShaderLookupStatus::InvalidInput;
    const ShaderModule* module = nullptr; // Borrowed until cache replacement/destruction.
    uint32_t matchingRecords = 0;
};
class ShaderMicrocodeCache {
public:
    struct Key {
        uint32_t index = 0;
        uint32_t stage = 0; // PM4: 0 vertex / 1 pixel.
        size_t moduleSlot = 0;
        std::vector<uint8_t> code;
    };
    const std::vector<Key>& keys() const { return keys_; }
    const ShaderModulePack& modules() const { return modules_; }
    // Exact byte equality avoids introducing a second hash algorithm. All
    // ambiguous translations remain ambiguous, even when the first one matches.
    ShaderLookupResult lookup(uint32_t stage, const uint8_t* code, size_t size) const;
    ShaderLookupResult lookupInline(const Pm4ShaderLoad& load) const;
private:
    std::vector<Key> keys_;
    ShaderModulePack modules_;
    friend bool parseShaderMicrocodeCache(const uint8_t*, size_t, ShaderMicrocodeCache&);
};
// Framing/association gate for explicitly selected, fully validated local
// conversion output. Not a SPIR-V validator or authentication mechanism.
bool parseShaderMicrocodeCache(const uint8_t* data, size_t size, ShaderMicrocodeCache& output);
}
