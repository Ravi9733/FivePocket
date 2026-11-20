#pragma once
#include "guest_address_space.h"
#include <cstdint>
#include <string>

namespace recomp::bootstrap {
enum class RuntimeState { Cold, Ready, Running, Suspended, Failed, Stopped };
enum class RendererState { NoSurface, Probing, Ready, Presented, Unsupported, Failed };
class SmokeRuntime final {
public:
    SmokeRuntime() = default;
    ~SmokeRuntime();
    SmokeRuntime(const SmokeRuntime&) = delete;
    SmokeRuntime& operator=(const SmokeRuntime&) = delete;
    bool initialize(const std::string& dataDirectory, const std::string& cacheDirectory);
    void surfaceAttached(int width, int height);
    void rendererReady(const std::string& message);
    void framePresented(const std::string& message);
    void rendererUnsupported(const std::string& message);
    void rendererFailed(const std::string& message);
    void surfaceDetached();
    void resume();
    void suspend();
    void shutdown();
    RuntimeState state() const noexcept { return state_; }
    RendererState rendererState() const noexcept { return rendererState_; }
    std::string describe(const std::string& label = "native") const;
private:
    GuestAddressSpace addressSpace_;
    RuntimeState state_ = RuntimeState::Cold;
    RendererState rendererState_ = RendererState::NoSurface;
    int width_ = 0, height_ = 0;
    std::string dataDirectory_, cacheDirectory_, rendererMessage_;
};
} // namespace recomp::bootstrap
