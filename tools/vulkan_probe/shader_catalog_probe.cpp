// Offline-artifact lookup diagnostic, not guest execution or GPU submission.
#include "shader_microcode_cache.h"
#include "private_input.h"
#include "process_limits.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (!recomp::gpu::initializeProbeProcess() || argc != 2) return 2;
    std::vector<uint8_t> bytes;
    recomp::gpu::ShaderMicrocodeCache cache;
    if (!recomp::gpu::readInput(argv[1], bytes, 80 * 1024 * 1024) ||
            !recomp::gpu::parseShaderMicrocodeCache(bytes.data(), bytes.size(), cache)) return 2;
    size_t found = 0, ambiguous = 0;
    for (const auto& key : cache.keys()) {
        const auto result = cache.lookup(key.stage, key.code.data(), key.code.size());
        if (result.status == recomp::gpu::ShaderLookupStatus::Ambiguous) { ++ambiguous; continue; }
        if (result.status != recomp::gpu::ShaderLookupStatus::Found || !result.module) return 1;
        ++found;
    }
    std::printf("SHADER_CATALOG_V1 records=%zu found=%zu ambiguous=%zu physical_reads=0 draws=0\n",
        cache.keys().size(), found, ambiguous);
    return 0;
}
