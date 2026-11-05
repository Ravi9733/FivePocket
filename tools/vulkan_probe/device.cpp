#include "device.h"
#include <cstdio>
#include <vector>

namespace recomp::gpu {
bool checkVulkan(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) return true;
    std::printf("VULKAN_PROBE_ERROR %s %d\n", operation, static_cast<int>(result));
    return false;
}
ProbeDevice::~ProbeDevice() {
    if (pipelineCache) vkDestroyPipelineCache(device, pipelineCache, nullptr);
    if (device) vkDestroyDevice(device, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
}
bool ProbeDevice::inspect() {
    if (instance) return false;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Recomp headless probe";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    if (!checkVulkan(vkCreateInstance(&info, nullptr, &instance), "create-instance")) return false;
    const auto features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
    if (!features2) return false;
    uint32_t count = 0;
    if (!checkVulkan(vkEnumeratePhysicalDevices(instance, &count, nullptr), "device-count") ||
            count == 0 || count > 16) return false;
    std::vector<VkPhysicalDevice> devices(count);
    if (!checkVulkan(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "devices")) return false;
    // Explicit diagnostic limitation: use device zero, no heuristic GPU choice.
    physical = devices[0];
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_2) return false;
    // Keep driver identity with each hardware result. Vendor-specific driver
    // versions are raw integers, not assumed Vulkan semantic versions.
    const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
    if (!properties2) return false;
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 allProperties{};
    allProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    allProperties.pNext = &driver;
    properties2(physical, &allProperties);
    std::printf("VULKAN_DRIVER_V1 device_name=%.*s driver_version_raw=%u driver_id=%u "
        "driver_name=%.*s driver_info=%.*s conformance=%u.%u.%u.%u\n",
        VK_MAX_PHYSICAL_DEVICE_NAME_SIZE, properties.deviceName, properties.driverVersion,
        static_cast<unsigned>(driver.driverID), VK_MAX_DRIVER_NAME_SIZE, driver.driverName,
        VK_MAX_DRIVER_INFO_SIZE, driver.driverInfo, driver.conformanceVersion.major,
        driver.conformanceVersion.minor, driver.conformanceVersion.subminor,
        driver.conformanceVersion.patch);
    indexing_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    addresses_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    addresses_.pNext = &indexing_;
    VkPhysicalDeviceFeatures2 all{};
    all.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    all.pNext = &addresses_;
    features2(physical, &all);
    features_ = all.features;
    std::printf("VULKAN_PROBE_V1 device=0 api=%u vendor=%u device_id=%u "
        "shaderInt64=%u shaderClipDistance=%u bufferDeviceAddress=%u runtimeDescriptorArray=%u "
        "sampledImageArrayDynamicIndexing=%u "
        "sampledImageNonUniformIndexing=%u descriptorBindingPartiallyBound=%u "
        "maxPushConstantsSize=%u maxBoundDescriptorSets=%u\n",
        properties.apiVersion, properties.vendorID, properties.deviceID,
        features_.shaderInt64, features_.shaderClipDistance, addresses_.bufferDeviceAddress,
        indexing_.runtimeDescriptorArray, features_.shaderSampledImageArrayDynamicIndexing,
        indexing_.shaderSampledImageArrayNonUniformIndexing,
        indexing_.descriptorBindingPartiallyBound, properties.limits.maxPushConstantsSize,
        properties.limits.maxBoundDescriptorSets);
    // Report support separately from device creation. These bits are NOT
    // enabled by this query and do not certify a third-party runtime or shader.
    std::printf("VULKAN_CORE_CAPS_V1 independentBlend=%u fragmentStoresAndAtomics=%u "
        "vertexPipelineStoresAndAtomics=%u geometryShader=%u fillModeNonSolid=%u "
        "maxStorageBufferRange=%u maxPerStageDescriptorStorageBuffers=%u\n",
        features_.independentBlend, features_.fragmentStoresAndAtomics,
        features_.vertexPipelineStoresAndAtomics, features_.geometryShader,
        features_.fillModeNonSolid, properties.limits.maxStorageBufferRange,
        properties.limits.maxPerStageDescriptorStorageBuffers);
    return true;
}
bool ProbeDevice::create(const ShaderRequirements& requirements) {
    return create(requirements,false);
}
bool ProbeDevice::create(const ShaderRequirements& requirements,bool samplerAnisotropy) {
    return create(requirements,samplerAnisotropy,false);
}
bool ProbeDevice::create(const ShaderRequirements& requirements,bool samplerAnisotropy,bool sampleRateShading) {
    return create(requirements,samplerAnisotropy,sampleRateShading,false);
}
bool ProbeDevice::create(const ShaderRequirements& requirements,bool samplerAnisotropy,bool sampleRateShading,bool independentBlend) {
    if (!physical || device) return false;
    // Conservative descriptor-heap profile: a runtime array capability does not
    // imply that its dynamically uniform accesses declare capability 29. Those
    // accesses still require the core feature to be explicitly enabled. This
    // does not authorize non-uniform indexing (unsupported by our module gate).
    const bool dynamicImages = requirements.sampledImageArrayDynamicIndexing ||
        requirements.runtimeDescriptorArray;
    if ((requirements.int64 && !features_.shaderInt64) ||
        (requirements.clipDistance && !features_.shaderClipDistance) ||
        (dynamicImages && !features_.shaderSampledImageArrayDynamicIndexing) ||
        (requirements.runtimeDescriptorArray && !indexing_.runtimeDescriptorArray) ||
        (requirements.bufferDeviceAddress && !addresses_.bufferDeviceAddress) ||
        (requirements.fragmentStoresAndAtomics && !features_.fragmentStoresAndAtomics) ||
        (requirements.vertexStoresAndAtomics && !features_.vertexPipelineStoresAndAtomics) ||
        (samplerAnisotropy && !features_.samplerAnisotropy) ||
        (sampleRateShading && !features_.sampleRateShading) ||
        (independentBlend && !features_.independentBlend)) {
        std::puts("VULKAN_PROBE_UNSUPPORTED required-shader-feature");
        return false;
    }
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    if (count == 0 || count > 256) return false;
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (queues[i].queueCount &&
            (queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            queueFamily = i; found = true; break;
        }
    }
    if (!found) return false;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderInt64 = requirements.int64;
    enabled.shaderClipDistance = requirements.clipDistance;
    enabled.shaderSampledImageArrayDynamicIndexing = dynamicImages;
    enabled.fragmentStoresAndAtomics = requirements.fragmentStoresAndAtomics;
    enabled.vertexPipelineStoresAndAtomics = requirements.vertexStoresAndAtomics;
    enabled.samplerAnisotropy = samplerAnisotropy;
    enabled.sampleRateShading = sampleRateShading;
    enabled.independentBlend = independentBlend;
    VkPhysicalDeviceDescriptorIndexingFeatures indexing{};
    indexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    indexing.runtimeDescriptorArray = requirements.runtimeDescriptorArray;
    VkPhysicalDeviceBufferDeviceAddressFeatures addresses{};
    addresses.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    addresses.bufferDeviceAddress = requirements.bufferDeviceAddress;
    addresses.pNext = &indexing;
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &addresses;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.pEnabledFeatures = &enabled;
    if (!checkVulkan(vkCreateDevice(physical, &info, nullptr, &device), "create-device")) return false;
    fragmentStoresAndAtomicsEnabled = enabled.fragmentStoresAndAtomics;
    vertexStoresAndAtomicsEnabled = enabled.vertexPipelineStoresAndAtomics;
    samplerAnisotropyEnabled = enabled.samplerAnisotropy;
    sampleRateShadingEnabled = enabled.sampleRateShading;
    independentBlendEnabled = enabled.independentBlend;
    std::printf("VULKAN_MRT_POLICY independent_blend_enabled=%u\n",enabled.independentBlend);
    std::printf("VULKAN_SAMPLE_RATE_POLICY enabled=%u\n",enabled.sampleRateShading);
    std::printf("VULKAN_NATIVE_SAMPLER_POLICY anisotropy_enabled=%u max_anisotropy=%.9g\n",
        enabled.samplerAnisotropy,properties.limits.maxSamplerAnisotropy);
    std::printf("VULKAN_ENABLED_V1 shaderInt64=%u sampledImageArrayDynamicIndexing=%u "
        "runtimeDescriptorArray=%u\n", enabled.shaderInt64,
        enabled.shaderSampledImageArrayDynamicIndexing, indexing.runtimeDescriptorArray);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    // The cache starts empty and is never seeded from disk: nothing outside this
    // process may decide what a pipeline compiles to. A failure here is not
    // fatal, because VK_NULL_HANDLE is exactly the previous behaviour.
    VkPipelineCacheCreateInfo cache{};
    cache.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (vkCreatePipelineCache(device, &cache, nullptr, &pipelineCache) != VK_SUCCESS)
        pipelineCache = VK_NULL_HANDLE;
    std::printf("VULKAN_PIPELINE_CACHE_POLICY created=%u seeded_from_disk=0\n",
        pipelineCache != VK_NULL_HANDLE);
    return queue != VK_NULL_HANDLE;
}
}
