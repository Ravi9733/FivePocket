#pragma once
#include "native_texture_image.h"
#include "native_texture_sampler.h"
#include <cstring>
#include <cstdlib>

namespace recomp::gpu {
// Standalone bounded sampling measurement. Explicit LODs, not derivatives or
// game draw admission. Real Vulkan queue/fence; no guest memory or publication.
inline std::optional<std::vector<std::array<float,4>>> sampleMipProbe(
        ProbeDevice& device,const NativeTextureImage& image,const NativeTextureSamplerPlan& plan,
        const std::vector<std::uint32_t>& code,const std::vector<std::array<float,4>>& queries) {
    if(queries.empty() || queries.size()>32768 || code.empty() || code.size()>65536 ||
        image.levelCount()!=plan.maxLevel+1 || !image.view() ||
        ((plan.magFilter || plan.minFilter) && !image.supportsLinearFiltering()))return std::nullopt;
    struct Resources {
        VkDevice d;
        std::array<VkBuffer,2> buffers{};std::array<VkDeviceMemory,2> memory{};std::array<void*,2> mapped{};
        VkSampler sampler{};VkShaderModule module{};VkDescriptorSetLayout setLayout{};
        VkDescriptorPool descriptors{};VkPipelineLayout layout{};VkPipeline pipeline{};
        VkCommandPool pool{};VkFence fence{};
        ~Resources() {
            if(fence)vkDestroyFence(d,fence,nullptr);
            if(pool)vkDestroyCommandPool(d,pool,nullptr);
            if(pipeline)vkDestroyPipeline(d,pipeline,nullptr);
            if(layout)vkDestroyPipelineLayout(d,layout,nullptr);
            if(descriptors)vkDestroyDescriptorPool(d,descriptors,nullptr);
            if(setLayout)vkDestroyDescriptorSetLayout(d,setLayout,nullptr);
            if(module)vkDestroyShaderModule(d,module,nullptr);
            if(sampler)vkDestroySampler(d,sampler,nullptr);
            for(unsigned i=0;i<2;++i) {
                if(mapped[i])vkUnmapMemory(d,memory[i]);
                if(buffers[i])vkDestroyBuffer(d,buffers[i],nullptr);
                if(memory[i])vkFreeMemory(d,memory[i],nullptr);
            }
        }
    } r{device.device};
    const std::array<VkDeviceSize,2> sizes{queries.size()*16,(queries.size()+2)*16};
    VkPhysicalDeviceMemoryProperties memory{};vkGetPhysicalDeviceMemoryProperties(device.physical,&memory);
    for(unsigned i=0;i<2;++i) {
        VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=sizes[i];b.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if(vkCreateBuffer(r.d,&b,nullptr,&r.buffers[i])!=VK_SUCCESS)return std::nullopt;
        VkMemoryRequirements required{};vkGetBufferMemoryRequirements(r.d,r.buffers[i],&required);
        unsigned type=memory.memoryTypeCount;
        constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for(unsigned j=0;j<memory.memoryTypeCount;++j)if((required.memoryTypeBits&(1u<<j)) &&
            (memory.memoryTypes[j].propertyFlags&flags)==flags){type=j;break;}
        if(type==memory.memoryTypeCount)return std::nullopt;
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=required.size;a.memoryTypeIndex=type;
        if(vkAllocateMemory(r.d,&a,nullptr,&r.memory[i])!=VK_SUCCESS ||
            vkBindBufferMemory(r.d,r.buffers[i],r.memory[i],0)!=VK_SUCCESS ||
            vkMapMemory(r.d,r.memory[i],0,VK_WHOLE_SIZE,0,&r.mapped[i])!=VK_SUCCESS)return std::nullopt;
    }
    std::memcpy(r.mapped[0],queries.data(),sizes[0]);std::memset(r.mapped[1],0x5A,sizes[1]);
    VkSamplerCreateInfo sampler{};sampler.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter=plan.magFilter?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    sampler.minFilter=plan.minFilter?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
    sampler.mipmapMode=plan.mipFilter?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
    constexpr VkSamplerAddressMode clamp[]{VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if(plan.clamp[0]>2 || plan.clamp[1]>2 || plan.magFilter>1 || plan.minFilter>1 || plan.mipFilter>1)return std::nullopt;
    sampler.addressModeU=clamp[plan.clamp[0]];sampler.addressModeV=clamp[plan.clamp[1]];
    sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sampler.maxLod=float(plan.maxLevel);
    if(vkCreateSampler(r.d,&sampler,nullptr,&r.sampler)!=VK_SUCCESS)return std::nullopt;
    VkShaderModuleCreateInfo module{};module.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module.codeSize=code.size()*4;module.pCode=code.data();
    if(vkCreateShaderModule(r.d,&module,nullptr,&r.module)!=VK_SUCCESS)return std::nullopt;
    const std::array<VkDescriptorSetLayoutBinding,3> bindings{{
        {0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
    VkDescriptorSetLayoutCreateInfo set{};set.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set.bindingCount=3;set.pBindings=bindings.data();
    if(vkCreateDescriptorSetLayout(r.d,&set,nullptr,&r.setLayout)!=VK_SUCCESS)return std::nullopt;
    const std::array<VkDescriptorPoolSize,2> counts{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1}}};
    VkDescriptorPoolCreateInfo dp{};dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dp.maxSets=1;dp.poolSizeCount=2;dp.pPoolSizes=counts.data();
    if(vkCreateDescriptorPool(r.d,&dp,nullptr,&r.descriptors)!=VK_SUCCESS)return std::nullopt;
    VkDescriptorSet descriptor{};VkDescriptorSetAllocateInfo allocate{};allocate.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool=r.descriptors;allocate.descriptorSetCount=1;allocate.pSetLayouts=&r.setLayout;
    if(vkAllocateDescriptorSets(r.d,&allocate,&descriptor)!=VK_SUCCESS)return std::nullopt;
    const std::array<VkDescriptorBufferInfo,2> buffers{{{r.buffers[0],0,sizes[0]},{r.buffers[1],0,sizes[1]}}};
    const VkDescriptorImageInfo texture{r.sampler,image.view(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::array<VkWriteDescriptorSet,3> writes{};
    for(unsigned i=0;i<3;++i) {
        auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=descriptor;w.dstBinding=i;
        w.descriptorCount=1;w.descriptorType=bindings[i].descriptorType;
        if(i<2)w.pBufferInfo=&buffers[i];else w.pImageInfo=&texture;
    }
    vkUpdateDescriptorSets(r.d,3,writes.data(),0,nullptr);
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,4};
    VkPipelineLayoutCreateInfo layout{};layout.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount=1;layout.pSetLayouts=&r.setLayout;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&push;
    if(vkCreatePipelineLayout(r.d,&layout,nullptr,&r.layout)!=VK_SUCCESS)return std::nullopt;
    VkComputePipelineCreateInfo pipeline{};pipeline.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;pipeline.layout=r.layout;
    pipeline.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module=r.module;pipeline.stage.pName="main";
    if(vkCreateComputePipelines(r.d,VK_NULL_HANDLE,1,&pipeline,nullptr,&r.pipeline)!=VK_SUCCESS)return std::nullopt;
    VkCommandPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pool.queueFamilyIndex=device.queueFamily;
    if(vkCreateCommandPool(r.d,&pool,nullptr,&r.pool)!=VK_SUCCESS)return std::nullopt;
    VkCommandBuffer command{};VkCommandBufferAllocateInfo commands{};commands.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commands.commandPool=r.pool;commands.commandBufferCount=1;commands.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    if(vkAllocateCommandBuffers(r.d,&commands,&command)!=VK_SUCCESS)return std::nullopt;
    VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if(vkCreateFence(r.d,&fence,nullptr,&r.fence)!=VK_SUCCESS)return std::nullopt;
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(command,&begin)!=VK_SUCCESS)return std::nullopt;
    VkMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,r.pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,r.layout,0,1,&descriptor,0,nullptr);
    const unsigned count=static_cast<unsigned>(queries.size());
    vkCmdPushConstants(command,r.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&count);vkCmdDispatch(command,(count+63)/64,1,1);
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    if(vkEndCommandBuffer(command)!=VK_SUCCESS)return std::nullopt;
    VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&command;
    if(vkQueueSubmit(device.queue,1,&submit,r.fence)!=VK_SUCCESS ||
        vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
    const auto* output=static_cast<const std::uint8_t*>(r.mapped[1]);
    if(std::memcmp(r.mapped[0],queries.data(),sizes[0]) ||
        !std::all_of(output,output+16,[](auto b){return b==0x5A;}) ||
        !std::all_of(output+sizes[1]-16,output+sizes[1],[](auto b){return b==0x5A;}))return std::nullopt;
    std::vector<std::array<float,4>> result(queries.size());std::memcpy(result.data(),output+16,count*16);
    return result;
}
}
