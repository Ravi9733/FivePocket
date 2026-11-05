// Headless diagnostics only: no guest GPU initialization, pipelines or draws.
// Lists supported device extensions, to check feasibility of hardware
// conditional rendering (VK_EXT_conditional_rendering) for predicated-draw
// support. Read-only Vulkan query; does not touch any guest/game state.
#include "device.h"
#include "process_limits.h"
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    if (!recomp::gpu::initializeProbeProcess()) return 2;
    recomp::gpu::ProbeDevice context;
    if (!context.inspect()) return 1;
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(context.physical, nullptr, &count, nullptr) != VK_SUCCESS) return 1;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(context.physical, nullptr, &count, extensions.data()) != VK_SUCCESS) return 1;
    bool conditionalRendering = false;
    bool transformFeedback = false;
    for (const auto& ext : extensions) {
        if (std::strcmp(ext.extensionName, "VK_EXT_conditional_rendering") == 0) conditionalRendering = true;
        if (std::strcmp(ext.extensionName, "VK_EXT_transform_feedback") == 0) transformFeedback = true;
    }
    std::printf("VULKAN_EXTENSION_LIST_V1 count=%u conditional_rendering=%d transform_feedback=%d\n",
        count, conditionalRendering ? 1 : 0, transformFeedback ? 1 : 0);
    for (const auto& ext : extensions) {
        std::printf("EXT %s %u\n", ext.extensionName, ext.specVersion);
    }
    return 0;
}
