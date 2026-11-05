// Standalone depth raster -> canonical packing -> reference color word readback.
// No title fixed-function replay, predicate execution, resolve or guest writeback.
#include "device.h"
#include "private_input.h"
#include "process_limits.h"
#include "rectangle_depth_target.h"
#include "xenos_edram_alias_clear.h"
#ifdef RECOMP_EDRAM_COLOR_RESOLVE
#include "xenos_color_copy.h"
#endif
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

namespace {
constexpr unsigned BackingBytes=recomp::gpu::AliasEdramBytes+32;
#ifdef RECOMP_EDRAM_COLOR_RESOLVE
constexpr unsigned ColorBytes=1280*736*4+32;
#else
constexpr unsigned ColorBytes=1280*720*4+32;
#endif
constexpr unsigned ExpectedBytes=BackingBytes+ColorBytes;
struct Resources {
    VkDevice d;
    bool depthWrite=true,stencilWrite=true;
    unsigned stencilReference=0;
    std::array<VkBuffer,3> buffers{};
    std::array<VkDeviceMemory,3> memory{};
    std::array<void*,3> mapped{};
    VkShaderModule vertexShader{};
    std::array<VkShaderModule,2> computeModules{};
    std::array<VkPipeline,2> computePipelines{};
    VkPipeline pipeline{};VkPipelineLayout layout{};
    VkDescriptorSetLayout setLayout{};VkDescriptorPool descriptors{};VkDescriptorSet set{};
    VkImage colorImage{};VkDeviceMemory colorMemory{};VkImageView colorView{};
    std::array<VkImageView,2> sampledViews{};
    VkRenderPass renderPass{};VkFramebuffer framebuffer{};
    VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
    explicit Resources(VkDevice device):d(device){}
    ~Resources(){
        if(fence)vkDestroyFence(d,fence,nullptr);
        if(pool)vkDestroyCommandPool(d,pool,nullptr);
        if(pipeline)vkDestroyPipeline(d,pipeline,nullptr);
        for(auto p:computePipelines)if(p)vkDestroyPipeline(d,p,nullptr);
        if(layout)vkDestroyPipelineLayout(d,layout,nullptr);
        if(descriptors)vkDestroyDescriptorPool(d,descriptors,nullptr);
        if(setLayout)vkDestroyDescriptorSetLayout(d,setLayout,nullptr);
        if(vertexShader)vkDestroyShaderModule(d,vertexShader,nullptr);
        for(auto m:computeModules)if(m)vkDestroyShaderModule(d,m,nullptr);
        if(framebuffer)vkDestroyFramebuffer(d,framebuffer,nullptr);
        if(renderPass)vkDestroyRenderPass(d,renderPass,nullptr);
        for(auto v:sampledViews)if(v)vkDestroyImageView(d,v,nullptr);
        if(colorView)vkDestroyImageView(d,colorView,nullptr);
        if(colorImage)vkDestroyImage(d,colorImage,nullptr);
        if(colorMemory)vkFreeMemory(d,colorMemory,nullptr);
        for(unsigned i=0;i<3;++i){
            if(mapped[i])vkUnmapMemory(d,memory[i]);
            if(buffers[i])vkDestroyBuffer(d,buffers[i],nullptr);
            if(memory[i])vkFreeMemory(d,memory[i],nullptr);
        }
    }
};

bool shader(VkDevice device,const std::vector<std::uint32_t>& words,VkShaderModule& out){
    VkShaderModuleCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize=words.size()*4;info.pCode=words.data();
    return vkCreateShaderModule(device,&info,nullptr,&out)==VK_SUCCESS;
}
bool setup(recomp::gpu::ProbeDevice& device,Resources& r,const std::array<std::vector<std::uint32_t>,3>& code,
        unsigned inputBytes,VkSampleMask sampleMask){
    const auto& limits=device.properties.limits;
    if(limits.maxStorageBufferRange<BackingBytes || limits.maxComputeWorkGroupInvocations<64 ||
       limits.maxComputeWorkGroupSize[0]<8 || limits.maxComputeWorkGroupSize[1]<8 ||
       limits.maxComputeWorkGroupCount[0]<160 || limits.maxComputeWorkGroupCount[1]<90 ||
       !(limits.sampledImageDepthSampleCounts&VK_SAMPLE_COUNT_4_BIT) ||
       !(limits.sampledImageStencilSampleCounts&VK_SAMPLE_COUNT_4_BIT))return false;
    VkPhysicalDeviceMemoryProperties properties{};vkGetPhysicalDeviceMemoryProperties(device.physical,&properties);
    const std::array<unsigned,3> sizes{inputBytes,BackingBytes,ColorBytes};
    for(unsigned i=0;i<3;++i){
        VkBufferCreateInfo buffer{};buffer.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size=sizes[i];buffer.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if(vkCreateBuffer(r.d,&buffer,nullptr,&r.buffers[i])!=VK_SUCCESS)return false;
        VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(r.d,r.buffers[i],&requirements);
        unsigned type=properties.memoryTypeCount;
        constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for(unsigned n=0;n<properties.memoryTypeCount;++n)if((requirements.memoryTypeBits&(1u<<n)) &&
            (properties.memoryTypes[n].propertyFlags&flags)==flags){type=n;break;}
        if(type==properties.memoryTypeCount)return false;
        VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=type;
        if(vkAllocateMemory(r.d,&allocation,nullptr,&r.memory[i])!=VK_SUCCESS ||
           vkBindBufferMemory(r.d,r.buffers[i],r.memory[i],0)!=VK_SUCCESS ||
           vkMapMemory(r.d,r.memory[i],0,VK_WHOLE_SIZE,0,&r.mapped[i])!=VK_SUCCESS)return false;
    }
    const recomp::gpu::RectangleDepthConfig config{640,360,VK_SAMPLE_COUNT_4_BIT,true,sampleMask};
    if(!shader(r.d,code[0],r.vertexShader) || !recomp::gpu::setupRectangleDepthTarget(device,r,config))return false;
    for(unsigned i=0;i<2;++i){
        VkImageViewCreateInfo view{};view.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image=r.colorImage;view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=VK_FORMAT_D32_SFLOAT_S8_UINT;
        view.subresourceRange={i?VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
        if(vkCreateImageView(r.d,&view,nullptr,&r.sampledViews[i])!=VK_SUCCESS ||
            !shader(r.d,code[i+1],r.computeModules[i]))return false;
    }
    std::array<VkDescriptorSetLayoutBinding,5> bindings{};
    for(unsigned i=0;i<5;++i)bindings[i]={i,i<3?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        1,i==0?VK_SHADER_STAGE_VERTEX_BIT:VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo set{};set.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set.bindingCount=bindings.size();set.pBindings=bindings.data();
    if(vkCreateDescriptorSetLayout(r.d,&set,nullptr,&r.setLayout)!=VK_SUCCESS)return false;
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,8};
    VkPipelineLayoutCreateInfo layout{};layout.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount=1;layout.pSetLayouts=&r.setLayout;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&push;
    if(vkCreatePipelineLayout(r.d,&layout,nullptr,&r.layout)!=VK_SUCCESS ||
        !recomp::gpu::createRectangleDepthPipeline(r,config))return false;
    for(unsigned i=0;i<2;++i){
        VkComputePipelineCreateInfo pipeline{};pipeline.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipeline.stage.module=r.computeModules[i];pipeline.stage.pName="main";
        pipeline.layout=r.layout;
        if(vkCreateComputePipelines(r.d,VK_NULL_HANDLE,1,&pipeline,nullptr,&r.computePipelines[i])!=VK_SUCCESS)return false;
    }
    const std::array<VkDescriptorPoolSize,2> counts{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2}}};
    VkDescriptorPoolCreateInfo pool{};pool.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets=1;pool.poolSizeCount=counts.size();pool.pPoolSizes=counts.data();
    if(vkCreateDescriptorPool(r.d,&pool,nullptr,&r.descriptors)!=VK_SUCCESS)return false;
    VkDescriptorSetAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool=r.descriptors;allocation.descriptorSetCount=1;allocation.pSetLayouts=&r.setLayout;
    if(vkAllocateDescriptorSets(r.d,&allocation,&r.set)!=VK_SUCCESS)return false;
    std::array<VkDescriptorBufferInfo,3> buffers{};std::array<VkDescriptorImageInfo,2> images{};
    std::array<VkWriteDescriptorSet,5> writes{};
    for(unsigned i=0;i<5;++i){
        auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet=r.set;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=bindings[i].descriptorType;
        if(i<3){buffers[i]={r.buffers[i],0,sizes[i]};w.pBufferInfo=&buffers[i];}
        else {images[i-3]={VK_NULL_HANDLE,r.sampledViews[i-3],VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};w.pImageInfo=&images[i-3];}
    }
    vkUpdateDescriptorSets(r.d,writes.size(),writes.data(),0,nullptr);
    VkCommandPoolCreateInfo commandPool{};commandPool.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPool.queueFamilyIndex=device.queueFamily;
    if(vkCreateCommandPool(r.d,&commandPool,nullptr,&r.pool)!=VK_SUCCESS)return false;
    VkCommandBufferAllocateInfo command{};command.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command.commandPool=r.pool;command.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;command.commandBufferCount=1;
    if(vkAllocateCommandBuffers(r.d,&command,&r.command)!=VK_SUCCESS)return false;
    VkFenceCreateInfo fence{};fence.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    return vkCreateFence(r.d,&fence,nullptr,&r.fence)==VK_SUCCESS;
}

std::vector<std::uint8_t> clearReference(){
    std::vector<std::uint8_t> result(ExpectedBytes,0x5A);
    constexpr std::uint32_t word=0x200000A5;
    for(unsigned i=3686400+16;i<7372800+16;i+=4)std::memcpy(result.data()+i,&word,4);
#ifdef RECOMP_EDRAM_COLOR_RESOLVE
    if(!recomp::gpu::copyColorToOwnedBuffer({0x14000500,0xC02D0,1280,720,false},0x100000,0x1F2D8000,
        0x2D00500,0x1000300,result.data()+16,recomp::gpu::AliasEdramBytes,
        result.data()+BackingBytes+16,ColorBytes-32))return {};
#else
    for(unsigned i=BackingBytes+16;i<ExpectedBytes-16;i+=4)std::memcpy(result.data()+i,&word,4);
#endif
    return result;
}
bool check(recomp::gpu::ProbeDevice& device,Resources& r,const std::vector<std::uint8_t>& source,
        const std::vector<std::uint8_t>& expected,unsigned draws,const std::array<unsigned,2>& controls){
    std::memcpy(r.mapped[0],source.data(),source.size());
    std::memset(r.mapped[1],0x5A,BackingBytes);std::memset(r.mapped[2],0x5A,ColorBytes);
    if(vkResetCommandPool(r.d,r.pool,0)!=VK_SUCCESS || vkResetFences(r.d,1,&r.fence)!=VK_SUCCESS)return false;
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return false;
    VkMemoryBarrier before{};before.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;before.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,1,&before,0,nullptr,0,nullptr);
    VkClearValue clear{};clear.depthStencil={.125f,0xA5u};
    VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass=r.renderPass;pass.framebuffer=r.framebuffer;pass.renderArea={{0,0},{640,360}};
    pass.clearValueCount=1;pass.pClearValues=&clear;
    vkCmdBeginRenderPass(r.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipeline);
    vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.layout,0,1,&r.set,0,nullptr);
    for(unsigned i=0;i<draws;++i)vkCmdDraw(r.command,6,1,0,0);
    vkCmdEndRenderPass(r.command);
    vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.computePipelines[0]);
    vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.layout,0,1,&r.set,0,nullptr);
    vkCmdPushConstants(r.command,r.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,controls.data());
    vkCmdDispatch(r.command,80,45,1);
    VkMemoryBarrier packed{};packed.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    packed.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;packed.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,1,&packed,0,nullptr,0,nullptr);
    vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_COMPUTE,r.computePipelines[1]);
    vkCmdDispatch(r.command,160,90,1);
    VkMemoryBarrier after{};after.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&after,0,nullptr,0,nullptr);
    if(vkEndCommandBuffer(r.command)!=VK_SUCCESS || vkGetFenceStatus(r.d,r.fence)!=VK_NOT_READY)return false;
    for(unsigned b=1;b<3;++b){
        const auto* data=static_cast<const std::uint8_t*>(r.mapped[b]);
        for(unsigned i=0;i<(b==1?BackingBytes:ColorBytes);++i)if(data[i]!=0x5A)return false;
    }
    VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
    if(vkQueueSubmit(device.queue,1,&submit,r.fence)!=VK_SUCCESS ||
        vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS){std::fflush(stdout);std::_Exit(4);}
    bool okay=std::memcmp(r.mapped[0],source.data(),source.size())==0;
    for(unsigned b=1;b<3;++b){
        const auto* actual=static_cast<const std::uint8_t*>(r.mapped[b]);
        const unsigned offset=b==1?0:BackingBytes,size=b==1?BackingBytes:ColorBytes;
        if(std::memcmp(actual,expected.data()+offset,size)==0)continue;
        okay=false;
        for(unsigned i=0;i<size;++i)if(actual[i]!=expected[offset+i]){
            std::uint32_t a=0,e=0;std::memcpy(&a,actual+(i&~3u),4);std::memcpy(&e,expected.data()+offset+(i&~3u),4);
            std::printf("EDRAM_DEPTH_ALIAS_MISMATCH buffer=%u byte=%u actual=%08x expected=%08x\n",b,i,a,e);break;
        }
    }
    std::printf("EDRAM_DEPTH_ALIAS_CASE draws=%u checked_output=%u checked_input=%zu fence=1 matched=%u\n",
        draws,ExpectedBytes,source.size(),unsigned(okay));return okay;
}
}

int main(int argc,char** argv){
    if((argc!=7 && argc!=8) || !recomp::gpu::initializeProbeProcess())return 2;
    unsigned mask=0;const std::string_view value(argv[6]);
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),mask);
    if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || mask>15)return 2;
    const std::string_view flag=argc==8?argv[7]:"";
    const bool corrupt=flag=="--corrupt-expected-control",noDepth=flag=="--disable-depth-write-control",
        noStencil=flag=="--disable-stencil-write-control",wrongStencil=flag=="--wrong-stencil-reference-control";
    const std::array<unsigned,2> controls{unsigned(flag=="--wrong-half-rows-control"),unsigned(flag=="--wrong-sample-control")};
    if(!flag.empty() && !corrupt && !noDepth && !noStencil && !wrongStencil && !controls[0] && !controls[1])return 2;
    std::array<std::vector<std::uint32_t>,3> code;
    for(unsigned i=0;i<3;++i){
        std::vector<std::uint8_t> bytes;
        if(!recomp::gpu::readInput(argv[i+1],bytes,65536) || bytes.size()<20 || bytes.size()%4)return 2;
        code[i].resize(bytes.size()/4);std::memcpy(code[i].data(),bytes.data(),bytes.size());
        if(code[i][0]!=0x07230203)return 2;
    }
    std::vector<std::uint8_t> source,expected;
    if(!recomp::gpu::readInput(argv[4],source,4096) || source.size()%4 ||
        !recomp::gpu::readInput(argv[5],expected,ExpectedBytes) || expected.size()!=ExpectedBytes)return 2;
    for(unsigned start:{0u,BackingBytes-16,BackingBytes,ExpectedBytes-16})
        for(unsigned i=0;i<16;++i)if(expected[start+i]!=0x5A)return 2;
    if(corrupt)expected[3686400+16]^=1;
    recomp::gpu::ProbeDevice device;if(!device.inspect() || !device.create({}))return 1;
    Resources resources(device.device);resources.depthWrite=!noDepth;resources.stencilWrite=!noStencil;
    resources.stencilReference=wrongStencil?0x3Cu:0u;
    if(!setup(device,resources,code,source.size(),mask))return 1;
    const auto cleared=clearReference();
    if(cleared.size()!=ExpectedBytes)return 2;
    for(unsigned draws:{0u,1u,2u,3u})if(!check(device,resources,source,draws?expected:cleared,draws,controls))return 1;
#ifdef RECOMP_EDRAM_COLOR_RESOLVE
    std::puts("EDRAM_COLOR_COPY_V1 reference_copy_kernel=1 tiled_bgra8=1 padding_bytes_untouched=81920 "
        "fresh_fences=4 retail_resolve_executed=0 guest_draws=0 game_frame=0 guest_writeback=0");
#else
    std::puts("EDRAM_DEPTH_ALIAS_V1 samples=4 sample_words=921600 depth_format=D32_SFLOAT_S8_UINT "
        "reference_edram_bytes=10485760 color_layout_word_readback=1 fresh_fences=4 retail_sample_locations=0 "
        "guest_draws=0 resolve_executed=0 game_frame=0 guest_writeback=0");
#endif
}
