#include "device.h"
#include "process_limits.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// Capability/ordering experiment only. No original game, guest memory or
// counter substitution. Across-submit continuity requires explicitly enabled
// calibrated_timestamps (EXT or KHR); ordinary timestamps do not promise it.
int main(){
    using namespace recomp::gpu;
    if(!initializeProbeProcess())return 2;
    ProbeDevice context;if(!context.inspect())return 1;
    std::uint32_t count=0;
    vkGetPhysicalDeviceQueueFamilyProperties(context.physical,&count,nullptr);
    if(!count || count>256)return 1;
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(context.physical,&count,queues.data());
    bool found=false;
    for(unsigned i=0;i<count;++i)if(queues[i].queueCount &&
        (queues[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){
        context.queueFamily=i;found=true;break;
    }
    if(!found)return 1;
    const auto bits=queues[context.queueFamily].timestampValidBits;
    const auto period=context.properties.limits.timestampPeriod;
    if(bits>64)return 1;
    if(!bits){std::puts("NATIVE_TIMESTAMP_PROBE: unsupported timestampValidBits=0 guest_writes=0");return 0;}
    if(!std::isfinite(period) || period<=0)return 1;
    count=0;
    if(vkEnumerateDeviceExtensionProperties(context.physical,nullptr,&count,nullptr)!=VK_SUCCESS || count>1024)return 1;
    std::vector<VkExtensionProperties> extensions(count);
    if(vkEnumerateDeviceExtensionProperties(context.physical,nullptr,&count,extensions.data())!=VK_SUCCESS)return 1;
    const char* calibratedName=nullptr;
    for(const auto& extension:extensions)if(!std::strcmp(extension.extensionName,VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) ||
        !std::strcmp(extension.extensionName,"VK_KHR_calibrated_timestamps"))calibratedName=extension.extensionName;
    const bool calibrated=calibratedName!=nullptr;
    const float priority=1;
    VkDeviceQueueCreateInfo queueInfo{};queueInfo.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex=context.queueFamily;queueInfo.queueCount=1;queueInfo.pQueuePriorities=&priority;
    VkDeviceCreateInfo deviceInfo{};deviceInfo.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount=1;deviceInfo.pQueueCreateInfos=&queueInfo;
    if(calibrated){deviceInfo.enabledExtensionCount=1;deviceInfo.ppEnabledExtensionNames=&calibratedName;}
    if(vkCreateDevice(context.physical,&deviceInfo,nullptr,&context.device)!=VK_SUCCESS)return 1;
    vkGetDeviceQueue(context.device,context.queueFamily,0,&context.queue);
    if(!context.queue)return 1;
    VkCommandPool pool=VK_NULL_HANDLE;VkQueryPool queries=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;
    const auto release=[&]{
        if(fence)vkDestroyFence(context.device,fence,nullptr);
        if(queries)vkDestroyQueryPool(context.device,queries,nullptr);
        if(pool)vkDestroyCommandPool(context.device,pool,nullptr);
    };
    VkCommandPoolCreateInfo poolInfo{};poolInfo.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;poolInfo.queueFamilyIndex=context.queueFamily;
    if(vkCreateCommandPool(context.device,&poolInfo,nullptr,&pool)!=VK_SUCCESS)return 1;
    VkQueryPoolCreateInfo queryInfo{};queryInfo.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryInfo.queryType=VK_QUERY_TYPE_TIMESTAMP;queryInfo.queryCount=6;
    VkFenceCreateInfo fenceInfo{};fenceInfo.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if(vkCreateQueryPool(context.device,&queryInfo,nullptr,&queries)!=VK_SUCCESS ||
        vkCreateFence(context.device,&fenceInfo,nullptr,&fence)!=VK_SUCCESS){release();return 1;}
    const auto mask=bits==64?UINT64_MAX:(std::uint64_t{1}<<bits)-1;
    std::uint64_t previous=0;
    for(unsigned sample=0;sample<3;++sample){
        VkCommandBufferAllocateInfo allocate{};allocate.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool=pool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
        VkCommandBuffer command=VK_NULL_HANDLE;
        if(vkAllocateCommandBuffers(context.device,&allocate,&command)!=VK_SUCCESS){release();return 1;}
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(vkBeginCommandBuffer(command,&begin)!=VK_SUCCESS){release();return 1;}
        vkCmdResetQueryPool(command,queries,sample*2,2);
        vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,sample*2);
        VkMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,sample*2+1);
        if(vkEndCommandBuffer(command)!=VK_SUCCESS || vkResetFences(context.device,1,&fence)!=VK_SUCCESS){release();return 1;}
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        if(vkQueueSubmit(context.queue,1,&submit,fence)!=VK_SUCCESS || vkWaitForFences(context.device,1,&fence,VK_TRUE,2'000'000'000)!=VK_SUCCESS){
            std::fflush(stdout);std::_Exit(4); // Do not destroy potentially in-flight objects.
        }
        std::array<std::uint64_t,4> result{};
        if(vkGetQueryPoolResults(context.device,queries,sample*2,2,sizeof(result),result.data(),16,
            VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)!=VK_SUCCESS || !result[1] || !result[3]){release();return 1;}
        const auto start=result[0]&mask,end=result[2]&mask,elapsed=(end-start)&mask;
        const auto cross=(start-previous)&mask;
        if(double(elapsed)*period>2e9 || (sample && calibrated && double(cross)*period>2e9)){release();return 1;}
        std::printf("NATIVE_TIMESTAMP_SAMPLE index=%u valid_bits=%u period_ns=%.9g start=%llu end=%llu elapsed_ns=%.9g cross_submit_ns=%.9g\n",
            sample,bits,double(period),static_cast<unsigned long long>(start),static_cast<unsigned long long>(end),
            double(elapsed)*period,sample&&calibrated?double(cross)*period:-1.0);
        previous=end;
    }
    release();
    std::printf("NATIVE_TIMESTAMP_PROBE: samples=3 queries=6 real_fences=3 calibrated_extension_enabled=%u cross_submit_contract=%u guest_writes=0 presentation=0 PASS\n",
        unsigned(calibrated),unsigned(calibrated));
}
