// Real translated vertex module + raw owned inputs, synthetic UV/depth fragment
// diagnostic. No title PS, texture sampling, PM4 execution, guest writes or frame.
#include "texture_fragment_target.h"
#include "private_input.h"
#include "process_limits.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
using namespace recomp::gpu;
constexpr unsigned Width=1280,Height=720,Pixels=Width*Height,OutputBytes=Pixels*16+32;
bool capabilityControls(){
    VkPhysicalDeviceProperties properties{};properties.apiVersion=VK_API_VERSION_1_2;
    auto& limits=properties.limits;limits.maxFramebufferWidth=Width;limits.maxFramebufferHeight=Height;
    limits.maxViewportDimensions[0]=2048;limits.maxViewportDimensions[1]=1024;
    limits.viewportBoundsRange[0]=-32768;limits.viewportBoundsRange[1]=32768;
    limits.viewportSubPixelBits=8;limits.framebufferColorSampleCounts=VK_SAMPLE_COUNT_1_BIT;
    unsigned rejections=0;
    for(unsigned mode=0;mode<4;++mode){
        FragmentTargetConfig config{Width,Height,VK_FORMAT_R32G32B32A32_SFLOAT,false,false,static_cast<FragmentViewportMode>(mode)};
        if(!fragmentTargetCapabilities(properties,config))return false;
        for(unsigned mutation=0;mutation<11;++mutation){auto bad=properties;
            switch(mutation){
                case 0:bad.apiVersion=VK_API_VERSION_1_0;break;
                case 1:bad.limits.maxFramebufferWidth=Width-1;break;
                case 2:bad.limits.maxFramebufferHeight=Height-1;break;
                case 3:bad.limits.maxViewportDimensions[0]=(mode==3?2048:Width)-1;break;
                case 4:bad.limits.maxViewportDimensions[1]=(mode==3?1024:Height)-1;break;
                case 5:bad.limits.viewportBoundsRange[0]=.5;break;
                case 6:bad.limits.viewportBoundsRange[1]=mode==2?1280.f:1279.f;break;
                case 7:bad.limits.viewportBoundsRange[0]=std::numeric_limits<float>::quiet_NaN();break;
                case 8:bad.limits.viewportBoundsRange[1]=std::numeric_limits<float>::quiet_NaN();break;
                case 9:bad.limits.framebufferColorSampleCounts=VK_SAMPLE_COUNT_4_BIT;break;
                case 10:bad.limits.viewportSubPixelBits=0;break;
            }
            const bool rejected=mutation!=10 || mode==2;
            if(fragmentTargetCapabilities(bad,config)==rejected)return false;
            rejections+=rejected;
        }
        for(unsigned mutation=0;mutation<7;++mutation){auto bad=config;
            if(mutation==0)bad.width=0;if(mutation==1)bad.height=0;
            if(mutation==2)bad.width=Width+1;if(mutation==3)bad.height=Height+1;
            if(mutation==4)bad.format=VK_FORMAT_R8_UNORM;
            if(mutation==5)bad.viewportMode=static_cast<FragmentViewportMode>(99);
            if(mutation==6){bad.width=32;bad.height=32;}
            const bool rejected=mutation!=6 || mode!=0;
            if(fragmentTargetCapabilities(properties,bad)==rejected)return false;
            rejections+=rejected;
        }
    }
    std::printf("COLOR_VIEWPORT_CAPABILITY_CONTROLS rejections=%u gpu_work=0\n",rejections);return rejections==68;
}
struct Resources {
    VkDevice d;
    VkImage colorImage{};VkDeviceMemory colorMemory{};VkImageView colorView{};
    VkRenderPass renderPass{};VkFramebuffer framebuffer{};
    VkShaderModule vertexShader{},shader{};VkPipeline pipeline{};VkPipelineLayout layout{};
    VkDescriptorSetLayout setLayout{};VkDescriptorPool descriptors{};VkDescriptorSet set{};
    std::array<VkBuffer,2> buffers{};std::array<VkDeviceMemory,2> memory{};std::array<void*,2> mapped{};
    VkCommandPool pool{};VkCommandBuffer command{};VkFence fence{};
    explicit Resources(VkDevice device):d(device){}
    Resources(const Resources&)=delete;Resources& operator=(const Resources&)=delete;
    ~Resources(){
        if(fence)vkDestroyFence(d,fence,nullptr);
        if(pool)vkDestroyCommandPool(d,pool,nullptr);
        if(pipeline)vkDestroyPipeline(d,pipeline,nullptr);
        if(shader)vkDestroyShaderModule(d,shader,nullptr);
        if(vertexShader)vkDestroyShaderModule(d,vertexShader,nullptr);
        if(descriptors)vkDestroyDescriptorPool(d,descriptors,nullptr);
        if(layout)vkDestroyPipelineLayout(d,layout,nullptr);
        if(setLayout)vkDestroyDescriptorSetLayout(d,setLayout,nullptr);
        if(framebuffer)vkDestroyFramebuffer(d,framebuffer,nullptr);
        if(renderPass)vkDestroyRenderPass(d,renderPass,nullptr);
        if(colorView)vkDestroyImageView(d,colorView,nullptr);
        if(colorImage)vkDestroyImage(d,colorImage,nullptr);
        if(colorMemory)vkFreeMemory(d,colorMemory,nullptr);
        for(unsigned i=0;i<2;++i){
            if(mapped[i])vkUnmapMemory(d,memory[i]);
            if(buffers[i])vkDestroyBuffer(d,buffers[i],nullptr);
            if(memory[i])vkFreeMemory(d,memory[i],nullptr);
        }
    }
    bool setup(ProbeDevice& device,const std::array<std::vector<uint32_t>,2>& code,
            const std::vector<uint8_t>& source,FragmentTargetConfig config){
        const std::array<unsigned,2> sizes{{static_cast<unsigned>(source.size()),OutputBytes}};
        if(device.properties.limits.maxStorageBufferRange<source.size() ||
            !setupTextureFragmentTarget(device,*this,config))return false;
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(device.physical,&props);
        for(unsigned i=0;i<2;++i){
            VkBufferCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;info.size=sizes[i];
            info.usage=i?VK_BUFFER_USAGE_TRANSFER_DST_BIT:VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            if(vkCreateBuffer(d,&info,nullptr,&buffers[i])!=VK_SUCCESS)return false;
            VkMemoryRequirements req{};vkGetBufferMemoryRequirements(d,buffers[i],&req);
            unsigned type=props.memoryTypeCount;
            constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            for(unsigned n=0;n<props.memoryTypeCount;++n)if((req.memoryTypeBits&(1u<<n)) &&
                (props.memoryTypes[n].propertyFlags&flags)==flags){type=n;break;}
            if(type==props.memoryTypeCount)return false;
            VkMemoryAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize=req.size;alloc.memoryTypeIndex=type;
            if(vkAllocateMemory(d,&alloc,nullptr,&memory[i])!=VK_SUCCESS ||
                vkBindBufferMemory(d,buffers[i],memory[i],0)!=VK_SUCCESS ||
                vkMapMemory(d,memory[i],0,VK_WHOLE_SIZE,0,&mapped[i])!=VK_SUCCESS)return false;
        }
        std::memcpy(mapped[0],source.data(),source.size());std::memset(mapped[1],0x5A,OutputBytes);
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo setInfo{};setInfo.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setInfo.bindingCount=1;setInfo.pBindings=&binding;
        if(vkCreateDescriptorSetLayout(d,&setInfo,nullptr,&setLayout)!=VK_SUCCESS)return false;
        VkPipelineLayoutCreateInfo layoutInfo{};layoutInfo.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount=1;layoutInfo.pSetLayouts=&setLayout;
        if(vkCreatePipelineLayout(d,&layoutInfo,nullptr,&layout)!=VK_SUCCESS)return false;
        for(unsigned i=0;i<2;++i){
            VkShaderModuleCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            info.codeSize=code[i].size()*4;info.pCode=code[i].data();
            if(vkCreateShaderModule(d,&info,nullptr,i?&shader:&vertexShader)!=VK_SUCCESS)return false;
        }
        if(!createTextureFragmentPipeline(*this,true,config))return false;
        VkDescriptorPoolSize count{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
        VkDescriptorPoolCreateInfo poolInfo{};poolInfo.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets=1;poolInfo.poolSizeCount=1;poolInfo.pPoolSizes=&count;
        if(vkCreateDescriptorPool(d,&poolInfo,nullptr,&descriptors)!=VK_SUCCESS)return false;
        VkDescriptorSetAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool=descriptors;alloc.descriptorSetCount=1;alloc.pSetLayouts=&setLayout;
        if(vkAllocateDescriptorSets(d,&alloc,&set)!=VK_SUCCESS)return false;
        VkDescriptorBufferInfo input{buffers[0],0,source.size()};
        VkWriteDescriptorSet write{};write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet=set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&input;
        vkUpdateDescriptorSets(d,1,&write,0,nullptr);
        VkCommandPoolCreateInfo commandPool{};commandPool.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        commandPool.queueFamilyIndex=device.queueFamily;
        if(vkCreateCommandPool(d,&commandPool,nullptr,&pool)!=VK_SUCCESS)return false;
        VkCommandBufferAllocateInfo commands{};commands.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commands.commandPool=pool;commands.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;commands.commandBufferCount=1;
        VkFenceCreateInfo fenceInfo{};fenceInfo.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        return vkAllocateCommandBuffers(d,&commands,&command)==VK_SUCCESS && vkCreateFence(d,&fenceInfo,nullptr,&fence)==VK_SUCCESS;
    }
};
bool draw(ProbeDevice& device,Resources& r,const std::vector<uint8_t>& source,FragmentViewportMode mode){
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return false;
    VkMemoryBarrier before{};before.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;before.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,1,&before,0,nullptr,0,nullptr);
    VkClearValue clear{};for(auto& c:clear.color.float32)c=203.25f;
    VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass=r.renderPass;pass.framebuffer=r.framebuffer;pass.renderArea={{0,0},{Width,Height}};
    pass.clearValueCount=1;pass.pClearValues=&clear;
    vkCmdBeginRenderPass(r.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.pipeline);
    vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.layout,0,1,&r.set,0,nullptr);
    vkCmdDraw(r.command,4,1,0,0);vkCmdEndRenderPass(r.command);
    VkBufferImageCopy copy{};copy.bufferOffset=16;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageExtent={Width,Height,1};
    vkCmdCopyImageToBuffer(r.command,r.colorImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.buffers[1],1,&copy);
    VkMemoryBarrier after{};after.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;after.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&after,0,nullptr,0,nullptr);
    if(vkEndCommandBuffer(r.command)!=VK_SUCCESS)return false;
    const auto* bytes=static_cast<const uint8_t*>(r.mapped[1]);
    for(unsigned i=0;i<OutputBytes;++i)if(bytes[i]!=0x5A)return false;
    if(vkGetFenceStatus(r.d,r.fence)!=VK_NOT_READY)return false;
    VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&r.command;
    if(vkQueueSubmit(device.queue,1,&submit,r.fence)!=VK_SUCCESS ||
        vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS){std::fflush(stdout);std::_Exit(4);}
    const bool inputMatch=std::memcmp(r.mapped[0],source.data(),source.size())==0;
    bool guards=true;for(unsigned i=0;i<16;++i)guards=guards && bytes[i]==0x5A && bytes[OutputBytes-16+i]==0x5A;
    const bool positive=mode==FragmentViewportMode::DiagnosticPositive;
    const bool half=mode==FragmentViewportMode::Color1280NegativeYHalfPixel || mode==FragmentViewportMode::Color1280ShaderHalfPixel;
    constexpr double Tolerance=0.000002; // Strictly below either half-pixel displacement.
    double maximum=0;unsigned mismatches=0,guestMismatches=0;
    for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x){
        std::array<float,4> pixel{};std::memcpy(pixel.data(),bytes+16+(y*Width+x)*16,16);
        const std::array<double,4> expected{{(x+(half?0:.5))/Width,
            positive?1-(y+.5)/Height:(y+(half?0:.5))/Height,positive?0.:1.,1}};
        const std::array<double,4> guest{{double(x)/Width,double(y)/Height,1,1}};
        bool mismatch=false,guestMismatch=false;
        for(unsigned lane=0;lane<4;++lane){
            const double error=std::abs(pixel[lane]-expected[lane]);maximum=std::max(maximum,error);
            if(error>Tolerance && mismatches<8)std::printf("COLOR_VIEWPORT_DIFFERENCE mode=%u x=%u y=%u lane=%u actual=%.9g expected=%.9g error=%.9g\n",
                unsigned(mode),x,y,lane,double(pixel[lane]),expected[lane],error);
            mismatch=mismatch || !std::isfinite(pixel[lane]) || error>Tolerance;
            guestMismatch=guestMismatch || !std::isfinite(pixel[lane]) || std::abs(pixel[lane]-guest[lane])>Tolerance;
        }
        mismatches+=mismatch;guestMismatches+=guestMismatch;
    }
    std::printf("COLOR_VIEWPORT_CASE mode=%u pixels=%u mismatches=%u guest_candidate_mismatches=%u max_error=%.9g input_match=%u guards=%u fence=1\n",
        unsigned(mode),Pixels,mismatches,guestMismatches,maximum,unsigned(inputMatch),unsigned(guards));
    return !mismatches && inputMatch && guards && (half?!guestMismatches:guestMismatches==Pixels);
}
}
int main(int argc,char** argv){
    const bool wrongMapped=argc==7 && std::strcmp(argv[6],"--wrong-mapped-module")==0;
    if((argc!=6 && !wrongMapped) || !recomp::gpu::initializeProbeProcess() || !capabilityControls())return 2;
    const bool small=std::strcmp(argv[5],"small")==0,r8=std::strcmp(argv[5],"r8")==0;
    if(!small && !r8)return 2;
    std::array<std::vector<uint32_t>,3> modules;
    for(unsigned i=0;i<3;++i){
        std::vector<uint8_t> bytes;
        if(!recomp::gpu::readInput(argv[1+i],bytes,65536) || bytes.size()<20 || bytes.size()%4)return 2;
        modules[i].resize(bytes.size()/4);std::memcpy(modules[i].data(),bytes.data(),bytes.size());
        if(modules[i][0]!=0x07230203)return 2;
    }
    std::vector<uint8_t> source;
    if(!recomp::gpu::readInput(argv[4],source,1382720) || source.size()!=(small?4352u:1382720u))return 2;
    recomp::gpu::ProbeDevice device;if(!device.inspect() || !device.create({}))return 1;
    std::printf("COLOR_VIEWPORT_CAPS subpixel_bits=%u bounds=%.1f,%.1f\n",device.properties.limits.viewportSubPixelBits,
        device.properties.limits.viewportBoundsRange[0],device.properties.limits.viewportBoundsRange[1]);
    unsigned fences=0,capabilityRejections=0;
    for(const auto mode:{FragmentViewportMode::DiagnosticPositive,FragmentViewportMode::Color1280NegativeY,
            FragmentViewportMode::Color1280NegativeYHalfPixel,FragmentViewportMode::Color1280ShaderHalfPixel}){
        FragmentTargetConfig config{Width,Height,VK_FORMAT_R32G32B32A32_SFLOAT,false,false,mode};
        const std::array<std::vector<uint32_t>,2> code{{modules[mode==FragmentViewportMode::Color1280ShaderHalfPixel && !wrongMapped?1:0],modules[2]}};
        Resources resources(device.device);
        if(mode==FragmentViewportMode::Color1280NegativeYHalfPixel && device.properties.limits.viewportSubPixelBits<1){
            if(resources.setup(device,code,source,config) || resources.colorImage || resources.buffers[0])return 1;
            ++capabilityRejections;std::puts("COLOR_VIEWPORT_CAPABILITY_REJECTED mode=2 before_allocation=1 fence=0");continue;
        }
        if(!resources.setup(device,code,source,config) || !draw(device,resources,source,mode))return 1;
        ++fences;
    }
    std::printf("COLOR_VIEWPORT_V2 native_fences=%u capability_rejections=%u title_pixel_shader=0 guest_state_admitted=0 guest_words_written=0 game_frame=0\n",fences,capabilityRejections);
    return 0;
}
