// Headless diagnostics only: no guest GPU initialization, pipelines or draws.
#include "device.h"
#include "private_input.h"
#include "process_limits.h"
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (!recomp::gpu::initializeProbeProcess()) return 2;
    if (argc != 1 && (argc != 3 || std::strcmp(argv[1], "--modules") != 0)) return 2;
    recomp::gpu::ShaderModulePack pack;
    if (argc == 3) {
        std::vector<uint8_t> bytes;
        if (!recomp::gpu::readInput(argv[2], bytes, 64 * 1024 * 1024) ||
            !recomp::gpu::parseShaderModulePack(bytes.data(), bytes.size(), pack)) {
            std::puts("VULKAN_PROBE_REJECTED invalid-pack");
            return 2;
        }
    }
    recomp::gpu::ProbeDevice context;
    if (!context.inspect()) return 1;
    if (argc == 1) return 0;
    if (!context.create(pack.requirements)) return 3;
    size_t created = 0;
    for (const auto& module : pack.modules) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = module.words.size() * sizeof(uint32_t);
        info.pCode = module.words.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        if (!recomp::gpu::checkVulkan(vkCreateShaderModule(context.device, &info, nullptr, &shader),
                                     "create-shader-module")) return 1;
        ++created;
        vkDestroyShaderModule(context.device, shader, nullptr);
    }
    std::printf("VULKAN_MODULES_V1 created=%zu total=%zu binding=%u pipelines=0 draws=0\n",
        created, pack.modules.size(), pack.constantBinding);
    return 0;
}
