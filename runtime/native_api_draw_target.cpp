#include "vulkan_queue_lock_all.h" // v177: every VkQueue call in this file holds the shared queue lock
#include "native_api_draw_target.h"
#include "vulkan_queue_lock.h" // v119
#include "native_color_chain_program.h"
#include "native_color_chain_resources.h"
#include "native_color_chain_commands.h"
#include "native_translated_draw.h"
#include "native_translated_resources.h"
#include "native_texture_decode_cache.h"
#if defined(RECOMP_DIAGNOSTIC_SHOW_FRAMES)
#include "diag_show_frames.h"
#endif
#include <cstring>
#include <cstdlib>
#include <cmath>

namespace recomp::gpu {
using TranslatedResources=NativeTranslatedResources;
struct NativeApiDrawTarget::Impl {
    using Resources=NativeColorChainResources<true,false,false>;
    static constexpr unsigned HalfBytes=1280*720*8;
    ProbeDevice& device;
    Resources resources;
    bool loaded=false,readbackReady=false;
    std::vector<std::unique_ptr<TranslatedResources>> translated;
    NativeTextureDecodeCache textureDecodeCache;
    explicit Impl(ProbeDevice& d):device(d),resources(d.device){}
    bool submit() {
        auto& r=resources;
        VkSubmitInfo info{};info.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.commandBufferCount=1;info.pCommandBuffers=&r.command;
        if(recomp::gpu::lockedQueueSubmit(device.queue,1,&info,r.fence)!=VK_SUCCESS ||
           vkWaitForFences(r.d,1,&r.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
        return true;
    }
};
NativeApiDrawTarget::NativeApiDrawTarget(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
NativeApiDrawTarget::~NativeApiDrawTarget()=default;
std::unique_ptr<NativeApiDrawTarget> NativeApiDrawTarget::create(ProbeDevice& device,const NativeColorChainProgram& program) {
    if(!device.device || !device.queue || program.coordinates()!=NativeColorCoordinateProfile::Padded1280W1)return nullptr;
    auto impl=std::make_unique<Impl>(device);
    auto& r=impl->resources;
    r.colorViewportMode=FragmentViewportMode::Color1280ShaderHalfPixel;
    const auto& all=program.code();
    const Impl::Resources::Code code{all[0],all[1],all[2],all[3],all[4],all[5]};
    const Impl::Resources::BufferSizes sizes{1382720,4352,Impl::HalfBytes+32,10485792,3768352,40};
    if(!r.setup(device,code,sizes,nullptr,true))return nullptr;
    for(unsigned i=0;i<sizes.size();++i)std::memset(r.mapped[i],0,sizes[i]);
    return std::unique_ptr<NativeApiDrawTarget>(new NativeApiDrawTarget(std::move(impl)));
}
bool NativeApiDrawTarget::loadHalf(const std::vector<std::uint8_t>& pixels) {
    if(pixels.size()!=Impl::HalfBytes)return false;
    auto& r=impl_->resources;
    if(vkResetCommandPool(r.d,r.pool,0)!=VK_SUCCESS || vkResetFences(r.d,1,&r.fence)!=VK_SUCCESS)return false;
    std::memcpy(static_cast<std::uint8_t*>(r.mapped[2])+16,pixels.data(),pixels.size());
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return false;
    VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&host,0,nullptr,0,nullptr);
    VkImageMemoryBarrier image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image.srcAccessMask=impl_->loaded?VK_ACCESS_TRANSFER_READ_BIT:0;image.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    image.oldLayout=impl_->loaded?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
    image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    image.image=r.colorImage;image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(r.command,impl_->loaded?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
    VkBufferImageCopy copy{};copy.bufferOffset=16;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageExtent={1280,720,1};
    vkCmdCopyBufferToImage(r.command,r.buffers[2],r.colorImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    image.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;image.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,0,nullptr,0,nullptr,1,&image);
    if(vkEndCommandBuffer(r.command)!=VK_SUCCESS)return false;
    impl_->submit();impl_->loaded=true;impl_->readbackReady=false;return true;
}
bool NativeApiDrawTarget::clearColor(const std::array<float,4>& color) {
    for(const auto c:color)if(!std::isfinite(c))return false;
    auto& r=impl_->resources;
    if(vkResetCommandPool(r.d,r.pool,0)!=VK_SUCCESS || vkResetFences(r.d,1,&r.fence)!=VK_SUCCESS)return false;
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return false;
    VkImageMemoryBarrier image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image.srcAccessMask=impl_->loaded?VK_ACCESS_TRANSFER_READ_BIT:0;
    image.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    image.oldLayout=impl_->loaded?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
    image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    image.image=r.colorImage;image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(r.command,impl_->loaded?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
    VkClearColorValue value{};std::copy(color.begin(),color.end(),value.float32);
    vkCmdClearColorImage(r.command,r.colorImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&image.subresourceRange);
    image.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    image.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,0,nullptr,0,nullptr,1,&image);
    if(vkEndCommandBuffer(r.command)!=VK_SUCCESS)return false;
    impl_->submit();impl_->loaded=true;impl_->readbackReady=false;return true;
}
bool NativeApiDrawTarget::drawValidatedAbi(unsigned pair,const std::vector<std::uint8_t>& packed) {
    if(!impl_->loaded || pair>1 || packed.size()!=(pair?4352u:1382720u))return false;
    auto& r=impl_->resources;
    std::memcpy(r.mapped[pair],packed.data(),packed.size());
    std::memset(r.mapped[2],0x5A,Impl::HalfBytes+32); // SSBO guards only; never the target image.
    NativeWordUpdatePlan noEvents;
    // Steps 1/2 select shader pairs, not frame numbers. Both use LOAD, retaining
    // the actual initialized target and every preceding draw's result.
    if(!recordNativeColorChainStage<false,false,false>(r,pair+1,noEvents))return false;
    impl_->submit();r.planes.ready=true;r.imageReady=true;impl_->readbackReady=true;return true;
}
bool NativeApiDrawTarget::drawTranslated(const NativeTranslatedDrawProgram& program,const Gta5InlineDraw& draw,
                                       const std::vector<std::uint32_t>& indices) {
#if defined(RECOMP_DIAGNOSTIC_SHOW_FRAMES)
    // Attempt v15 show-frames: structural checks only (diag_show_frames.h); state is not proven.
    if(!impl_->loaded || diagShowFramesTranslatedDraw(draw,program,indices))return false;
#else
    if(!impl_->loaded || !validateNativeTranslatedDraw(draw,program,indices))return false;
#endif
    std::vector<XenosTextureDescription> descriptions;
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> pixels;
    for(const auto& texture:draw.textures) {
    const auto desc=XenosTextureDescription::decode(texture.fetch);
    if(!desc || texture.literalZero || (desc->format!=2 && desc->format!=6 && desc->format!=18 && desc->format!=20) || desc->width>2048 || desc->height>2048 ||
       (desc->format==2 && desc->swizzle!=std::array<unsigned,4>{4,4,4,0}) || desc->sign!=std::array<unsigned,4>{0,0,0,0} ||
       (desc->clamp[0]!=0 && desc->clamp[0]!=2) || (desc->clamp[1]!=0 && desc->clamp[1]!=2) || desc->clamp[2]!=0 || desc->exponentBias || desc->integerComponents ||
       desc->mipMin || desc->mipMax || ((desc->fetch[3]>>19)&63)!=21)return false;
    for(const auto component:desc->swizzle)if(component>5)return false;
    auto decoded=impl_->textureDecodeCache.linearize(*desc,texture.source);
    if(!decoded || decoded->size()!=std::uint64_t{desc->width}*desc->height*(desc->format==2?1:4))return false;
    descriptions.push_back(*desc);pixels.push_back(std::move(decoded));
    }
    auto& r=impl_->resources;
    TranslatedResources* extra=nullptr;
    for(const auto& candidate:impl_->translated) {
        if(candidate->textureCount!=descriptions.size() || candidate->vertexCode!=program.vertexCode ||
           candidate->pixelCode!=program.pixelCode)continue;
        bool matching=true;
        for(unsigned i=0;i<descriptions.size();++i) {
            const auto& desc=descriptions[i];const auto& texture=candidate->textures[i];
            if(texture.width!=desc.width || texture.height!=desc.height || texture.format!=desc.format ||
               texture.swizzle!=desc.swizzle || texture.clamp!=desc.clamp){matching=false;break;}
        }
        if(matching){extra=candidate.get();break;}
    }
    if(!extra) {
        if(impl_->translated.size()>=32)return false;
        auto candidate=std::make_unique<TranslatedResources>(r.d);
        if(!candidate->setup(impl_->device,r.loadPass,program,descriptions))return false;
        extra=candidate.get();impl_->translated.push_back(std::move(candidate));
    }
    auto& t=*extra;auto* dst=static_cast<std::uint8_t*>(t.inputMapped);
    const auto word=[&](unsigned at,std::uint32_t v){for(unsigned b=0;b<4;++b)dst[at+b]=v>>(24-8*b);};
    word(0,draw.vertexCount);word(4,*gta5DrawRegister(draw,0x2302)==4);word(8,0);word(12,0);
    for(unsigned i=0;i<1024;++i){word(16+i*4,draw.vertexConstants[i]);word(4112+i*4,draw.pixelConstants[i]);}
    std::memcpy(dst+8208,draw.vertices.data(),draw.vertices.size());
    for(unsigned i=0;i<pixels.size();++i)
        std::memcpy(t.textures[i].uploadMapped,pixels[i]->data(),pixels[i]->size());
    if(!indices.empty())std::memcpy(t.indexMapped,indices.data(),indices.size()*4);
    if(vkResetCommandPool(r.d,r.pool,0)!=VK_SUCCESS || vkResetFences(r.d,1,&r.fence)!=VK_SUCCESS)return false;
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(r.command,&begin)!=VK_SUCCESS)return false;
    VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
    host.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,1,&host,0,nullptr,0,nullptr);
    for(unsigned i=0;i<t.textureCount;++i) {
    const auto& texture=t.textures[i];
    VkImageMemoryBarrier image{};image.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image.srcAccessMask=texture.ready?VK_ACCESS_SHADER_READ_BIT:0;image.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    image.oldLayout=texture.ready?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;image.image=texture.image;image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(r.command,texture.ready?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={texture.width,texture.height,1};
    vkCmdCopyBufferToImage(r.command,texture.upload,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    image.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;image.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(r.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&image);
    }
    VkRenderPassBeginInfo pass{};pass.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;pass.renderPass=r.loadPass;pass.framebuffer=r.framebuffer;pass.renderArea={{0,0},{1280,720}};
    vkCmdBeginRenderPass(r.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,t.pipeline);
#if defined(RECOMP_DIAGNOSTIC_SHOW_FRAMES)
    const auto clip=diagShowFramesScissor(draw);
#else
    const auto clip=*nativeTranslatedScissor(draw);
#endif
    const VkRect2D scissor{{static_cast<std::int32_t>(clip[0]),static_cast<std::int32_t>(clip[1])},{clip[2],clip[3]}};
    vkCmdSetScissor(r.command,0,1,&scissor);
#if defined(RECOMP_DIAGNOSTIC_SHOW_FRAMES)
    const auto viewport=diagShowFramesViewport(draw);
#else
    const auto viewport=*nativeTranslatedViewport(draw);
#endif
    vkCmdPushConstants(r.command,t.layout,VK_SHADER_STAGE_VERTEX_BIT,0,16,viewport.data());
    vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,t.layout,0,1,&t.set,0,nullptr);
    if(program.indexed) {
        vkCmdBindIndexBuffer(r.command,t.indices,0,VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(r.command,static_cast<std::uint32_t>(indices.size()),1,0,0,0);
    } else vkCmdDraw(r.command,draw.vertexCount/4*6,1,0,0);
    vkCmdEndRenderPass(r.command);
    if(vkEndCommandBuffer(r.command)!=VK_SUCCESS)return false;
    impl_->submit();
    for(unsigned i=0;i<t.textureCount;++i)t.textures[i].ready=true;
    impl_->readbackReady=false;return true;
}
std::optional<std::vector<std::uint8_t>> NativeApiDrawTarget::readHalf()const {
    if(!impl_->readbackReady)return std::nullopt;
    const auto* pixels=static_cast<const std::uint8_t*>(impl_->resources.mapped[2])+16;
    return std::vector<std::uint8_t>(pixels,pixels+Impl::HalfBytes);
}
std::optional<std::vector<std::uint8_t>> NativeApiDrawTarget::resolveTiled(const std::vector<std::uint8_t>& previous) {
    if(!impl_->loaded || previous.size()!=3768320)return std::nullopt;
    auto& r=impl_->resources;
    // A clear or load initializes real image contents without any draw. Stage3
    // performs an empty LOAD pass, copies that image to the half buffer, then
    // runs the existing GPU store/resolve. Never reuse a stale prior readback.
    // Only the store's completely written source rectangle is read by copy.
    // Padding in the destination is retained from the actual owned texture.
    std::memset(r.mapped[3],0,10485792);
    std::memcpy(static_cast<std::uint8_t*>(r.mapped[4])+16,previous.data(),previous.size());
    NativeWordUpdatePlan noEvents;noEvents.bufferBytes=40;
    if(!recordNativeColorChainStage<true,false,false>(r,3,noEvents))return std::nullopt;
    impl_->submit();
    r.planes.ready=true;r.imageReady=true;impl_->readbackReady=true;
    const auto* pixels=static_cast<const std::uint8_t*>(r.mapped[4])+16;
    return std::vector<std::uint8_t>(pixels,pixels+previous.size());
}
}
