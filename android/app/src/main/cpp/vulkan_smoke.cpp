#include "vulkan_smoke.h"
#include <android/log.h>
#include <algorithm>
#include <array>
#include <cstring>

namespace recomp::bootstrap {
namespace {
constexpr const char* Tag = "RecompVulkanSmoke";
bool ok(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) return true;
    __android_log_print(ANDROID_LOG_ERROR, Tag, "%s failed: VkResult=%d", operation, int(result));
    return false;
}
template <typename T> T instanceFunction(VkInstance instance, const char* name) {
    return reinterpret_cast<T>(vkGetInstanceProcAddr(instance, name));
}
}
VulkanSmokeRenderer::VulkanSmokeRenderer(SmokeRuntime& runtime) noexcept : runtime_(runtime) {}
VulkanSmokeRenderer::~VulkanSmokeRenderer() { detachWindow(); destroyDevice(); }
bool VulkanSmokeRenderer::configureRgbaDiagnostic(std::vector<std::uint8_t> rgba, bool rejectReadback) {
    if (rgba.empty() || rgba.size() > 128U * 1024U * 1024U) return false;
    diagnostic_ = std::move(rgba);
    rejectReadback_ = rejectReadback;
    configured_ = true;
    return !rejectReadback_;
}
bool VulkanSmokeRenderer::initializeDevice() {
    if (device_) return true;
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app{}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "FivePocket Graphics Self-Test"; app.applicationVersion = 1;
    app.pEngineName = "FivePocket"; app.engineVersion = 1; app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo create{}; create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &app; create.enabledExtensionCount = 2; create.ppEnabledExtensionNames = extensions;
    if (!ok(vkCreateInstance(&create, nullptr, &instance_), "vkCreateInstance")) return false;
    std::uint32_t count = 0;
    if (!ok(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices") || !count) return false;
    std::vector<VkPhysicalDevice> devices(count);
    if (!ok(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "vkEnumeratePhysicalDevices")) return false;
    for (auto candidate : devices) {
        std::uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
        std::vector<VkQueueFamilyProperties> properties(families);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, properties.data());
        for (std::uint32_t i = 0; i < families; ++i) {
            if (properties[i].queueCount && (properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                physical_ = candidate; queueFamily_ = i; break;
            }
        }
        if (physical_) break;
    }
    if (!physical_) return false;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{}; queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily_; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{}; deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1; deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1; deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    if (!ok(vkCreateDevice(physical_, &deviceInfo, nullptr, &device_), "vkCreateDevice")) return false;
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    VkCommandPoolCreateInfo pool{}; pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool.queueFamilyIndex = queueFamily_;
    if (!ok(vkCreateCommandPool(device_, &pool, nullptr, &commandPool_), "vkCreateCommandPool")) return false;
    VkCommandBufferAllocateInfo command{}; command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command.commandPool = commandPool_; command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; command.commandBufferCount = 1;
    if (!ok(vkAllocateCommandBuffers(device_, &command, &command_), "vkAllocateCommandBuffers")) return false;
    VkSemaphoreCreateInfo semaphore{}; semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (!ok(vkCreateSemaphore(device_, &semaphore, nullptr, &acquired_), "vkCreateSemaphore(acquire)") ||
        !ok(vkCreateSemaphore(device_, &semaphore, nullptr, &rendered_), "vkCreateSemaphore(render)") ) return false;
    VkFenceCreateInfo fence{}; fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    return ok(vkCreateFence(device_, &fence, nullptr, &fence_), "vkCreateFence");
}
bool VulkanSmokeRenderer::attachWindow(ANativeWindow* ownedWindow, int width, int height) {
    if (!ownedWindow || window_ || width < 1 || height < 1 || width > 4096 || height > 4096) {
        if (ownedWindow) ANativeWindow_release(ownedWindow);
        return false;
    }
    window_ = ownedWindow; // Takes ownership of the caller's ANativeWindow reference.
    if (!initializeDevice()) { runtime_.rendererUnsupported("Vulkan device or swapchain extensions unavailable"); detachWindow(); return false; }
    const auto createSurface = instanceFunction<PFN_vkCreateAndroidSurfaceKHR>(instance_, "vkCreateAndroidSurfaceKHR");
    if (!createSurface) { runtime_.rendererUnsupported("VK_KHR_android_surface unavailable"); detachWindow(); return false; }
    VkAndroidSurfaceCreateInfoKHR info{}; info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR; info.window = window_;
    if (!ok(createSurface(instance_, &info, nullptr, &surface_), "vkCreateAndroidSurfaceKHR")) {
        runtime_.rendererFailed("Android Vulkan surface creation failed"); detachWindow(); return false;
    }
    runtime_.surfaceAttached(width, height);
    if (!createSwapchain(width, height) || !drawOneFrame()) {
        runtime_.rendererFailed("Vulkan swapchain clear/present failed"); detachWindow(); return false;
    }
    runtime_.rendererReady("Vulkan swapchain ready");
    runtime_.framePresented("Clear/present succeeded");
    return true;
}
bool VulkanSmokeRenderer::createSwapchain(int width, int height) {
    auto getCapabilities = instanceFunction<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(instance_, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    auto getFormats = instanceFunction<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(instance_, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (!getCapabilities || !getFormats) return false;
    VkSurfaceCapabilitiesKHR caps{};
    if (!ok(getCapabilities(physical_, surface_, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return false;
    std::uint32_t formatCount = 0;
    if (!ok(getFormats(physical_, surface_, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR") || !formatCount) return false;
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (!ok(getFormats(physical_, surface_, &formatCount, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR")) return false;
    auto selected = formats.front();
    for (const auto& f : formats) if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) { selected = f; break; }
    format_ = selected.format;
    extent_ = caps.currentExtent;
    if (extent_.width == 0xFFFFFFFFu) {
        extent_.width = std::clamp(static_cast<std::uint32_t>(width), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp(static_cast<std::uint32_t>(height), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    std::uint32_t imageCount = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) imageCount = std::min(imageCount, caps.maxImageCount);
    VkSwapchainCreateInfoKHR info{}; info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface_; info.minImageCount = imageCount; info.imageFormat = selected.format;
    info.imageColorSpace = selected.colorSpace; info.imageExtent = extent_; info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; info.preTransform = caps.currentTransform;
    info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
        ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; info.clipped = VK_TRUE;
    if (!ok(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_), "vkCreateSwapchainKHR")) return false;
    std::uint32_t actual = 0;
    if (!ok(vkGetSwapchainImagesKHR(device_, swapchain_, &actual, nullptr), "vkGetSwapchainImagesKHR")) return false;
    images_.resize(actual);
    if (!ok(vkGetSwapchainImagesKHR(device_, swapchain_, &actual, images_.data()), "vkGetSwapchainImagesKHR")) return false;
    initializedImages_.assign(actual, false);
    return true;
}
bool VulkanSmokeRenderer::drawOneFrame() {
    if (!swapchain_ || !configured_ || rejectReadback_) return false;
    if (!ok(vkWaitForFences(device_, 1, &fence_, VK_TRUE, 5'000'000'000ULL), "vkWaitForFences")) return false;
    std::uint32_t index = 0;
    const VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, 5'000'000'000ULL, acquired_, VK_NULL_HANDLE, &index);
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return ok(acquired, "vkAcquireNextImageKHR");
    if (!ok(vkResetFences(device_, 1, &fence_), "vkResetFences") ||
        !ok(vkResetCommandBuffer(command_, 0), "vkResetCommandBuffer")) return false;
    VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (!ok(vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer")) return false;
    VkImageMemoryBarrier toClear{}; toClear.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toClear.oldLayout = initializedImages_[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
    toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toClear.image = images_[index]; toClear.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toClear.srcAccessMask = 0; toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toClear);
    const VkClearColorValue color{{0.035f, 0.055f, 0.085f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(command_, images_[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    VkImageMemoryBarrier toPresent = toClear;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; toPresent.dstAccessMask = 0;
    vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);
    if (!ok(vkEndCommandBuffer(command_), "vkEndCommandBuffer")) return false;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired_; submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command_;
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &rendered_;
    if (!ok(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit")) return false;
    VkPresentInfoKHR present{}; present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1; present.pWaitSemaphores = &rendered_;
    present.swapchainCount = 1; present.pSwapchains = &swapchain_; present.pImageIndices = &index;
    const VkResult result = vkQueuePresentKHR(queue_, &present);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return ok(result, "vkQueuePresentKHR");
    initializedImages_[index] = true;
    ++queuedPresentations_;
    return true;
}
bool VulkanSmokeRenderer::resize(int width, int height) {
    if (!window_ || width < 1 || height < 1 || width > 4096 || height > 4096) return false;
    if (device_) vkDeviceWaitIdle(device_);
    destroySwapchain();
    if (!createSwapchain(width, height) || !drawOneFrame()) {
        runtime_.rendererFailed("Vulkan swapchain resize failed"); return false;
    }
    runtime_.surfaceAttached(width, height);
    runtime_.framePresented("Clear/present succeeded after resize");
    return true;
}
bool VulkanSmokeRenderer::updateRgbaDiagnostic(std::vector<std::uint8_t> rgba, bool rejectReadback) {
    if (!configureRgbaDiagnostic(std::move(rgba), rejectReadback) || !device_ || !swapchain_) return false;
    return drawOneFrame();
}
void VulkanSmokeRenderer::destroySwapchain() noexcept {
    if (swapchain_ && device_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE; images_.clear(); initializedImages_.clear();
}
void VulkanSmokeRenderer::destroyDevice() noexcept {
    if (device_) vkDeviceWaitIdle(device_);
    if (fence_ && device_) vkDestroyFence(device_, fence_, nullptr);
    if (acquired_ && device_) vkDestroySemaphore(device_, acquired_, nullptr);
    if (rendered_ && device_) vkDestroySemaphore(device_, rendered_, nullptr);
    if (commandPool_ && device_) vkDestroyCommandPool(device_, commandPool_, nullptr);
    if (device_) vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE; fence_ = VK_NULL_HANDLE; acquired_ = rendered_ = VK_NULL_HANDLE;
    commandPool_ = VK_NULL_HANDLE; command_ = VK_NULL_HANDLE; queue_ = VK_NULL_HANDLE;
    if (surface_ && instance_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (instance_) vkDestroyInstance(instance_, nullptr);
    instance_ = VK_NULL_HANDLE; physical_ = VK_NULL_HANDLE;
}
void VulkanSmokeRenderer::detachWindow() noexcept {
    if (device_) vkDeviceWaitIdle(device_);
    destroySwapchain();
    if (surface_ && instance_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (window_) ANativeWindow_release(window_);
    window_ = nullptr;
    runtime_.surfaceDetached();
}
} // namespace recomp::bootstrap
