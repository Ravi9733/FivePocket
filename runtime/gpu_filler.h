#pragma once
// v569: paced by the unit's own GPU cost (see run()). v568 (driving goal, 2026-10-21; MEASUREMENT ONLY, the owner's yes: "build a measurement-only keep-warm GPU filler that runs
// only in the idle gaps"): diag-measure-gpu-filler. While that switch file exists, a thread of its own keeps a separate
// Vulkan device created with VK_QUEUE_GLOBAL_PRIORITY_LOW_EXT (the Fold's Adreno 730 offers VK_EXT_global_priority; the
// game's device keeps the default MEDIUM) and submits small units of GPU work to it: buffer-to-buffer copies, the kind
// that raised the Fold's GPU clock 285 -> 439 MHz in gbs1. The kernel driver runs the low-priority context only when the
// game's context has nothing queued, so the units land in the GPU's idle gaps. After each unit (submit to fence: W ms of
// wall time) the thread sleeps W*(100-duty)/duty ms, so the units take about duty % of wall time (the file's text, 1-60,
// default 15; W includes any wait behind the game's work, so the GPU time actually filled is at most that). Nothing of the
// game's rendering is touched: own instance, device, memory, queue, thread. Question: does holding the GPU busier raise
// the governor's clock (gpb1/gpb2: ~70 % of the game's pass time scales with 1/clock) and shorten the driving frame?
// Logs RENDER_FILLER every ~2 s. Absent switch: one access() a second.
#include <vulkan/vulkan.h>
#include <android/log.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
namespace recomp_filler {
#define RECOMP_FILLER_LOG(...) __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",__VA_ARGS__)
struct Gpu {
    VkInstance inst=VK_NULL_HANDLE;VkDevice dev=VK_NULL_HANDLE;VkQueue q=VK_NULL_HANDLE;VkBuffer src=VK_NULL_HANDLE,dst=VK_NULL_HANDLE;
    VkDeviceMemory mem=VK_NULL_HANDLE;VkCommandPool pool=VK_NULL_HANDLE;VkCommandBuffer cb=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    bool ok=false;
};
static constexpr VkDeviceSize UnitBytes=8u<<20;static constexpr unsigned CopiesPerUnit=4;
inline bool makeGpu(Gpu& g) noexcept {
    VkApplicationInfo a{VK_STRUCTURE_TYPE_APPLICATION_INFO};a.apiVersion=VK_API_VERSION_1_1;a.pApplicationName="recomp-filler";
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ic.pApplicationInfo=&a;
    if(vkCreateInstance(&ic,nullptr,&g.inst)!=VK_SUCCESS){g.inst=VK_NULL_HANDLE;return false;}
    std::uint32_t n=1;VkPhysicalDevice pd=VK_NULL_HANDLE;
    if(vkEnumeratePhysicalDevices(g.inst,&n,&pd)<0 || !n || !pd)return false;
    VkDeviceQueueGlobalPriorityCreateInfoEXT gp{VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_EXT};gp.globalPriority=VK_QUEUE_GLOBAL_PRIORITY_LOW_EXT;
    const float one=1.0f;VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.pNext=&gp;qc.queueFamilyIndex=0;qc.queueCount=1;qc.pQueuePriorities=&one;
    const char* ext[]={"VK_EXT_global_priority"};
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;dc.enabledExtensionCount=1;dc.ppEnabledExtensionNames=ext;
    const VkResult dr=vkCreateDevice(pd,&dc,nullptr,&g.dev);
    if(dr!=VK_SUCCESS){g.dev=VK_NULL_HANDLE;RECOMP_FILLER_LOG("RENDER_FILLER init=failed device=%d",int(dr));return false;}
    vkGetDeviceQueue(g.dev,0,0,&g.q);
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=UnitBytes;bc.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if(vkCreateBuffer(g.dev,&bc,nullptr,&g.src)!=VK_SUCCESS || vkCreateBuffer(g.dev,&bc,nullptr,&g.dst)!=VK_SUCCESS)return false;
    VkMemoryRequirements r{};vkGetBufferMemoryRequirements(g.dev,g.src,&r);
    VkPhysicalDeviceMemoryProperties mp{};vkGetPhysicalDeviceMemoryProperties(pd,&mp);
    std::uint32_t type=~0u;for(std::uint32_t i=0;i<mp.memoryTypeCount;++i)
        if((r.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
    if(type==~0u)return false;
    const VkDeviceSize stride=(r.size+r.alignment-1)/r.alignment*r.alignment;
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=2*stride;ma.memoryTypeIndex=type;
    if(vkAllocateMemory(g.dev,&ma,nullptr,&g.mem)!=VK_SUCCESS){g.mem=VK_NULL_HANDLE;return false;}
    if(vkBindBufferMemory(g.dev,g.src,g.mem,0)!=VK_SUCCESS || vkBindBufferMemory(g.dev,g.dst,g.mem,stride)!=VK_SUCCESS)return false;
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=0;
    if(vkCreateCommandPool(g.dev,&pc,nullptr,&g.pool)!=VK_SUCCESS){g.pool=VK_NULL_HANDLE;return false;}
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=g.pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
    if(vkAllocateCommandBuffers(g.dev,&ca,&g.cb)!=VK_SUCCESS)return false;
    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; // (recorded once, submitted again and again)
    if(vkBeginCommandBuffer(g.cb,&b)!=VK_SUCCESS)return false;
    VkBufferCopy c{0,0,UnitBytes};
    for(unsigned k=0;k<CopiesPerUnit;++k) {
        vkCmdCopyBuffer(g.cb,k&1?g.dst:g.src,k&1?g.src:g.dst,1,&c);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(g.cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,nullptr,0,nullptr);
    }
    if(vkEndCommandBuffer(g.cb)!=VK_SUCCESS)return false;
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if(vkCreateFence(g.dev,&fc,nullptr,&g.fence)!=VK_SUCCESS){g.fence=VK_NULL_HANDLE;return false;}
    return g.ok=true;
}
inline int readDuty(const char* path) noexcept { // -1: the switch is absent
    FILE* f=std::fopen(path,"r");if(!f)return -1;
    int d=15;if(std::fscanf(f,"%d",&d)!=1)d=15;std::fclose(f);
    return d<1?1:d>60?60:d;
}
inline void run() noexcept {
    using Clock=std::chrono::steady_clock;
    const char* path=RECOMP_APP_PATH("/private-trace/diag-measure-gpu-filler");
    Gpu g;bool tried=false;
    for(;;) {
        int duty=readDuty(path);
        if(duty<0){std::this_thread::sleep_for(std::chrono::seconds(1));continue;}
        if(!tried){tried=true;const bool ok=makeGpu(g);RECOMP_FILLER_LOG("RENDER_FILLER init=%s duty=%d unit_mb=%u",ok?"ok":"failed",duty,unsigned(CopiesPerUnit*(UnitBytes>>20)));}
        if(!g.ok){std::this_thread::sleep_for(std::chrono::seconds(5));continue;}
        auto logAt=Clock::now()+std::chrono::seconds(2),pollAt=Clock::now()+std::chrono::seconds(1);
        std::uint64_t units=0,failed=0;double wallMs=0,minMs=1e9,sleptMs=0,ownMs=1e9,ownGpuMs=0;const auto t0=Clock::now();
        RECOMP_FILLER_LOG("RENDER_FILLER on=1 duty=%d",duty);
        while(duty>0) {
            const auto s=Clock::now();
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&g.cb;
            vkResetFences(g.dev,1,&g.fence);
            if(vkQueueSubmit(g.q,1,&si,g.fence)!=VK_SUCCESS || vkWaitForFences(g.dev,1,&g.fence,VK_TRUE,1000000000ull)!=VK_SUCCESS) {
                ++failed;std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else {
                const double w=std::chrono::duration<double,std::milli>(Clock::now()-s).count();++units;wallMs+=w;if(w<minMs)minMs=w;
                // v569: pace by the unit's OWN GPU cost (the fastest unit of the last ~2 s: one that found the GPU idle), not by
                // its wall time (v568's gf1-1: mean 8.4 ms of mostly waiting behind the game, min 2.1 ms, so the GPU it filled
                // was ~1/4 of the duty asked). duty % of wall time is then the filler's own GPU work.
                if(w<ownMs)ownMs=w;
                const double rest=std::min(200.0,ownMs*(100.0-duty)/duty);sleptMs+=rest;ownGpuMs+=ownMs;
                std::this_thread::sleep_for(std::chrono::duration<double,std::milli>(rest));
            }
            const auto now=Clock::now();
            if(now>=pollAt){pollAt=now+std::chrono::seconds(1);duty=readDuty(path);}
            if(now>=logAt || duty<=0) {
                const double span=std::chrono::duration<double,std::milli>(now-t0).count();
                RECOMP_FILLER_LOG("RENDER_FILLER on=%u duty=%d units=%llu failed=%llu unit_wall_ms_mean=%.2f unit_wall_ms_min=%.2f wall_share=%.1f slept_share=%.1f own_gpu_share=%.1f",
                    unsigned(duty>0),duty,(unsigned long long)units,(unsigned long long)failed,units?wallMs/units:0.0,units?minMs:0.0,
                    span>0?100.0*wallMs/span:0.0,span>0?100.0*sleptMs/span:0.0,span>0?100.0*ownGpuMs/span:0.0);
                logAt=now+std::chrono::seconds(2);ownMs=1e9; // (v569: the own-cost estimate restarts each log period)
            }
        }
    }
}
inline void start() noexcept {
    static std::once_flag once;
    std::call_once(once,[]{try{std::thread(run).detach();}catch(...){RECOMP_FILLER_LOG("RENDER_FILLER thread=failed");}});
}
} // namespace recomp_filler
