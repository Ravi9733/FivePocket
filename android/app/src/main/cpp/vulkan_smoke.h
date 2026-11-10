#pragma once
#include "smoke_runtime.h"
#include <android/native_window.h>
#include <cstdint>
#include <vector>

namespace recomp::bootstrap {
class VulkanSmokeRenderer final {
public:
    explicit VulkanSmokeRenderer(SmokeRuntime& runtime) noexcept;
    ~VulkanSmokeRenderer();
    VulkanSmokeRenderer(const VulkanSmokeRenderer&) = delete;
    VulkanSmokeRenderer& operator=(const VulkanSmokeRenderer&) = delete;
    bool configureRgbaDiagnostic(std::vector<std::uint8_t> rgba, bool rejectReadback);
    bool attachWindow(ANativeWindow* ownedWindow, int width, int height);
    bool resize(int width, int height);
    bool updateRgbaDiagnostic(std::vector<std::uint8_t> rgba, bool rejectReadback);
    void detachWindow() noexcept;
    bool hasQueuedPresentation() const noexcept { return queuedPresentations_ != 0; }
    std::uint64_t queuedPresentationCount() const noexcept { return queuedPresentations_; }
private:
    bool initializeDevice();
    bool createSwapchain(int width, int height);
    bool drawOneFrame();
    void destroySwapchain() noexcept;
    void destroyDevice() noexcept;
    SmokeRuntime& runtime_;
    ANativeWindow* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queueFamily_ = 0;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<bool> initializedImages_;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_ = VK_NULL_HANDLE;
    VkSemaphore acquired_ = VK_NULL_HANDLE, rendered_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    std::vector<std::uint8_t> diagnostic_;
    bool configured_ = false, rejectReadback_ = false;
    std::uint64_t queuedPresentations_ = 0;
};
} // namespace recomp::bootstrap
