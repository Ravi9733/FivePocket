#include "smoke_runtime.h"
#include "guest_address_space.h"

#include <cassert>
#include <string>

using recomp::bootstrap::RuntimeState;
using recomp::bootstrap::RendererState;
using recomp::bootstrap::SmokeRuntime;
using recomp::bootstrap::GuestAddressSpace;

int main() {
    {
        GuestAddressSpace addressSpace;
        assert(addressSpace.reserve());
        assert(addressSpace.isReserved());
        assert(addressSpace.base() != nullptr);
        assert(addressSpace.translate(0x82000000U)
                == addressSpace.base() + 0x82000000U);
        *addressSpace.translate(0x82000000U) = 0x5A;
        assert(*addressSpace.translate(0x82000000U) == 0x5A);
        addressSpace.release();
        assert(!addressSpace.isReserved());
    }

    SmokeRuntime runtime;
    assert(runtime.state() == RuntimeState::Cold);

    assert(!runtime.initialize("", "/tmp/cache"));
    assert(runtime.state() == RuntimeState::Failed);

    assert(runtime.initialize("/tmp/data", "/tmp/cache"));
    assert(runtime.state() == RuntimeState::Ready);
    assert(runtime.rendererState() == RendererState::NoSurface);

    runtime.surfaceAttached(1920, 1080);
    assert(runtime.rendererState() == RendererState::Probing);
    assert(runtime.describe("host-test").find("Surface: 1920x1080") != std::string::npos);

    runtime.rendererReady("Vulkan 1.0 swapchain ready");
    assert(runtime.rendererState() == RendererState::Ready);
    runtime.framePresented("Clear/present succeeded");
    assert(runtime.rendererState() == RendererState::Presented);
    assert(runtime.describe("host-test").find("Clear/present succeeded") != std::string::npos);

    runtime.resume();
    assert(runtime.state() == RuntimeState::Running);
    assert(runtime.describe("host-test").find("reserved (4 GiB virtual)")
            != std::string::npos);
    assert(runtime.describe("host-test").find("Game executable: not loaded; no guest execution") != std::string::npos);

    runtime.suspend();
    assert(runtime.state() == RuntimeState::Suspended);
    runtime.resume();
    assert(runtime.state() == RuntimeState::Running);

    runtime.rendererUnsupported("VK_KHR_android_surface unavailable");
    assert(runtime.rendererState() == RendererState::Unsupported);
    runtime.surfaceDetached();
    assert(runtime.rendererState() == RendererState::NoSurface);

    runtime.shutdown();
    assert(runtime.state() == RuntimeState::Stopped);
    assert(runtime.describe("host-test").find("Guest address space: unavailable")
            != std::string::npos);

    // Android may recreate an Activity after a configuration or process event.
    assert(runtime.initialize("/tmp/data", "/tmp/cache"));
    assert(runtime.state() == RuntimeState::Ready);

    runtime.surfaceAttached(1, 1);
    runtime.rendererFailed("vkCreateSwapchainKHR failed");
    assert(runtime.rendererState() == RendererState::Failed);
}
