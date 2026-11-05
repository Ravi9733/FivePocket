#include "vulkan_queue_lock_all.h" // v177: every VkQueue call in this file holds the shared queue lock
#include <android/log.h>
#include <array>
#include <memory>
#include <mutex>
#include "native_texture_image.h"
#include "vulkan_queue_lock.h" // v119
#include "xenos_float32_texture.h"
#include "native_color8_resolve.h"
#include "xenos_depth24_texture.h"
#include "xenos_raw10_texture.h"
#include <cstring>
#include <cstdlib>

namespace recomp::gpu {
namespace {
unsigned memoryType(const ProbeDevice& d, unsigned allowed, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p{}; vkGetPhysicalDeviceMemoryProperties(d.physical,&p);
    for(unsigned i=0;i<p.memoryTypeCount;++i)
        if((allowed&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags)return i;
    return UINT32_MAX;
}
struct Transfer {
    VkDevice d;
    VkBuffer buffer{}; VkDeviceMemory memory{}; void* mapped=nullptr;
    VkCommandPool pool{}; VkCommandBuffer command{}; VkFence fence{};
    ~Transfer() {
        if(fence)vkDestroyFence(d,fence,nullptr);
        if(pool)vkDestroyCommandPool(d,pool,nullptr);
        if(mapped)vkUnmapMemory(d,memory);
        if(buffer)vkDestroyBuffer(d,buffer,nullptr);
        if(memory)vkFreeMemory(d,memory,nullptr);
    }
    bool setup(const ProbeDevice& device, unsigned bytes) {
        VkBufferCreateInfo b{}; b.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        b.size=bytes; b.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(vkCreateBuffer(d,&b,nullptr,&buffer)!=VK_SUCCESS)return false;
        VkMemoryRequirements r{};vkGetBufferMemoryRequirements(d,buffer,&r);
        const auto type=memoryType(device,r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if(type==UINT32_MAX)return false;
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=type;
        if(vkAllocateMemory(d,&a,nullptr,&memory)!=VK_SUCCESS || vkBindBufferMemory(d,buffer,memory,0)!=VK_SUCCESS ||
            vkMapMemory(d,memory,0,VK_WHOLE_SIZE,0,&mapped)!=VK_SUCCESS)return false;
        VkCommandPoolCreateInfo p{};p.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        p.queueFamilyIndex=device.queueFamily;p.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if(vkCreateCommandPool(d,&p,nullptr,&pool)!=VK_SUCCESS)return false;
        VkCommandBufferAllocateInfo c{};c.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        c.commandPool=pool;c.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;c.commandBufferCount=1;
        VkFenceCreateInfo f{};f.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        return vkAllocateCommandBuffers(d,&c,&command)==VK_SUCCESS && vkCreateFence(d,&f,nullptr,&fence)==VK_SUCCESS;
    }
};
}

NativeTextureImage::~NativeTextureImage() {
    if(view_)vkDestroyImageView(device_.device,view_,nullptr);
    if(image_)vkDestroyImage(device_.device,image_,nullptr);
    if(memory_)vkFreeMemory(device_.device,memory_,nullptr);
}
std::unique_ptr<NativeTextureImage> NativeTextureImage::create(ProbeDevice& device,
        const XenosTextureDescription& description,const std::vector<std::uint8_t>& guestBytes) {
    // Format25 imports as UNORM16. Unlike the raw float formats, changing its
    // sign/integer selects changes the sampled numbers and cannot be ignored.
    if(description.format==25 && (description.integerComponents ||
       std::any_of(description.sign.begin(),description.sign.end(),[](auto sign){return sign!=0;})))return nullptr;
    // k_16_16_16_16_FLOAT and k_32_FLOAT are addressed through their raw
    // storage view for the same reason the capture uses one: the colour import
    // refuses their sign and integer-component selects, which do not change the
    // stored bytes.
    const auto plan=description.format==32 || description.format==36 || description.format==25?
        planXenosFloatStorage(description):planXenosTexture2D(description);
    if(!device.device || !device.queue || !plan || guestBytes.size()<plan->sourceBytes)return nullptr;
    auto result=std::unique_ptr<NativeTextureImage>(new NativeTextureImage(device,*plan));
    result->float32_=description.format==36;
    // k_16_16 arrives from a resolve that wrote unsigned halves, and its fetch
    // constant reads them TextureSign::kUnsigned, so it imports as two
    // normalized channels rather than the signed pair the target stored.
    result->fixed16_=description.format==25;
    if(!result->initialize() || !result->upload(guestBytes))return nullptr;
    return result;
}
std::unique_ptr<NativeTextureImage> NativeTextureImage::createUnpackedMips(ProbeDevice& device,
        const XenosTextureDescription& description,const std::vector<std::uint8_t>& base,
        const std::vector<std::uint8_t>& mips) {
    const auto plan=planXenosUnpackedMipChain(description.fetch);
    const auto original=XenosTextureDescription::decode(description.fetch);
    if(!device.device || !device.queue || !plan || !original ||
        base.size()<plan->baseBytes || mips.size()<plan->mipBytes)return nullptr;
    auto result=std::unique_ptr<NativeTextureImage>(new NativeTextureImage(device,plan->levels.front().layout));
    result->layout_.description=*original; // Preserve the actual submitted binding.
    result->levels_.clear();
    for(const auto& level:plan->levels)result->levels_.push_back(level.layout);
    result->unpackedMips_=true;
    if(!result->initialize() || !result->uploadMipChain(base,mips))return nullptr;
    return result;
}
std::unique_ptr<NativeTextureImage> NativeTextureImage::createDepth24Float(ProbeDevice& device,
        const XenosTextureDescription& description,const std::vector<std::uint8_t>& guestBytes) {
    const auto storage=planXenosDepth24FloatStorage(description);
    const auto original=XenosTextureDescription::decode(description.fetch);
    if(!device.device || !device.queue || !storage || !original || guestBytes.size()!=storage->sourceBytes)return nullptr;
    auto result=std::unique_ptr<NativeTextureImage>(new NativeTextureImage(device,*storage));
    result->layout_.description=*original;result->depth24Float_=true;
    if(!result->initialize() || !result->uploadDepth24Float(guestBytes))return nullptr;
    return result;
}
bool NativeTextureImage::uploadDepth24Float(const std::vector<std::uint8_t>& guestBytes) {
    if(!depth24Float_ || unpackedMips_ || layers_!=1 || levelCount()!=1)return false;
    auto bytes=decodeXenosDepth24FloatTexture(layout_.description,guestBytes);
    return bytes && transfer(*bytes,true);
}
std::unique_ptr<NativeTextureImage> NativeTextureImage::createRaw10Unsigned(ProbeDevice& device,
        const XenosTextureDescription& description,const std::vector<std::uint8_t>& guestBytes) {
    const auto storage=planXenosRaw10UnsignedStorage(description);
    const auto original=XenosTextureDescription::decode(description.fetch);
    if(!device.device || !device.queue || !storage || !original ||
       guestBytes.size()!=storage->sourceBytes)return nullptr;
    auto result=std::unique_ptr<NativeTextureImage>(new NativeTextureImage(device,*storage));
    result->layout_.description=*original;result->raw10Unsigned_=true;
    if(!result->initialize() || !result->uploadRaw10Unsigned(guestBytes))return nullptr;
    return result;
}
bool NativeTextureImage::uploadRaw10Unsigned(const std::vector<std::uint8_t>& guestBytes) {
    if(!raw10Unsigned_ || unpackedMips_ || layers_!=1 || levelCount()!=1)return false;
    auto bytes=linearizeXenosRaw10UnsignedTexture(layout_.description,guestBytes);
    return bytes && transfer(*bytes,true);
}
bool NativeTextureImage::initialize() {
    std::uint64_t total=0;
    for(const auto& level:levels_) {
        total=(total+3)&~std::uint64_t{3}; // VkBufferImageCopy offsets, including R8.
        linearOffsets_.push_back(static_cast<std::uint32_t>(total));
        total+=std::uint64_t{level.linearBytes}*layers_;
        if(total>64u*1024*1024)return false;
    }
    stagingBytes_=static_cast<std::uint32_t>(total);
    // k_16_16_16_16_FLOAT is four binary16 per texel and imports as RGBA16F.
    // Any OTHER eight-byte texel would fall through to RGBA8 below and be read
    // as the wrong format, so it is refused rather than approximated.
    if(layout_.bytesPerTexel>4 && layout_.description.format!=32)return false;
    const auto format=raw10Unsigned_?VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        depth24Float_?VK_FORMAT_R32_SFLOAT:layout_.description.format==30?VK_FORMAT_R16_SFLOAT:
        layout_.description.format==32?VK_FORMAT_R16G16B16A16_SFLOAT:
        // k_32_FLOAT is one binary32 per texel. Its raw view addresses as
        // format 6, so the recorded original format is what decides this.
        float32_?VK_FORMAT_R32_SFLOAT:
        fixed16_?VK_FORMAT_R16G16_UNORM:
        layout_.bytesPerTexel==1?VK_FORMAT_R8_UNORM:VK_FORMAT_R8G8B8A8_UNORM;
    VkFormatProperties props{};vkGetPhysicalDeviceFormatProperties(device_.physical,format,&props);
    linearFilterSupported_=(props.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)!=0;
    // Adreno740 supports R32 sampling/transfers but does not advertise linear
    // filtering. Keep exact32-bit storage; never fall back to lossy16-bit depth.
    // Legacy images retain their filtering requirement. A depth-image consumer
    // must separately check supportsLinearFiltering() before requesting it.
    const auto required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|
        (depth24Float_ || float32_?0u:VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    if((props.optimalTilingFeatures&required)!=required)return false;
    VkImageCreateInfo i{};i.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    i.imageType=VK_IMAGE_TYPE_2D;i.format=format;
    i.extent={layout_.description.width,layout_.description.height,1};
    i.mipLevels=levelCount();i.arrayLayers=layers_;i.samples=VK_SAMPLE_COUNT_1_BIT;i.tiling=VK_IMAGE_TILING_OPTIMAL;
    i.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImageFormatProperties supported{};
    if(vkGetPhysicalDeviceImageFormatProperties(device_.physical,format,i.imageType,i.tiling,i.usage,0,&supported)!=VK_SUCCESS ||
        i.extent.width>supported.maxExtent.width || i.extent.height>supported.maxExtent.height ||
        i.mipLevels>supported.maxMipLevels || layers_>supported.maxArrayLayers)return false;
    if(vkCreateImage(device_.device,&i,nullptr,&image_)!=VK_SUCCESS)return false;
    VkMemoryRequirements r{};vkGetImageMemoryRequirements(device_.device,image_,&r);
    const auto type=memoryType(device_,r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(type==UINT32_MAX)return false;
    VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=type;
    if(vkAllocateMemory(device_.device,&a,nullptr,&memory_)!=VK_SUCCESS ||
        vkBindImageMemory(device_.device,image_,memory_,0)!=VK_SUCCESS)return false;
    VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    v.image=image_;v.viewType=layers_==6?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
    v.format=format;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,levelCount(),0,layers_};
    constexpr VkComponentSwizzle component[]{VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_A,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ONE};
    const auto channel=[&](unsigned c){const auto s=layout_.description.swizzle[c];
        return component[(depth24Float_ || float32_ || layout_.description.format==30 || layout_.bytesPerTexel==1) && s<4?0:s];}; // Xenos replicates the last component before swizzling.
    v.components={channel(0),channel(1),channel(2),channel(3)};
    return vkCreateImageView(device_.device,&v,nullptr,&view_)==VK_SUCCESS;
}
namespace {
// Attempt v12: most native draws re-import the same guest textures every frame (UI atlases, fonts);
// the CPU detile was 12% of the render thread in gameplay. Reuse linear bytes only on an exact match:
// all six fetch words and ALL source bytes (the hash is a filter, never a hit by itself), as in
// NativeTextureDecodeCache. LRU under a byte budget; one leaf mutex, no calls out while held.
struct LinearCacheEntry {std::array<std::uint32_t,6> fetch{};std::uint64_t hash=0;
    std::shared_ptr<const std::vector<std::uint8_t>> source,linear;std::uint64_t used=0;
    std::size_t bytes()const noexcept{return source?source->size()+linear->size():0;}};
std::mutex linearCacheMutex;std::array<LinearCacheEntry,48> linearCache;std::uint64_t linearCacheClock=0;
std::size_t linearCacheBytes=0;constexpr std::size_t linearCacheBudget=96u*1024*1024;
std::uint64_t linearCacheHits=0,linearCacheMisses=0;
std::uint64_t hashBytes(const std::vector<std::uint8_t>& b) {
    std::uint64_t h=0x9E3779B97F4A7C15ull^b.size();std::size_t i=0;
    for(;i+8<=b.size();i+=8){std::uint64_t w;std::memcpy(&w,b.data()+i,8);h=(h^w)*0x100000001B3ull;h^=h>>29;}
    for(;i<b.size();++i)h=(h^b[i])*0x100000001B3ull;
    return h;
}
}
bool NativeTextureImage::upload(const std::vector<std::uint8_t>& guestBytes) {
    if(depth24Float_ || raw10Unsigned_ || unpackedMips_ || layers_!=1)return false; // Explicit format imports require their own upload path.
    const auto hash=hashBytes(guestBytes);
    const auto& fetch=layout_.description.fetch;
    std::shared_ptr<const std::vector<std::uint8_t>> linear;
    {
        std::lock_guard lock(linearCacheMutex);
        for(auto& e:linearCache)
            if(e.source && e.hash==hash && e.fetch==fetch && *e.source==guestBytes)
                {e.used=++linearCacheClock;linear=e.linear;++linearCacheHits;break;}
    }
    if(!linear) {
        auto bytes=linearizeXenosTexture2D(layout_.description,guestBytes);
        if(!bytes)return false;
        linear=std::make_shared<const std::vector<std::uint8_t>>(std::move(*bytes));
        auto source=std::make_shared<const std::vector<std::uint8_t>>(guestBytes);
        const auto need=source->size()+linear->size();
        std::lock_guard lock(linearCacheMutex);++linearCacheMisses;
        if(need<=linearCacheBudget/4) {
            for(;;) {  // evict least recently used until one slot and the byte budget are free
                LinearCacheEntry* victim=nullptr;bool freeSlot=false;
                for(auto& e:linearCache){if(!e.source){freeSlot=true;continue;}if(!victim||e.used<victim->used)victim=&e;}
                if(freeSlot && linearCacheBytes+need<=linearCacheBudget)break;
                if(!victim)break;
                linearCacheBytes-=victim->bytes();*victim=LinearCacheEntry{};
            }
            for(auto& e:linearCache)if(!e.source){e=LinearCacheEntry{fetch,hash,source,linear,++linearCacheClock};linearCacheBytes+=need;break;}
        }
        if(!((linearCacheHits+linearCacheMisses)&1023))
            __android_log_print(ANDROID_LOG_INFO,"RecompRenderer","RENDER_DIAG_TEXTURE_CACHE hits=%llu misses=%llu bytes=%zu",
                (unsigned long long)linearCacheHits,(unsigned long long)linearCacheMisses,linearCacheBytes);
    }
    // transfer(upload=true) only copies from `bytes`; the shared cached vector is never written.
    return transfer(const_cast<std::vector<std::uint8_t>&>(*linear),true);
}
bool NativeTextureImage::uploadMipChain(const std::vector<std::uint8_t>& base,
        const std::vector<std::uint8_t>& mips) {
    if(!unpackedMips_)return false;
    const auto levels=linearizeXenosUnpackedMipChain(layout_.description.fetch,base,mips);
    return levels && uploadLevels(*levels);
}
bool NativeTextureImage::uploadLevels(const std::vector<std::vector<std::uint8_t>>& levels) {
    if(levels.size()!=levels_.size())return false;
    std::vector<std::uint8_t> bytes(stagingBytes_,0);
    for(unsigned i=0;i<levels.size();++i) {
        if(levels[i].size()!=levels_[i].linearBytes)return false;
        std::copy(levels[i].begin(),levels[i].end(),bytes.begin()+linearOffsets_[i]);
    }
    return transfer(bytes,true);
}
std::unique_ptr<NativeTextureImage> NativeTextureImage::createCubeBase(ProbeDevice& device,
        const XenosTextureDescription& description,const std::vector<std::uint8_t>& allFaces) {
    const auto p=planNativeColor8ResolveFace(description,0);
    if(!device.device || !device.queue || !p || p->original.dimension!=XenosTextureDescription::Dimension::Cube ||
       allFaces.size()!=std::uint64_t{p->storage.sourceBytes}*6)return nullptr;
    for(unsigned face=1;face<6;++face)if(!planNativeColor8ResolveFace(description,face))return nullptr;
    auto result=std::unique_ptr<NativeTextureImage>(new NativeTextureImage(device,p->storage));
    result->layout_.description=p->original;result->layers_=6;
    if(!result->initialize() || !result->uploadCubeBase(allFaces))return nullptr;
    return result;
}
bool NativeTextureImage::uploadCubeBase(const std::vector<std::uint8_t>& allFaces) {
    if(layers_!=6 || unpackedMips_ || levelCount()!=1)return false;
    const auto faceBytes=levels_[0].sourceBytes;
    if(allFaces.size()!=std::uint64_t{faceBytes}*6)return false;
    std::vector<std::uint8_t> bytes;bytes.reserve(stagingBytes_);
    for(unsigned face=0;face<6;++face) {
        const auto p=planNativeColor8ResolveFace(layout_.description,face);
        if(!p || p->storage.sourceBytes!=faceBytes)return false;
        const std::vector<std::uint8_t> source(allFaces.begin()+face*faceBytes,allFaces.begin()+(face+1)*faceBytes);
        const auto linear=linearizeXenosTexture2D(p->storage.description,source);
        if(!linear || linear->size()!=levels_[0].linearBytes)return false;
        bytes.insert(bytes.end(),linear->begin(),linear->end());
    }
    return transfer(bytes,true);
}
std::optional<std::vector<std::uint8_t>> NativeTextureImage::readback(unsigned level,unsigned layer) {
    if(!uploaded_ || level>=levelCount() || layer>=layers_)return std::nullopt;
    std::vector<std::uint8_t> bytes(stagingBytes_);
    if(!transfer(bytes,false))return std::nullopt;
    const auto offset=linearOffsets_[level]+layer*levels_[level].linearBytes;
    return std::vector<std::uint8_t>(bytes.begin()+offset,bytes.begin()+offset+levels_[level].linearBytes);
}
bool NativeTextureImage::transfer(std::vector<std::uint8_t>& bytes,bool upload) {
    if(!image_ || bytes.size()!=stagingBytes_ || (!upload && !uploaded_))return false;
    Transfer t{device_.device};if(!t.setup(device_,stagingBytes_))return false;
    if(upload)std::memcpy(t.mapped,bytes.data(),bytes.size());
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(t.command,&begin)!=VK_SUCCESS)return false;
    if(upload) {
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(t.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&host,0,nullptr,0,nullptr);
    }
    VkImageMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask=uploaded_?VK_ACCESS_SHADER_READ_BIT:0;
    barrier.dstAccessMask=upload?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout=uploaded_?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout=upload?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.image=image_;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,levelCount(),0,layers_};
    vkCmdPipelineBarrier(t.command,uploaded_?VK_PIPELINE_STAGE_ALL_COMMANDS_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    std::vector<VkBufferImageCopy> copies;
    for(unsigned level=0;level<levelCount();++level) {
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,layers_};
        copy.bufferOffset=linearOffsets_[level];
        copy.imageExtent={levels_[level].description.width,levels_[level].description.height,1};
        copies.push_back(copy);
    }
    if(upload)vkCmdCopyBufferToImage(t.command,t.buffer,image_,barrier.newLayout,levelCount(),copies.data());
    else vkCmdCopyImageToBuffer(t.command,image_,barrier.newLayout,t.buffer,levelCount(),copies.data());
    barrier.srcAccessMask=barrier.dstAccessMask;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout=barrier.newLayout;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(t.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,0,nullptr,0,nullptr,1,&barrier);
    if(!upload) {
        VkMemoryBarrier host{};host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(t.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    }
    if(vkEndCommandBuffer(t.command)!=VK_SUCCESS)return false;
    VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&t.command;
    if(recomp::gpu::lockedQueueSubmit(device_.queue,1,&submit,t.fence)!=VK_SUCCESS)return false;
    // A timeout leaves resources in use; do not destruct/free or publish success.
    if(vkWaitForFences(device_.device,1,&t.fence,VK_TRUE,5'000'000'000ULL)!=VK_SUCCESS)std::_Exit(4);
    uploaded_=true;
    if(!upload)std::memcpy(bytes.data(),t.mapped,bytes.size());
    return true;
}
}
