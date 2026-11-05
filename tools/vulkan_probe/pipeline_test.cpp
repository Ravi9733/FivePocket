// Driver pipeline compilation only; never draw or submit game shaders here.
#include "device.h"
#include "private_input.h"
#include "process_limits.h"
#include <array>
#include <cstdio>
#include <cstring>

namespace {
using recomp::gpu::checkVulkan;
struct Resources {
    explicit Resources(VkDevice input) : device(input) {}
    VkDevice device;
    std::array<VkDescriptorSetLayout, 5> layouts{};
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkShaderModule fixture = VK_NULL_HANDLE;
    VkShaderModule current = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    ~Resources() {
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (current) vkDestroyShaderModule(device, current, nullptr);
        if (fixture) vkDestroyShaderModule(device, fixture, nullptr);
        if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        for (auto layout : layouts) if (layout) vkDestroyDescriptorSetLayout(device, layout, nullptr);
    }
};
bool shader(VkDevice device, const std::vector<uint32_t>& words, VkShaderModule& output) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = words.size() * sizeof(uint32_t);
    info.pCode = words.data();
    return checkVulkan(vkCreateShaderModule(device, &info, nullptr, &output), "pipeline-shader");
}
bool setup(Resources& resources, const std::vector<uint32_t>& fixture) {
    if (!shader(resources.device, fixture, resources.fixture)) return false;
    for (uint32_t set = 0; set < resources.layouts.size(); ++set) {
        const uint32_t count = set == 4 ? 3 : 1;
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        for (uint32_t i = 0; i < count; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = set == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER :
                (set == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            bindings[i].descriptorCount = 1; // Static-use compile test, not an allocated texture heap.
            bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout.bindingCount = count;
        layout.pBindings = bindings.data();
        if (!checkVulkan(vkCreateDescriptorSetLayout(resources.device, &layout, nullptr, &resources.layouts[set]),
                         "pipeline-descriptor-layout")) return false;
    }
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = resources.layouts.size();
    layout.pSetLayouts = resources.layouts.data();
    if (!checkVulkan(vkCreatePipelineLayout(resources.device, &layout, nullptr, &resources.pipelineLayout),
                     "graphics-pipeline-layout")) return false;
    std::array<VkAttachmentDescription, 5> attachments{};
    std::array<VkAttachmentReference, 4> colors{};
    for (uint32_t i = 0; i < attachments.size(); ++i) {
        attachments[i].format = i == 4 ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[i].finalLayout = i == 4 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (i < 4) colors[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkAttachmentReference depth{4, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = colors.size();
    subpass.pColorAttachments = colors.data();
    subpass.pDepthStencilAttachment = &depth;
    VkRenderPassCreateInfo renderPass{};
    renderPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPass.attachmentCount = attachments.size();
    renderPass.pAttachments = attachments.data();
    renderPass.subpassCount = 1;
    renderPass.pSubpasses = &subpass;
    return checkVulkan(vkCreateRenderPass(resources.device, &renderPass, nullptr, &resources.renderPass),
                       "graphics-render-pass");
}
bool compile(Resources& resources, const recomp::gpu::ShaderModule& module) {
    if (!shader(resources.device, module.words, resources.current)) return false;
    const bool vertex = module.executionModel == 0;
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex ? resources.current : resources.fixture;
    stages[0].pName = "shaderMain";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = resources.current;
    stages[1].pName = "shaderMain";
    std::vector<VkVertexInputAttributeDescription> attributes;
    constexpr VkFormat formats[3][4] = {
        {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT},
        {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT},
        {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT},
    };
    for (const auto& input : module.vertexInputs) {
        attributes.push_back({input.location, 0, formats[input.kind][input.components - 1],
                              static_cast<uint32_t>(attributes.size()) * 16});
    }
    VkVertexInputBindingDescription binding{0, static_cast<uint32_t>(attributes.size()) * 16,
                                            VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo input{};
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    input.vertexBindingDescriptionCount = attributes.empty() ? 0 : 1;
    input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = attributes.size();
    input.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, 16, 16, 0, 1};
    VkRect2D scissor{{0,0}, {16,16}};
    VkPipelineViewportStateCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = 1; view.pViewports = &viewport;
    view.scissorCount = 1; view.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.rasterizerDiscardEnable = vertex;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo samples{};
    samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    std::array<VkPipelineColorBlendAttachmentState, 4> blendAttachments{};
    for (auto& attachment : blendAttachments) attachment.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = blendAttachments.size(); blend.pAttachments = blendAttachments.data();
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = vertex ? 1 : 2; info.pStages = stages.data();
    info.pVertexInputState = &input; info.pInputAssemblyState = &assembly;
    info.pViewportState = &view; info.pRasterizationState = &raster;
    info.pMultisampleState = &samples; info.pDepthStencilState = &depth; info.pColorBlendState = &blend;
    info.layout = resources.pipelineLayout; info.renderPass = resources.renderPass;
    if (!checkVulkan(vkCreateGraphicsPipelines(resources.device, VK_NULL_HANDLE, 1, &info, nullptr,
                                              &resources.pipeline), "create-graphics-pipeline")) return false;
    vkDestroyPipeline(resources.device, resources.pipeline, nullptr);
    resources.pipeline = VK_NULL_HANDLE;
    vkDestroyShaderModule(resources.device, resources.current, nullptr);
    resources.current = VK_NULL_HANDLE;
    return true;
}
}
int main(int argc, char** argv) {
    if (!recomp::gpu::initializeProbeProcess()) return 2;
    if (argc != 3) return 2;
    std::vector<uint8_t> packBytes, fixtureBytes;
    recomp::gpu::ShaderModulePack pack;
    if (!recomp::gpu::readInput(argv[1], packBytes, 64 * 1024 * 1024) ||
        !recomp::gpu::parseShaderModulePack(packBytes.data(), packBytes.size(), pack) || pack.constantBinding != 2 ||
        !recomp::gpu::readInput(argv[2], fixtureBytes, 1024 * 1024) || fixtureBytes.size() < 20 || fixtureBytes.size() % 4)
        return 2;
    std::vector<uint32_t> fixture(fixtureBytes.size() / 4);
    std::memcpy(fixture.data(), fixtureBytes.data(), fixtureBytes.size());
    if (fixture[0] != 0x07230203) return 2;
    recomp::gpu::ProbeDevice context;
    if (!context.inspect() || context.properties.limits.maxBoundDescriptorSets < 5 ||
        context.properties.limits.maxColorAttachments < 4 ||
        context.properties.limits.maxVertexOutputComponents < 76 || !context.create(pack.requirements)) return 1;
    Resources resources(context.device);
    if (!setup(resources, fixture)) return 1;
    size_t vertices = 0, pixels = 0;
    for (const auto& module : pack.modules) {
        for (const auto& input : module.vertexInputs)
            if (input.location >= context.properties.limits.maxVertexInputAttributes) return 2;
        if (module.vertexInputs.size() * 16 > context.properties.limits.maxVertexInputBindingStride) return 2;
        if (!compile(resources, module)) {
            std::printf("VULKAN_PIPELINES_FAILED index=%u model=%u\n", module.index, module.executionModel);
            return 1;
        }
        if (module.executionModel == 0) ++vertices; else ++pixels;
    }
    std::printf("VULKAN_PIPELINES_V1 vertex=%zu pixel=%zu draws=0 game_pairings_verified=0\n", vertices, pixels);
    return 0;
}
