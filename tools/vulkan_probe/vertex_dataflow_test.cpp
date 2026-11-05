// Standalone compute/controlled fragment probes. Never a guest draw or game frame.
#include "device.h"
#include "private_input.h"
#include "process_limits.h"
#ifdef RECOMP_R8_PLANES_SAMPLE
#include "r8_plane_images.h"
#endif
#ifdef RECOMP_TEXTURE32_FRAGMENT
#include "texture_fragment_target.h"
#endif
#ifdef RECOMP_RECTANGLE_DEPTH_STENCIL
#include "rectangle_depth_target.h"
#endif
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace {
#ifdef RECOMP_FULLSIZE_HALF_COLOR
constexpr unsigned MaxVertices=1280*720,VertexBytes=8,OutputBytes=MaxVertices*VertexBytes+32;
#ifdef RECOMP_PERSISTENT_HALF_COLOR
constexpr const char* Profile="PERSISTENT_HALF_COLOR";
#else
constexpr const char* Profile="FULLSIZE_HALF_COLOR";
#endif
#elif defined(RECOMP_TILED_COLOR_COPY)
constexpr unsigned MaxVertices=1,VertexBytes=1280*736*4,OutputBytes=VertexBytes+32;
constexpr const char* Profile="TILED_COLOR_COPY";
#elif defined(RECOMP_HALF_COLOR_STORE)
constexpr unsigned MaxVertices=1,VertexBytes=10485760,OutputBytes=VertexBytes+32;
constexpr const char* Profile="HALF_COLOR_STORE";
#elif defined(RECOMP_HALF_COLOR_LOAD)
constexpr unsigned MaxVertices=1,VertexBytes=1280*720*8,OutputBytes=VertexBytes+32;
constexpr const char* Profile="HALF_COLOR_LOAD";
#elif defined(RECOMP_COLOR_7E3)
constexpr unsigned MaxVertices=256,VertexBytes=32,OutputBytes=8224;
constexpr const char* Profile="COLOR_7E3";
#elif defined(RECOMP_RECTANGLE_DEPTH_STENCIL)
constexpr unsigned MaxVertices=1024,OutputBytes=5168;
constexpr const char* Profile="RECTANGLE_DEPTH_STENCIL";
#elif defined(RECOMP_RECTANGLE_VERTEX_PAIR)
constexpr unsigned MaxVertices=1024,VertexBytes=16,OutputBytes=16416;
constexpr const char* Profile="RECTANGLE_VERTEX_PAIR";
#elif defined(RECOMP_R8_PLANES_VERTEX_PAIR)
constexpr unsigned MaxVertices=1024,VertexBytes=16,OutputBytes=16416;
constexpr const char* Profile="R8_PLANES_VERTEX_PAIR";
#elif defined(RECOMP_R8_PLANES_FRAGMENT)
constexpr unsigned MaxVertices=1024,VertexBytes=16,OutputBytes=16416;
constexpr const char* Profile="R8_PLANES_FRAGMENT";
#elif defined(RECOMP_R8_PLANES_SAMPLE)
constexpr unsigned MaxVertices=4,VertexBytes=16,OutputBytes=96;
constexpr const char* Profile="R8_PLANES_PIXEL";
#elif defined(RECOMP_TEXTURE32_VERTEX_PAIR)
constexpr unsigned MaxVertices=1024,VertexBytes=16,OutputBytes=16416;
constexpr const char* Profile="TEXTURE32_VERTEX_PAIR";
#elif defined(RECOMP_TEXTURE32_FRAGMENT)
constexpr unsigned MaxVertices=1024,VertexBytes=16,OutputBytes=16416;
constexpr const char* Profile="TEXTURE32_FRAGMENT";
#elif defined(RECOMP_TEXTURE32_PIXEL)
constexpr unsigned MaxVertices=1024,VertexBytes=16,OutputBytes=16416;
constexpr const char* Profile="TEXTURE32_PIXEL";
#elif defined(RECOMP_PIXEL_SAMPLE_ARITHMETIC)
constexpr unsigned MaxVertices=4,VertexBytes=16,OutputBytes=96;
constexpr const char* Profile="PIXEL_SAMPLE_ARITHMETIC";
#elif defined(RECOMP_TEXTURE32_SAMPLE)
constexpr unsigned MaxVertices=1024,VertexBytes=16,OutputBytes=16416;
constexpr const char* Profile="TEXTURE32_SAMPLE";
#elif defined(RECOMP_LOADING_VERTEX_ARITHMETIC)
constexpr unsigned MaxVertices=128,VertexBytes=48,OutputBytes=MaxVertices*VertexBytes+32;
constexpr const char* Profile="LOADING_VERTEX_ARITHMETIC";
#elif defined(RECOMP_VERTEX_ARITHMETIC)
constexpr unsigned MaxVertices=4,VertexBytes=48,OutputBytes=224;
constexpr const char* Profile="VERTEX_ARITHMETIC";
#else
constexpr unsigned MaxVertices=3,VertexBytes=32,OutputBytes=128;
constexpr const char* Profile="VERTEX_DATAFLOW";
#endif
#ifdef RECOMP_PERSISTENT_HALF_COLOR
constexpr unsigned ExpectedFileBytes=72;
#else
constexpr unsigned ExpectedFileBytes=OutputBytes;
#endif
#ifdef RECOMP_TEXTURE32_FRAGMENT
#ifdef RECOMP_PERSISTENT_HALF_COLOR
constexpr recomp::gpu::FragmentTargetConfig ColorTarget{1280,720,VK_FORMAT_R16G16B16A16_SFLOAT,true};
#elif defined(RECOMP_FULLSIZE_HALF_COLOR)
constexpr recomp::gpu::FragmentTargetConfig ColorTarget{1280,720,VK_FORMAT_R16G16B16A16_SFLOAT};
#else
constexpr recomp::gpu::FragmentTargetConfig ColorTarget{};
#endif
#endif
#ifdef RECOMP_TEXTURE32_VERTEX_PAIR
constexpr VkShaderStageFlags ShaderStage=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
constexpr auto PipelineStage=VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
#elif defined(RECOMP_TEXTURE32_FRAGMENT)
constexpr auto ShaderStage=VK_SHADER_STAGE_FRAGMENT_BIT;
constexpr auto PipelineStage=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
#else
constexpr auto ShaderStage=VK_SHADER_STAGE_COMPUTE_BIT;
constexpr auto PipelineStage=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
#endif
#if defined(RECOMP_TILED_COLOR_COPY) || defined(RECOMP_HALF_COLOR_LOAD)
constexpr unsigned InputLimit=10485760+32;
#elif defined(RECOMP_HALF_COLOR_STORE)
constexpr unsigned InputLimit=1280*720*8+32;
#elif defined(RECOMP_RECTANGLE_VERTEX_PAIR)
constexpr unsigned InputLimit=4096;
#elif defined(RECOMP_R8_PLANES_VERTEX_PAIR)
constexpr unsigned InputLimit=recomp::gpu::R8PlaneImages::ImageBytes+512;
#elif defined(RECOMP_R8_PLANES_FRAGMENT)
constexpr unsigned InputLimit=recomp::gpu::R8PlaneImages::ImageBytes+16+80;
#elif defined(RECOMP_R8_PLANES_SAMPLE)
constexpr unsigned InputLimit=recomp::gpu::R8PlaneImages::ImageBytes+128+80;
#elif defined(RECOMP_LOADING_VERTEX_ARITHMETIC)
constexpr unsigned InputLimit=256*1024+8208;
#elif defined(RECOMP_TEXTURE32_VERTEX_PAIR)
constexpr unsigned InputLimit=4608;
#elif defined(RECOMP_TEXTURE32_PIXEL)
constexpr unsigned InputLimit=4176;
#else
constexpr unsigned InputLimit=4096;
#endif
struct Resources {
    VkDevice d;
#ifdef RECOMP_PERSISTENT_HALF_COLOR
    VkRenderPass loadPass{};
    unsigned sequenceStep=0;
    bool forceClear=false,disableBlend=false;
#endif
#ifdef RECOMP_RECTANGLE_DEPTH_STENCIL
    bool depthWrite=true,stencilWrite=true;
    unsigned stencilReference=0;
#endif
    std::array<VkBuffer,2> buffers{};
    std::array<VkDeviceMemory,2> memory{};
    std::array<void*,2> mapped{};
    VkShaderModule shader{};VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};VkPipeline pipeline{};
    VkDescriptorPool descriptors{};VkDescriptorSet set{};
    VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
#ifdef RECOMP_TEXTURE32_FRAGMENT
    VkShaderModule vertexShader{};VkImage colorImage{};VkDeviceMemory colorMemory{};
    VkImageView colorView{};VkRenderPass renderPass{};VkFramebuffer framebuffer{};
#endif
#ifdef RECOMP_TEXTURE32_SAMPLE
    VkImage image{};VkDeviceMemory imageMemory{};VkImageView view{};VkSampler sampler{};
    bool imageReady=false,replaceImageControl=false;
#endif
#ifdef RECOMP_R8_PLANES_SAMPLE
    recomp::gpu::R8PlaneImages planes;
    explicit Resources(VkDevice device):d(device),planes(device){}
#else
    explicit Resources(VkDevice device):d(device){}
#endif
    ~Resources(){
        if(fence)vkDestroyFence(d,fence,nullptr);
        if(pool)vkDestroyCommandPool(d,pool,nullptr);
        if(pipeline)vkDestroyPipeline(d,pipeline,nullptr);
        if(layout)vkDestroyPipelineLayout(d,layout,nullptr);
        if(descriptors)vkDestroyDescriptorPool(d,descriptors,nullptr);
        if(setLayout)vkDestroyDescriptorSetLayout(d,setLayout,nullptr);
        if(shader)vkDestroyShaderModule(d,shader,nullptr);
#ifdef RECOMP_TEXTURE32_FRAGMENT
        if(vertexShader)vkDestroyShaderModule(d,vertexShader,nullptr);
        if(framebuffer)vkDestroyFramebuffer(d,framebuffer,nullptr);
        if(renderPass)vkDestroyRenderPass(d,renderPass,nullptr);
#ifdef RECOMP_PERSISTENT_HALF_COLOR
        if(loadPass)vkDestroyRenderPass(d,loadPass,nullptr);
#endif
        if(colorView)vkDestroyImageView(d,colorView,nullptr);
        if(colorImage)vkDestroyImage(d,colorImage,nullptr);
        if(colorMemory)vkFreeMemory(d,colorMemory,nullptr);
#endif
#ifdef RECOMP_TEXTURE32_SAMPLE
        if(sampler)vkDestroySampler(d,sampler,nullptr);
        if(view)vkDestroyImageView(d,view,nullptr);
        if(image)vkDestroyImage(d,image,nullptr);
        if(imageMemory)vkFreeMemory(d,imageMemory,nullptr);
#endif
        for(unsigned i=0;i<2;++i){if(mapped[i])vkUnmapMemory(d,memory[i]);
            if(buffers[i])vkDestroyBuffer(d,buffers[i],nullptr);
            if(memory[i])vkFreeMemory(d,memory[i],nullptr);}
    }
};
bool setup(recomp::gpu::ProbeDevice& device,Resources& r,
        const std::vector<std::uint32_t>& code,std::size_t inputBytes,const std::vector<std::uint32_t>& vertexCode) {
#ifndef RECOMP_TEXTURE32_FRAGMENT
    (void)vertexCode;
#endif
#if defined(RECOMP_TILED_COLOR_COPY) || defined(RECOMP_HALF_COLOR_STORE) || defined(RECOMP_HALF_COLOR_LOAD)
    const auto& limits=device.properties.limits;
    if(limits.maxStorageBufferRange<std::max(InputLimit,OutputBytes) || limits.maxComputeWorkGroupInvocations<64 ||
        limits.maxComputeWorkGroupSize[0]<8 || limits.maxComputeWorkGroupSize[1]<8 ||
        limits.maxComputeWorkGroupCount[0]<160 || limits.maxComputeWorkGroupCount[1]<90)return false;
#endif
    VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(device.physical,&props);
    for(unsigned i=0;i<2;++i){
        VkBufferCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size=i?OutputBytes:inputBytes;info.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
#ifdef RECOMP_TEXTURE32_FRAGMENT
        if(i)info.usage|=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
#endif
#if defined(RECOMP_TEXTURE32_SAMPLE) || defined(RECOMP_R8_PLANES_SAMPLE)
        if(!i)info.usage|=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
#endif
        if(vkCreateBuffer(r.d,&info,nullptr,&r.buffers[i])!=VK_SUCCESS)return false;
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(r.d,r.buffers[i],&req);
        unsigned type=props.memoryTypeCount;
        for(unsigned n=0;n<props.memoryTypeCount;++n)if((req.memoryTypeBits&(1u<<n)) &&
            (props.memoryTypes[n].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=n;break;}
        if(type==props.memoryTypeCount)return false;
        VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize=req.size;allocation.memoryTypeIndex=type;
        if(vkAllocateMemory(r.d,&allocation,nullptr,&r.memory[i])!=VK_SUCCESS ||
            vkBindBufferMemory(r.d,r.buffers[i],r.memory[i],0)!=VK_SUCCESS ||
            vkMapMemory(r.d,r.memory[i],0,VK_WHOLE_SIZE,0,&r.mapped[i])!=VK_SUCCESS)return false;
    }
#ifdef RECOMP_TEXTURE32_SAMPLE
    VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(device.physical,VK_FORMAT_R8G8B8A8_UNORM,&format);
    constexpr VkFormatFeatureFlags required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if((format.optimalTilingFeatures&required)!=required)return false;
    VkImageCreateInfo image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;image.imageType=VK_IMAGE_TYPE_2D;
    image.format=VK_FORMAT_R8G8B8A8_UNORM;image.extent={32,32,1};image.mipLevels=1;image.arrayLayers=1;
    image.samples=VK_SAMPLE_COUNT_1_BIT;image.tiling=VK_IMAGE_TILING_OPTIMAL;
    image.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageFormatProperties supported{};
    if(vkGetPhysicalDeviceImageFormatProperties(device.physical,image.format,image.imageType,image.tiling,
            image.usage,0,&supported)!=VK_SUCCESS || supported.maxExtent.width<32 || supported.maxExtent.height<32)return false;
    if(vkCreateImage(r.d,&image,nullptr,&r.image)!=VK_SUCCESS)return false;
    VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(r.d,r.image,&requirements);
    unsigned memoryType=props.memoryTypeCount;
    for(unsigned i=0;i<props.memoryTypeCount;++i)if((requirements.memoryTypeBits&(1u<<i)) &&
        (props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){memoryType=i;break;}
    if(memoryType==props.memoryTypeCount)return false;
    VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=memoryType;
    if(vkAllocateMemory(r.d,&allocation,nullptr,&r.imageMemory)!=VK_SUCCESS ||
        vkBindImageMemory(r.d,r.image,r.imageMemory,0)!=VK_SUCCESS)return false;
    VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image=r.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=image.format;
    view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(vkCreateImageView(r.d,&view,nullptr,&r.view)!=VK_SUCCESS)return false;
    VkSamplerCreateInfo sampler{};sampler.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter=VK_FILTER_LINEAR;sampler.minFilter=VK_FILTER_LINEAR;sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU=VK_SAMPLER_ADDRESS_MODE_REPEAT;sampler.addressModeV=VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;sampler.minLod=0;sampler.maxLod=0;
    if(vkCreateSampler(r.d,&sampler,nullptr,&r.sampler)!=VK_SUCCESS)return false;
#endif
#ifdef RECOMP_R8_PLANES_SAMPLE
    if(!r.planes.setup(device))return false;
#endif
    VkShaderModuleCreateInfo shader{};shader.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader.codeSize=code.size()*4;shader.pCode=code.data();
    if(vkCreateShaderModule(r.d,&shader,nullptr,&r.shader)!=VK_SUCCESS)return false;
#ifdef RECOMP_TEXTURE32_FRAGMENT
    shader.codeSize=vertexCode.size()*4;shader.pCode=vertexCode.data();
    if(vkCreateShaderModule(r.d,&shader,nullptr,&r.vertexShader)!=VK_SUCCESS ||
#ifdef RECOMP_RECTANGLE_DEPTH_STENCIL
        !recomp::gpu::setupRectangleDepthTarget(device,r))return false;
#elif defined(RECOMP_PERSISTENT_HALF_COLOR)
        !recomp::gpu::setupTextureFragmentTarget(device,r,ColorTarget,&r.loadPass))return false;
#else
        !recomp::gpu::setupTextureFragmentTarget(device,r,ColorTarget))return false;
#endif
#endif
#ifdef RECOMP_R8_PLANES_SAMPLE
    constexpr unsigned BindingCount=5;
#elif defined(RECOMP_TEXTURE32_SAMPLE)
    constexpr unsigned BindingCount=3;
#else
    constexpr unsigned BindingCount=2;
#endif
    std::array<VkDescriptorSetLayoutBinding,BindingCount> bindings{};
    for(unsigned i=0;i<2;++i){bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount=1;bindings[i].stageFlags=ShaderStage;}
#ifdef RECOMP_TEXTURE32_SAMPLE
    bindings[2]={2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,ShaderStage,nullptr};
#elif defined(RECOMP_R8_PLANES_SAMPLE)
    for(unsigned i=2;i<5;++i)bindings[i]={i,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,ShaderStage,nullptr};
#endif
    VkDescriptorSetLayoutCreateInfo set{};set.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set.bindingCount=BindingCount;set.pBindings=bindings.data();
    if(vkCreateDescriptorSetLayout(r.d,&set,nullptr,&r.setLayout)!=VK_SUCCESS)return false;
    VkPipelineLayoutCreateInfo layout{};layout.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount=1;layout.pSetLayouts=&r.setLayout;
    if(vkCreatePipelineLayout(r.d,&layout,nullptr,&r.layout)!=VK_SUCCESS)return false;
#ifdef RECOMP_RECTANGLE_DEPTH_STENCIL
    if(!recomp::gpu::createRectangleDepthPipeline(r))return false;
#elif defined(RECOMP_TEXTURE32_FRAGMENT)
    auto pipelineTarget=ColorTarget;
#ifdef RECOMP_PERSISTENT_HALF_COLOR
    if(r.disableBlend)pipelineTarget.alphaBlend=false;
#endif
    if(!recomp::gpu::createTextureFragmentPipeline(r,
#if defined(RECOMP_TEXTURE32_VERTEX_PAIR) && !defined(RECOMP_RECTANGLE_VERTEX_PAIR)
        true
#else
        false
#endif
        ,pipelineTarget))return false;
#else
    VkComputePipelineCreateInfo pipeline{};pipeline.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipeline.stage.module=r.shader;pipeline.stage.pName="main";
    pipeline.layout=r.layout;
    if(vkCreateComputePipelines(r.d,VK_NULL_HANDLE,1,&pipeline,nullptr,&r.pipeline)!=VK_SUCCESS)return false;
#endif
    std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,BindingCount>2?BindingCount-2:1}}};
    VkDescriptorPoolCreateInfo descriptor{};descriptor.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptor.maxSets=1;descriptor.poolSizeCount=BindingCount>2?2:1;descriptor.pPoolSizes=sizes.data();
    if(vkCreateDescriptorPool(r.d,&descriptor,nullptr,&r.descriptors)!=VK_SUCCESS)return false;
    VkDescriptorSetAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool=r.descriptors;alloc.descriptorSetCount=1;alloc.pSetLayouts=&r.setLayout;
    if(vkAllocateDescriptorSets(r.d,&alloc,&r.set)!=VK_SUCCESS)return false;
    std::array<VkDescriptorBufferInfo,2> data{{{r.buffers[0],0,inputBytes},{r.buffers[1],0,OutputBytes}}};
    std::array<VkWriteDescriptorSet,BindingCount> writes{};
    for(unsigned i=0;i<2;++i){auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet=r.set;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;w.pBufferInfo=&data[i];}
#ifdef RECOMP_TEXTURE32_SAMPLE
    VkDescriptorImageInfo imageBinding{r.sampler,r.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[2].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[2].dstSet=r.set;writes[2].dstBinding=2;
    writes[2].descriptorCount=1;writes[2].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo=&imageBinding;
#elif defined(RECOMP_R8_PLANES_SAMPLE)
    std::array<VkDescriptorImageInfo,3> imageBindings{};
    for(unsigned i=0;i<3;++i){
        imageBindings[i]={r.planes.sampler,r.planes.views[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        auto& w=writes[i+2];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet=r.set;w.dstBinding=i+2;w.descriptorCount=1;
        w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&imageBindings[i];
    }
#endif
    vkUpdateDescriptorSets(r.d,BindingCount,writes.data(),0,nullptr);
    VkCommandPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pool.queueFamilyIndex=device.queueFamily;
    if(vkCreateCommandPool(r.d,&pool,nullptr,&r.pool)!=VK_SUCCESS)return false;
    VkCommandBufferAllocateInfo command{};command.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command.commandPool=r.pool;command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command.commandBufferCount=1;
    if(vkAllocateCommandBuffers(r.d,&command,&r.command)!=VK_SUCCESS)return false;
    VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    return vkCreateFence(r.d,&fence,nullptr,&r.fence)==VK_SUCCESS;
}
bool check(recomp::gpu::ProbeDevice& device,Resources& r,const std::vector<std::uint8_t>& source,
        const std::vector<std::uint8_t>& expected,unsigned invocations) {
    std::memcpy(r.mapped[0],source.data(),source.size());std::memset(r.mapped[1],0x5A,OutputBytes);
    if(vkResetCommandPool(r.d,r.pool,0)!=VK_SUCCESS || vkResetFences(r.d,1,&r.fence)!=VK_SUCCESS)return false;
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return false;
    VkMemoryBarrier before{};before.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;before.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
#ifdef RECOMP_TEXTURE32_SAMPLE
    before.dstAccessMask|=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|PipelineStage,0,1,&before,0,nullptr,0,nullptr);
    VkImageMemoryBarrier upload{};upload.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    upload.srcAccessMask=r.imageReady?VK_ACCESS_SHADER_READ_BIT:0;upload.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    upload.oldLayout=r.imageReady?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
    upload.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    upload.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;upload.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    upload.image=r.image;upload.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(r.command,r.imageReady?PipelineStage:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&upload);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={32,32,1};
    if(r.replaceImageControl){const VkClearColorValue zero{};
        vkCmdClearColorImage(r.command,r.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&upload.subresourceRange);}
    else vkCmdCopyBufferToImage(r.command,r.buffers[0],r.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    upload.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;upload.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    upload.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;upload.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,PipelineStage,0,0,nullptr,0,nullptr,1,&upload);
#elif defined(RECOMP_R8_PLANES_SAMPLE)
    before.dstAccessMask|=VK_ACCESS_TRANSFER_READ_BIT;
#ifdef RECOMP_R8_PLANES_FRAGMENT
    before.dstAccessMask|=VK_ACCESS_TRANSFER_WRITE_BIT;
#endif
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|PipelineStage,
        0,1,&before,0,nullptr,0,nullptr);
    r.planes.upload(r.command,r.buffers[0],PipelineStage);
#else
#ifdef RECOMP_RECTANGLE_VERTEX_PAIR
    before.dstAccessMask|=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,PipelineStage|VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&before,0,nullptr,0,nullptr);
#else
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,PipelineStage,0,1,&before,0,nullptr,0,nullptr);
#endif
#endif
#ifdef RECOMP_TEXTURE32_FRAGMENT
    VkClearValue clear{};
#ifdef RECOMP_RECTANGLE_DEPTH_STENCIL
    clear.depthStencil={0.125f,0xA5u};
#elif defined(RECOMP_FULLSIZE_HALF_COLOR)
    // Exactly representable binary16 value with storage bits 0x5A5A.
    for(auto& channel:clear.color.float32)channel=203.25f;
#else
    std::memset(&clear.color,0x5A,sizeof(clear.color));
#endif
    VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass=r.renderPass;pass.framebuffer=r.framebuffer;
#ifdef RECOMP_PERSISTENT_HALF_COLOR
    if(r.sequenceStep && !r.forceClear)pass.renderPass=r.loadPass;
#endif
    pass.renderArea={{0,0},{ColorTarget.width,ColorTarget.height}};
    pass.clearValueCount=1;pass.pClearValues=&clear;
    vkCmdBeginRenderPass(r.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipeline);
    vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.layout,0,1,&r.set,0,nullptr);
#ifdef RECOMP_RECTANGLE_VERTEX_PAIR
    constexpr unsigned DrawVertices=6;
#elif defined(RECOMP_TEXTURE32_VERTEX_PAIR)
    constexpr unsigned DrawVertices=4;
#else
    constexpr unsigned DrawVertices=3;
#endif
    for(unsigned draw=0;draw<invocations;++draw)vkCmdDraw(r.command,DrawVertices,1,0,0);
    vkCmdEndRenderPass(r.command);
    VkBufferImageCopy readback{};readback.bufferOffset=16;
#ifdef RECOMP_RECTANGLE_DEPTH_STENCIL
    readback.imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};readback.imageExtent={32,32,1};
    vkCmdCopyImageToBuffer(r.command,r.colorImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.buffers[1],1,&readback);
    readback.bufferOffset=4128;readback.imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;
#else
    readback.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    readback.imageExtent={ColorTarget.width,ColorTarget.height,1};
#endif
    vkCmdCopyImageToBuffer(r.command,r.colorImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.buffers[1],1,&readback);
    VkMemoryBarrier after{};after.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,nullptr,0,nullptr);
#else
    vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.pipeline);
    vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.layout,0,1,&r.set,0,nullptr);
#if defined(RECOMP_TILED_COLOR_COPY) || defined(RECOMP_HALF_COLOR_STORE) || defined(RECOMP_HALF_COLOR_LOAD)
    for(unsigned i=0;i<invocations;++i){
        if(i){VkMemoryBarrier repeat{};repeat.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            repeat.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;repeat.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(r.command,PipelineStage,PipelineStage,0,1,&repeat,0,nullptr,0,nullptr);}
        vkCmdDispatch(r.command,160,90,1);
    }
#else
    if(invocations)vkCmdDispatch(r.command,invocations,1,1);
#endif
    VkMemoryBarrier after{};after.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(r.command,PipelineStage,VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,nullptr,0,nullptr);
#endif
    if(vkEndCommandBuffer(r.command)!=VK_SUCCESS)return false;
    const auto* output=static_cast<const std::uint8_t*>(r.mapped[1]);
    for(unsigned i=0;i<OutputBytes;++i)if(output[i]!=0x5A)return false;
    if(vkGetFenceStatus(r.d,r.fence)!=VK_NOT_READY)return false;
    VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
    if(vkQueueSubmit(device.queue,1,&submit,r.fence)!=VK_SUCCESS ||
        vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS){std::fflush(stdout);std::_Exit(4);}
#ifdef RECOMP_TEXTURE32_SAMPLE
    r.imageReady=true;
#elif defined(RECOMP_R8_PLANES_SAMPLE)
    r.planes.ready=true;
#endif
#if defined(RECOMP_TILED_COLOR_COPY) || defined(RECOMP_FULLSIZE_HALF_COLOR) || defined(RECOMP_HALF_COLOR_STORE) || defined(RECOMP_HALF_COLOR_LOAD)
    std::vector<std::uint8_t> match(OutputBytes,0x5A);
#else
    std::array<std::uint8_t,OutputBytes> match{};match.fill(0x5A);
#endif
#ifdef RECOMP_PERSISTENT_HALF_COLOR
    if(r.sequenceStep>=5)return false;
    for(unsigned i=0;i<MaxVertices;++i)std::memcpy(match.data()+16+i*8,expected.data()+16+r.sequenceStep*8,8);
    ++r.sequenceStep;
#elif defined(RECOMP_RECTANGLE_DEPTH_STENCIL)
    if(invocations)std::memcpy(match.data()+16,expected.data()+16,OutputBytes-32);
    else {
        constexpr float clearedDepth=0.125f;
        for(unsigned i=0;i<MaxVertices;++i)std::memcpy(match.data()+16+i*4,&clearedDepth,4);
        std::memset(match.data()+4128,0xA5,MaxVertices);
    }
#elif defined(RECOMP_TEXTURE32_FRAGMENT)
    if(invocations)std::memcpy(match.data()+16,expected.data()+16,VertexBytes*MaxVertices);
#else
    if(invocations)std::memcpy(match.data()+16,expected.data()+16,VertexBytes*std::min(invocations,MaxVertices));
#endif
    const bool okay=std::memcmp(output,match.data(),OutputBytes)==0 && std::memcmp(r.mapped[0],source.data(),source.size())==0;
    if(!okay)for(unsigned i=0;i<OutputBytes;++i)if(output[i]!=match[i]){
        std::uint32_t actualWord=0,expectedWord=0;const unsigned at=i&~3u;
        std::memcpy(&actualWord,output+at,4);std::memcpy(&expectedWord,match.data()+at,4);
        std::printf("%s_MISMATCH byte=%u actual_word=%08x expected_word=%08x\n",Profile,i,actualWord,expectedWord);break;
    }
#ifdef RECOMP_TEXTURE32_FRAGMENT
    std::printf("%s_CASE draws=%u checked_output=%u checked_input=%zu fence=1 matched=%u title_raster_state_replayed=0 guest_draws=0\n",
#else
    std::printf("%s_CASE invocations=%u checked_output=%u checked_input=%zu fence=1 matched=%u native_vertex_pipeline=0 draws=0\n",
#endif
        Profile,invocations,OutputBytes,source.size(),unsigned(okay));return okay;
}
}
int main(int argc,char** argv){
#ifdef RECOMP_TEXTURE32_FRAGMENT
    constexpr int RequiredArgc=5,InputArgument=3;
#else
    constexpr int RequiredArgc=4,InputArgument=2;
#endif
#ifdef RECOMP_PERSISTENT_HALF_COLOR
    const bool forceClear=argc==RequiredArgc+1 && std::strcmp(argv[RequiredArgc],"--clear-every-pass-control")==0;
    const bool disableBlend=argc==RequiredArgc+1 && std::strcmp(argv[RequiredArgc],"--disable-alpha-blend-control")==0;
#else
    constexpr bool forceClear=false,disableBlend=false;
#endif
    const bool corrupt=argc==RequiredArgc+1 && std::strcmp(argv[RequiredArgc],"--corrupt-expected-control")==0;
#ifdef RECOMP_RECTANGLE_DEPTH_STENCIL
    const bool noDepth=argc==RequiredArgc+1 && std::strcmp(argv[RequiredArgc],"--disable-depth-write-control")==0;
    const bool noStencil=argc==RequiredArgc+1 && std::strcmp(argv[RequiredArgc],"--disable-stencil-write-control")==0;
    const bool wrongStencil=argc==RequiredArgc+1 && std::strcmp(argv[RequiredArgc],"--wrong-stencil-reference-control")==0;
#else
    constexpr bool noDepth=false,noStencil=false,wrongStencil=false;
#endif
#ifdef RECOMP_TEXTURE32_SAMPLE
    const bool replaceImage=argc==RequiredArgc+1 && std::strcmp(argv[RequiredArgc],"--replace-upload-with-zero-control")==0;
#else
    const bool replaceImage=false;
#endif
#ifdef RECOMP_R8_PLANES_SAMPLE
    int zeroPlane=-1;
    if(argc==RequiredArgc+1)for(unsigned i=0;i<3;++i){
        const std::array<const char*,3> names{{"--zero-upload-plane-0-control","--zero-upload-plane-1-control","--zero-upload-plane-2-control"}};
        if(std::strcmp(argv[RequiredArgc],names[i])==0)zeroPlane=static_cast<int>(i);
    }
#else
    constexpr int zeroPlane=-1;
#endif
    if((argc!=RequiredArgc && !corrupt && !replaceImage && zeroPlane<0 && !noDepth && !noStencil && !wrongStencil && !forceClear && !disableBlend) ||
        !recomp::gpu::initializeProbeProcess())return 2;
    std::vector<std::uint8_t> module,input,expected;
    if(!recomp::gpu::readInput(argv[1],module,65536) || module.size()<20 || module.size()%4 ||
        !recomp::gpu::readInput(argv[InputArgument],input,InputLimit) || input.size()%4 ||
        !recomp::gpu::readInput(argv[InputArgument+1],expected,ExpectedFileBytes) || expected.size()!=ExpectedFileBytes)return 2;
#ifdef RECOMP_R8_PLANES_SAMPLE
    if(input.size()<recomp::gpu::R8PlaneImages::ImageBytes)return 2;
#elif defined(RECOMP_TEXTURE32_PIXEL)
    if(input.size()<4096)return 2; // Complete upload required; shader guards missing constants.
#elif defined(RECOMP_TEXTURE32_SAMPLE)
    if(input.size()!=4096)return 2;
#endif
    std::vector<std::uint32_t> code(module.size()/4);std::memcpy(code.data(),module.data(),module.size());
    if(code[0]!=0x07230203)return 2;
    std::vector<std::uint32_t> vertexCode;
#ifdef RECOMP_TEXTURE32_FRAGMENT
    std::vector<std::uint8_t> vertex;
    if(!recomp::gpu::readInput(argv[2],vertex,65536) || vertex.size()<20 || vertex.size()%4)return 2;
    vertexCode.resize(vertex.size()/4);std::memcpy(vertexCode.data(),vertex.data(),vertex.size());
    if(vertexCode[0]!=0x07230203)return 2;
#endif
    for(unsigned i=0;i<16;++i)if(expected[i]!=0x5A || expected[ExpectedFileBytes-16+i]!=0x5A)return 2;
#ifdef RECOMP_RECTANGLE_DEPTH_STENCIL
    for(unsigned i=4112;i<4128;++i)if(expected[i]!=0x5A)return 2;
#endif
#ifdef RECOMP_PERSISTENT_HALF_COLOR
    if(corrupt)expected[24]^=1;
#else
    if(corrupt)expected[16]^=1;
#endif
    recomp::gpu::ProbeDevice device;if(!device.inspect() || !device.create({}))return 1;
    std::uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(device.physical,&count,nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);vkGetPhysicalDeviceQueueFamilyProperties(device.physical,&count,queues.data());
#ifdef RECOMP_TEXTURE32_FRAGMENT
    constexpr auto QueueFlag=VK_QUEUE_GRAPHICS_BIT;
#else
    constexpr auto QueueFlag=VK_QUEUE_COMPUTE_BIT;
#endif
    if(device.queueFamily>=count || !(queues[device.queueFamily].queueFlags&QueueFlag))return 2;
    Resources resources(device.device);
#ifdef RECOMP_PERSISTENT_HALF_COLOR
    resources.forceClear=forceClear;resources.disableBlend=disableBlend;
#endif
#ifdef RECOMP_RECTANGLE_DEPTH_STENCIL
    resources.depthWrite=!noDepth;resources.stencilWrite=!noStencil;
    resources.stencilReference=wrongStencil?0x3Cu:0u;
#endif
    if(!setup(device,resources,code,input.size(),vertexCode))return 1;
#ifdef RECOMP_TEXTURE32_SAMPLE
    resources.replaceImageControl=replaceImage;
#elif defined(RECOMP_R8_PLANES_SAMPLE)
    resources.planes.zeroPlane=zeroPlane;
#endif
#ifdef RECOMP_PERSISTENT_HALF_COLOR
    for(unsigned draws:{0u,1u,2u,3u,0u})if(!check(device,resources,input,expected,draws))return 1;
#elif defined(RECOMP_TEXTURE32_FRAGMENT) || defined(RECOMP_TILED_COLOR_COPY) || defined(RECOMP_HALF_COLOR_STORE) || defined(RECOMP_HALF_COLOR_LOAD)
    for(unsigned draws:{0u,1u,2u,3u})if(!check(device,resources,input,expected,draws))return 1;
#else
    for(unsigned invocations:{0u,1u,MaxVertices,MaxVertices+1})if(!check(device,resources,input,expected,invocations))return 1;
#endif
#ifdef RECOMP_TEXTURE32_SAMPLE
#ifdef RECOMP_PERSISTENT_HALF_COLOR
    std::puts("TEXTURE32_UPLOAD_V1 images=1 uploads=5 native_repeat_linear_lod0=1 xenos_sampler_verified=0");
#else
    std::puts("TEXTURE32_UPLOAD_V1 images=1 uploads=4 native_repeat_linear_lod0=1 xenos_sampler_verified=0");
#endif
#elif defined(RECOMP_R8_PLANES_SAMPLE)
#ifdef RECOMP_PERSISTENT_HALF_COLOR
    std::puts("R8_PLANES_UPLOAD_V1 images=3 uploads=15 native_repeat_linear_lod0=1 output_swizzle_rrr1=1 xenos_sampler_verified=0");
#else
    std::puts("R8_PLANES_UPLOAD_V1 images=3 uploads=12 native_repeat_linear_lod0=1 output_swizzle_rrr1=1 xenos_sampler_verified=0");
#endif
#endif
#ifdef RECOMP_RECTANGLE_DEPTH_STENCIL
    std::puts("DEPTH_STENCIL_STORAGE_V1 format=D32_SFLOAT_S8_UINT native_aspect_copies=2 pixel_shader_bound=0 xenos_edram=0");
#endif
#ifdef RECOMP_FULLSIZE_HALF_COLOR
    std::puts("HALF_COLOR_STORAGE_V1 width=1280 height=720 format=R16G16B16A16_SFLOAT rounding=test_expectation retail_format12_verified=0");
#endif
#ifdef RECOMP_PERSISTENT_HALF_COLOR
    std::printf("%s_V1 fresh_fences=5 explicit_alpha_blend=1 color_preserved=1 game_frame=0 guest_writeback=0\n",Profile);return 0;
#else
    std::printf("%s_V1 fresh_fences=4 game_frame=0 guest_writeback=0\n",Profile);return 0;
#endif
}
