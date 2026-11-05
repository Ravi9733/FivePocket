#pragma once
// Read-only standalone diagnostic. Restores TRANSFER_SRC layout after a fence.
#include "native_color8_clear_target.h"
#include <cassert>
#include <fstream>
using namespace recomp::gpu;
struct SampleReader {
    ProbeDevice& d;VkSampler sampler{};VkImageView view{};VkBuffer output{};VkDeviceMemory memory{};void* mapped{};
    VkShaderModule shader{};VkDescriptorSetLayout setLayout{};VkPipelineLayout layout{};VkPipeline pipeline{};
    VkDescriptorPool descriptors{};VkDescriptorSet set{};VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
    unsigned width,height,samples,bytes;VkImage image;
    VkImageAspectFlags aspects;
    VkImageLayout retainedLayout;
    bool ownsView=true;
    SampleReader(ProbeDevice& device,NativeColor8ClearTarget& target,const char* path,VkImageAspectFlagBits aspect=VK_IMAGE_ASPECT_COLOR_BIT):
        SampleReader(device,target.plan().width,target.plan().height,target.plan().samples,
            aspect==VK_IMAGE_ASPECT_COLOR_BIT?target.borrowSamplesForReadOnlyDiagnostic():target.borrowDepthForReadOnlyDiagnostic(),
            aspect==VK_IMAGE_ASPECT_COLOR_BIT?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_D32_SFLOAT_S8_UINT,aspect,path) {}
    SampleReader(ProbeDevice& device,unsigned w,unsigned h,unsigned n,VkImage source,VkFormat format,
            VkImageAspectFlagBits aspect,const char* path,unsigned components=1,
            VkImageLayout sourceLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VkImageView borrowedView=VK_NULL_HANDLE):d(device),
        width(w),height(h),samples(n),bytes(w*h*n*components*4+32),image(source),
        aspects(aspect==VK_IMAGE_ASPECT_COLOR_BIT?VK_IMAGE_ASPECT_COLOR_BIT:VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT),
        retainedLayout(sourceLayout),ownsView(borrowedView==VK_NULL_HANDLE) {
        assert(retainedLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || retainedLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        assert(w && h && w<=1280 && h<=720 && (n==1 || n==2 || n==4) && (components==1 || components==5));
        assert(image);std::ifstream f(path,std::ios::binary|std::ios::ate);assert(f);
        const auto size=static_cast<unsigned>(f.tellg());assert(size && size%4==0 && size<65536);
        std::vector<unsigned> code(size/4);f.seekg(0);assert(f.read(reinterpret_cast<char*>(code.data()),size));
        VkShaderModuleCreateInfo sm{};sm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;sm.codeSize=size;sm.pCode=code.data();
        assert(vkCreateShaderModule(d.device,&sm,nullptr,&shader)==VK_SUCCESS);
        VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;v.image=image;
        v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=format;
        v.subresourceRange={static_cast<VkImageAspectFlags>(aspect),0,1,0,1};
        if(borrowedView)view=borrowedView;
        else assert(vkCreateImageView(d.device,&v,nullptr,&view)==VK_SUCCESS);
        VkSamplerCreateInfo s{};s.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        s.magFilter=s.minFilter=VK_FILTER_NEAREST;s.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        s.addressModeU=s.addressModeV=s.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        assert(vkCreateSampler(d.device,&s,nullptr,&sampler)==VK_SUCCESS);
        VkBufferCreateInfo b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;b.size=bytes;b.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        assert(vkCreateBuffer(d.device,&b,nullptr,&output)==VK_SUCCESS);
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(d.device,output,&req);
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(d.physical,&props);
        unsigned type=props.memoryTypeCount;
        for(unsigned i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i)) &&
            (props.memoryTypes[i].propertyFlags&6)==6){type=i;break;}
        assert(type<props.memoryTypeCount);
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=req.size;a.memoryTypeIndex=type;
        assert(vkAllocateMemory(d.device,&a,nullptr,&memory)==VK_SUCCESS);
        assert(vkBindBufferMemory(d.device,output,memory,0)==VK_SUCCESS);
        assert(vkMapMemory(d.device,memory,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS);
        const VkDescriptorSetLayoutBinding bindings[]{
            {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo sl{};sl.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sl.bindingCount=2;sl.pBindings=bindings;assert(vkCreateDescriptorSetLayout(d.device,&sl,nullptr,&setLayout)==VK_SUCCESS);
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,12};
        VkPipelineLayoutCreateInfo pl{};pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount=1;pl.pSetLayouts=&setLayout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
        assert(vkCreatePipelineLayout(d.device,&pl,nullptr,&layout)==VK_SUCCESS);
        VkComputePipelineCreateInfo p{};p.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;p.layout=layout;
        p.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        p.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;p.stage.module=shader;p.stage.pName="main";
        assert(vkCreateComputePipelines(d.device,VK_NULL_HANDLE,1,&p,nullptr,&pipeline)==VK_SUCCESS);
        const VkDescriptorPoolSize counts[]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
        VkDescriptorPoolCreateInfo dp{};dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dp.maxSets=1;dp.poolSizeCount=2;dp.pPoolSizes=counts;
        assert(vkCreateDescriptorPool(d.device,&dp,nullptr,&descriptors)==VK_SUCCESS);
        VkDescriptorSetAllocateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ds.descriptorPool=descriptors;ds.descriptorSetCount=1;ds.pSetLayouts=&setLayout;
        assert(vkAllocateDescriptorSets(d.device,&ds,&set)==VK_SUCCESS);
        const VkDescriptorImageInfo di{sampler,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo bi{output,0,bytes};std::array<VkWriteDescriptorSet,2> writes{};
        for(unsigned i=0;i<2;++i){auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet=set;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=counts[i].type;}
        writes[0].pImageInfo=&di;writes[1].pBufferInfo=&bi;vkUpdateDescriptorSets(d.device,2,writes.data(),0,nullptr);
        VkCommandPoolCreateInfo cp{};cp.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;cp.queueFamilyIndex=d.queueFamily;
        assert(vkCreateCommandPool(d.device,&cp,nullptr,&pool)==VK_SUCCESS);
        VkCommandBufferAllocateInfo cb{};cb.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;cb.commandPool=pool;
        cb.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cb.commandBufferCount=1;
        assert(vkAllocateCommandBuffers(d.device,&cb,&command)==VK_SUCCESS);
        VkFenceCreateInfo fc{};fc.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        assert(vkCreateFence(d.device,&fc,nullptr,&fence)==VK_SUCCESS);
    }
    std::vector<std::uint32_t> readWords() {
        assert(vkResetCommandPool(d.device,pool,0)==VK_SUCCESS);assert(vkResetFences(d.device,1,&fence)==VK_SUCCESS);
        std::memset(mapped,0x5A,bytes);
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        assert(vkBeginCommandBuffer(command,&begin)==VK_SUCCESS);
        VkImageMemoryBarrier imageBarrier{};imageBarrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imageBarrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT;imageBarrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        imageBarrier.oldLayout=retainedLayout;imageBarrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if(retainedLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)imageBarrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;
        imageBarrier.srcQueueFamilyIndex=imageBarrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        imageBarrier.image=image;imageBarrier.subresourceRange={aspects,0,1,0,1};
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,1,&host,0,nullptr,1,&imageBarrier);
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
        const unsigned shape[]{width,height,samples};vkCmdPushConstants(command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,12,shape);
        vkCmdDispatch(command,(width+7)/8,(height+7)/8,1);
        imageBarrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;imageBarrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT;
        imageBarrier.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;imageBarrier.newLayout=retainedLayout;
        if(retainedLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)imageBarrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT|VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,1,&host,0,nullptr,1,&imageBarrier);
        assert(vkEndCommandBuffer(command)==VK_SUCCESS);
        const auto* raw=static_cast<const std::uint8_t*>(mapped);
        assert(std::all_of(raw,raw+bytes,[](auto b){return b==0x5A;}));
        assert(vkGetFenceStatus(d.device,fence)==VK_NOT_READY);
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        if(vkQueueSubmit(d.queue,1,&submit,fence)!=VK_SUCCESS ||
            vkWaitForFences(d.device,1,&fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        assert(std::all_of(raw,raw+16,[](auto b){return b==0x5A;}));
        assert(std::all_of(raw+bytes-16,raw+bytes,[](auto b){return b==0x5A;}));
        std::vector<std::uint32_t> result((bytes-32)/4);std::memcpy(result.data(),raw+16,bytes-32);
        return result;
    }
    void checkWord(std::uint32_t expected) {
        const auto result=readWords();for(auto word:result)assert(word==expected);
    }
    void check(const std::array<std::uint8_t,4>& b) {
        checkWord(std::uint32_t(b[0])|(std::uint32_t(b[1])<<8)|(std::uint32_t(b[2])<<16)|(std::uint32_t(b[3])<<24));
    }
    SampleReader(const SampleReader&)=delete;
    SampleReader& operator=(const SampleReader&)=delete;
    ~SampleReader() {
        vkDestroyFence(d.device,fence,nullptr);vkDestroyCommandPool(d.device,pool,nullptr);
        vkDestroyDescriptorPool(d.device,descriptors,nullptr);vkDestroyPipeline(d.device,pipeline,nullptr);
        vkDestroyPipelineLayout(d.device,layout,nullptr);vkDestroyDescriptorSetLayout(d.device,setLayout,nullptr);
        vkDestroyShaderModule(d.device,shader,nullptr);vkDestroySampler(d.device,sampler,nullptr);
        if(ownsView)vkDestroyImageView(d.device,view,nullptr);
        vkUnmapMemory(d.device,memory);vkDestroyBuffer(d.device,output,nullptr);vkFreeMemory(d.device,memory,nullptr);
    }
};
