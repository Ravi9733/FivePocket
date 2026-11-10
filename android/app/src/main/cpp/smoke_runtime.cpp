#include "smoke_runtime.h"
#include <sstream>

namespace recomp::bootstrap {
SmokeRuntime::~SmokeRuntime() { shutdown(); }
bool SmokeRuntime::initialize(const std::string& dataDirectory, const std::string& cacheDirectory) {
    if (state_ == RuntimeState::Ready || state_ == RuntimeState::Running ||
        state_ == RuntimeState::Suspended) return !dataDirectory_.empty() && !cacheDirectory_.empty();
    if (dataDirectory.empty() || cacheDirectory.empty()) {
        state_ = RuntimeState::Failed;
        return false;
    }
    if (!addressSpace_.reserve()) {
        state_ = RuntimeState::Failed;
        return false;
    }
    dataDirectory_ = dataDirectory;
    cacheDirectory_ = cacheDirectory;
    rendererState_ = RendererState::NoSurface;
    rendererMessage_.clear();
    state_ = RuntimeState::Ready;
    return true;
}
void SmokeRuntime::surfaceAttached(int width, int height) {
    if (state_ == RuntimeState::Cold || state_ == RuntimeState::Failed ||
        state_ == RuntimeState::Stopped || width <= 0 || height <= 0) return;
    width_ = width; height_ = height;
    rendererState_ = RendererState::Probing;
    rendererMessage_ = "Vulkan surface attached";
}
void SmokeRuntime::rendererReady(const std::string& message) {
    rendererState_ = RendererState::Ready; rendererMessage_ = message;
}
void SmokeRuntime::framePresented(const std::string& message) {
    rendererState_ = RendererState::Presented; rendererMessage_ = message;
}
void SmokeRuntime::rendererUnsupported(const std::string& message) {
    rendererState_ = RendererState::Unsupported; rendererMessage_ = message;
}
void SmokeRuntime::rendererFailed(const std::string& message) {
    rendererState_ = RendererState::Failed; rendererMessage_ = message;
}
void SmokeRuntime::surfaceDetached() {
    width_ = height_ = 0;
    rendererState_ = RendererState::NoSurface;
    rendererMessage_.clear();
}
void SmokeRuntime::resume() {
    if (state_ == RuntimeState::Ready || state_ == RuntimeState::Suspended)
        state_ = RuntimeState::Running;
}
void SmokeRuntime::suspend() {
    if (state_ == RuntimeState::Running) state_ = RuntimeState::Suspended;
}
void SmokeRuntime::shutdown() {
    if (state_ == RuntimeState::Stopped) return;
    addressSpace_.release();
    dataDirectory_.clear(); cacheDirectory_.clear();
    width_ = height_ = 0;
    rendererState_ = RendererState::NoSurface;
    rendererMessage_.clear();
    state_ = RuntimeState::Stopped;
}
std::string SmokeRuntime::describe(const std::string& label) const {
    std::ostringstream out;
    out << "Runtime: " << label << "\n";
    out << "State: ";
    switch (state_) {
        case RuntimeState::Cold: out << "cold"; break;
        case RuntimeState::Ready: out << "ready"; break;
        case RuntimeState::Running: out << "running"; break;
        case RuntimeState::Suspended: out << "suspended"; break;
        case RuntimeState::Failed: out << "failed"; break;
        case RuntimeState::Stopped: out << "stopped"; break;
    }
    out << "\nRenderer: ";
    switch (rendererState_) {
        case RendererState::NoSurface: out << "no surface"; break;
        case RendererState::Probing: out << "probing"; break;
        case RendererState::Ready: out << "ready"; break;
        case RendererState::Presented: out << "presented"; break;
        case RendererState::Unsupported: out << "unsupported"; break;
        case RendererState::Failed: out << "failed"; break;
    }
    if (width_ > 0 && height_ > 0) out << "\nSurface: " << width_ << "x" << height_;
    if (!rendererMessage_.empty()) out << "\n" << rendererMessage_;
    out << "\nGuest address space: " << (addressSpace_.isReserved() ? "reserved (4 GiB virtual)" : "unavailable");
    out << "\nGame executable: not loaded; no guest execution";
    return out.str();
}
} // namespace recomp::bootstrap
