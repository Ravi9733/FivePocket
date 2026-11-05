// Execute the synthetic descriptor32 ABI fixture and verify every output word.
#include "device.h"
#include "private_input.h"
#include "process_limits.h"
#include "shader_constant_upload.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
using recomp::gpu::checkVulkan;
constexpr size_t OutputWords = 2784;

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t* mapped = nullptr;
    size_t words = 0;
};
struct Resources {
    explicit Resources(VkDevice input) : device(input) {}
    VkDevice device;
    std::array<Buffer, 4> buffers{};
    std::array<VkDescriptorSetLayout, 5> layouts{};
    std::array<VkDescriptorSet, 5> sets{};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    ~Resources() {
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (shader) vkDestroyShaderModule(device, shader, nullptr);
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
        for (const auto layout : layouts) if (layout) vkDestroyDescriptorSetLayout(device, layout, nullptr);
        for (auto& buffer : buffers) {
            if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
            if (buffer.handle) vkDestroyBuffer(device, buffer.handle, nullptr);
            if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
        }
    }
};

bool createBuffer(recomp::gpu::ProbeDevice& context, Buffer& output, size_t words) {
    output.words = words;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = words * sizeof(uint32_t);
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!checkVulkan(vkCreateBuffer(context.device, &info, nullptr, &output.handle), "create-buffer")) return false;
    VkMemoryRequirements required{};
    vkGetBufferMemoryRequirements(context.device, output.handle, &required);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(context.physical, &memory);
    uint32_t memoryType = std::numeric_limits<uint32_t>::max();
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((required.memoryTypeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & flags) == flags) { memoryType = i; break; }
    }
    if (memoryType == std::numeric_limits<uint32_t>::max()) return false;
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = required.size;
    allocation.memoryTypeIndex = memoryType;
    if (!checkVulkan(vkAllocateMemory(context.device, &allocation, nullptr, &output.memory), "allocate-buffer") ||
        !checkVulkan(vkBindBufferMemory(context.device, output.handle, output.memory, 0), "bind-buffer")) return false;
    void* mapped = nullptr;
    if (!checkVulkan(vkMapMemory(context.device, output.memory, 0, allocation.allocationSize, 0, &mapped), "map-buffer")) return false;
    output.mapped = static_cast<uint32_t*>(mapped);
    return true;
}

bool configure(recomp::gpu::ProbeDevice& context, Resources& resources, const std::vector<uint32_t>& code) {
    const std::array<size_t, 4> sizes{1024, 896, 80, OutputWords};
    for (size_t i = 0; i < sizes.size(); ++i)
        if (!createBuffer(context, resources.buffers[i], sizes[i])) return false;
    for (uint32_t set = 0; set < resources.layouts.size(); ++set) {
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        const uint32_t count = set == 4 ? 3 : (set == 0 ? 1 : 0);
        for (uint32_t i = 0; i < count; ++i) {
            bindings[i].binding = set == 0 ? 1 : i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = count;
        info.pBindings = bindings.data();
        if (!checkVulkan(vkCreateDescriptorSetLayout(context.device, &info, nullptr, &resources.layouts[set]),
                         "create-descriptor-layout")) return false;
    }
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = resources.layouts.size();
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &poolSize;
    if (!checkVulkan(vkCreateDescriptorPool(context.device, &pool, nullptr, &resources.pool), "create-descriptor-pool")) return false;
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = resources.pool;
    allocate.descriptorSetCount = resources.layouts.size();
    allocate.pSetLayouts = resources.layouts.data();
    if (!checkVulkan(vkAllocateDescriptorSets(context.device, &allocate, resources.sets.data()), "allocate-descriptors")) return false;
    std::array<VkDescriptorBufferInfo, 4> bufferInfo{};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        bufferInfo[i] = {resources.buffers[i].handle, 0, resources.buffers[i].words * sizeof(uint32_t)};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = resources.sets[i == 3 ? 0 : 4];
        writes[i].dstBinding = i == 3 ? 1 : i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfo[i];
    }
    vkUpdateDescriptorSets(context.device, writes.size(), writes.data(), 0, nullptr);
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = resources.layouts.size();
    layout.pSetLayouts = resources.layouts.data();
    if (!checkVulkan(vkCreatePipelineLayout(context.device, &layout, nullptr, &resources.pipelineLayout), "create-pipeline-layout")) return false;
    VkShaderModuleCreateInfo shader{};
    shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader.codeSize = code.size() * sizeof(uint32_t);
    shader.pCode = code.data();
    if (!checkVulkan(vkCreateShaderModule(context.device, &shader, nullptr, &resources.shader), "create-fixture-shader")) return false;
    VkComputePipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = resources.shader;
    pipeline.stage.pName = "shaderMain";
    pipeline.layout = resources.pipelineLayout;
    if (!checkVulkan(vkCreateComputePipelines(context.device, VK_NULL_HANDLE, 1, &pipeline, nullptr,
                                            &resources.pipeline), "create-compute-pipeline")) return false;
    VkCommandPoolCreateInfo commandPool{};
    commandPool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPool.queueFamilyIndex = context.queueFamily;
    if (!checkVulkan(vkCreateCommandPool(context.device, &commandPool, nullptr, &resources.commandPool), "create-command-pool")) return false;
    VkCommandBufferAllocateInfo command{};
    command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command.commandPool = resources.commandPool;
    command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command.commandBufferCount = 1;
    if (!checkVulkan(vkAllocateCommandBuffers(context.device, &command, &resources.command), "allocate-command")) return false;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (!checkVulkan(vkBeginCommandBuffer(resources.command, &begin), "begin-command")) return false;
    VkMemoryBarrier upload{};
    upload.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    upload.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    upload.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(resources.command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &upload, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(resources.command, VK_PIPELINE_BIND_POINT_COMPUTE, resources.pipeline);
    vkCmdBindDescriptorSets(resources.command, VK_PIPELINE_BIND_POINT_COMPUTE, resources.pipelineLayout,
                           0, resources.sets.size(), resources.sets.data(), 0, nullptr);
    vkCmdDispatch(resources.command, 1, 1, 1);
    VkMemoryBarrier download{};
    download.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    download.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    download.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(resources.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &download, 0, nullptr, 0, nullptr);
    if (!checkVulkan(vkEndCommandBuffer(resources.command), "end-command")) return false;
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    return checkVulkan(vkCreateFence(context.device, &fence, nullptr, &resources.fence), "create-fence");
}

uint32_t sourceWord(size_t buffer, size_t word, uint32_t iteration) {
    constexpr std::array<uint32_t, 12> bitPatterns{0, 0x80000000, 0x7F800000, 0xFF800000,
        0x7FC1ABCD, 0x7F812345, 1, 0x007FFFFF, 0x00800000, 0x3F800000, 0x01234567, 0xFEDCBA98};
    if (buffer < 2 && word < bitPatterns.size()) return bitPatterns[word];
    if (buffer == 2 && word == 76) return iteration;
    return 0x3f000000u ^ (uint32_t(word) * 997u + uint32_t(buffer) * 4099u);
}

bool run(recomp::gpu::ProbeDevice& context, Resources& resources, uint32_t iteration) {
    auto& buffers = resources.buffers;
    recomp::gpu::ShaderFloatConstants decoded;
    for (size_t i = 0; i < 2; ++i) {
        std::vector<uint8_t> source(buffers[i].words * 4);
        for (size_t word = 0; word < buffers[i].words; ++word) {
            const auto value = sourceWord(i, word, iteration);
            for (unsigned byte = 0; byte < 4; ++byte)
                source[word * 4 + byte] = uint8_t(value >> ((3 - byte) * 8));
        }
        if (!recomp::gpu::decodeShaderFloatConstants(i == 0 ? recomp::gpu::FloatConstantStage::Vertex
                : recomp::gpu::FloatConstantStage::Pixel, 0, buffers[i].words / 4,
                source.data(), source.size(), decoded)) return false;
    }
    std::memcpy(buffers[0].mapped, decoded.vertex.data(), decoded.vertex.size() * 4);
    std::memcpy(buffers[1].mapped, decoded.pixel.data(), decoded.pixel.size() * 4);
    // Shared ABI fields are host-managed here, not inferred from a guest bank.
    for (size_t word = 0; word < buffers[2].words; ++word)
        buffers[2].mapped[word] = sourceWord(2, word, iteration);
    for (size_t i = 0; i < OutputWords; ++i) buffers[3].mapped[i] = 0xdeadbeefu;
    if (!checkVulkan(vkResetFences(context.device, 1, &resources.fence), "reset-fence")) return false;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &resources.command;
    if (!checkVulkan(vkQueueSubmit(context.queue, 1, &submit, resources.fence), "submit-compute")) return false;
    const auto waited = vkWaitForFences(context.device, 1, &resources.fence, VK_TRUE, 5'000'000'000ULL);
    if (waited != VK_SUCCESS) {
        checkVulkan(waited, "wait-compute");
        // Do not destroy potentially in-flight resources or claim completion.
        std::fflush(stdout);
        std::_Exit(4);
    }
    std::array<uint32_t, OutputWords> expected{};
    size_t cursor = 0;
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < buffers[i].words; ++j) expected[cursor++] = sourceWord(i, j, iteration);
    const std::array<uint32_t, 16> sharedIndices{64,76,77,65,66,67,68,69,70,71,72,73,74,75,78,79};
    for (size_t i = 0; i < sharedIndices.size(); ++i)
        expected[2000 + i] = i == 1 ? (iteration != 0) : sourceWord(2, sharedIndices[i], iteration);
    constexpr std::array<uint32_t, 4> values{0x3f800000, 0x40000000, 0x40400000, 0x40800000};
    for (uint32_t pattern = 0; pattern < 3; ++pattern) {
        const uint32_t mask = pattern == 0 ? 0 : (pattern == 1 ? 0xffffffffu : 0x80000001u);
        for (uint32_t index = 0; index < 64; ++index) {
            const bool swapped = index < 32 && (mask & (1u << (index & 31u)));
            for (uint32_t j = 0; j < 4; ++j)
                expected[2016 + (pattern * 64 + index) * 4 + j] = values[swapped ? (j ^ 1u) : j];
        }
    }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (buffers[3].mapped[i] != expected[i]) {
            std::printf("VULKAN_CONSTANTS_MISMATCH iteration=%u word=%zu\n", iteration, i);
            return false;
        }
    }
    return true;
}
}

int main(int argc, char** argv) {
    if (!recomp::gpu::initializeProbeProcess()) return 2;
    if (argc != 2) return 2;
    std::vector<uint8_t> data;
    if (!recomp::gpu::readInput(argv[1], data, 1024 * 1024) || data.size() < 20 || data.size() % 4) return 2;
    std::vector<uint32_t> code(data.size() / 4);
    std::memcpy(code.data(), data.data(), data.size()); // Supported host: little-endian Android ARM64.
    if (code[0] != 0x07230203) return 2;
    recomp::gpu::ProbeDevice context;
    recomp::gpu::ShaderRequirements requirements;
    requirements.runtimeDescriptorArray = true;
    if (!context.inspect() || context.properties.limits.maxBoundDescriptorSets < 5 || !context.create(requirements)) return 1;
    Resources resources(context.device);
    if (!configure(context, resources, code) || !run(context, resources, 0) || !run(context, resources, 1)) return 1;
    std::printf("VULKAN_CONSTANTS_V1 dispatches=2 verified_words=%zu binding=2 shaderInt64_enabled=0 "
        "guest_be_float_words=3840\n", OutputWords * 2);
    return 0;
}
