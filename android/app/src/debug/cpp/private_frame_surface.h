#pragma once
#include "private_trace_api.h"
#include "private_display_sequence.h"
#include "smoke_runtime.h"
#include "vulkan_smoke.h"
#include <android/log.h>
#include <android/native_window.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <memory>
#include <vector>

// Owned-image GPU-audited consumer for separate stopped/live C ABIs. It never
// owns guest memory, resumes guest writers, publishes guest commands or claims
// vblank. Caller enforces its ABI's writer lifetime/parking contract.
class PrivateFrameSurface final {
public:
    bool attach(ANativeWindow* owned,int width,int height,bool reject,bool failAfterPresent=false,bool rejectSecond=false){
        std::lock_guard lock(mutex_);
        if(!owned || window_ || width<1 || height<1 || width>4096 || height>4096){
            if(owned)ANativeWindow_release(owned);return false;
        }
        window_=owned;width_=width;height_=height;reject_=reject;failAfterPresent_=failAfterPresent;
        rejectSecond_=rejectSecond;generation_.fetch_add(1,std::memory_order_acq_rel);return true;
    }
    // V6 (library direct present, attempt v255): a new reference to the current window for the library (released with
    // releaseForLibrary) and the generation it belongs to; the generation changes at every attach and detach.
    ANativeWindow* acquireForLibrary(std::uint64_t* generation){
        std::lock_guard lock(mutex_);
        if(generation)*generation=generation_.load(std::memory_order_acquire);
        if(!window_)return nullptr;
        ANativeWindow_acquire(window_);return window_;
    }
    static void releaseForLibrary(ANativeWindow* window){if(window)ANativeWindow_release(window);}
    std::uint64_t windowGeneration() const noexcept {return generation_.load(std::memory_order_acquire);}
    void resize(int width,int height){
        std::lock_guard lock(mutex_);
        if(width<1 || height<1 || width>4096 || height>4096)return;
        width_=width;height_=height;
        auto& active=sequencedRenderer_?*sequencedRenderer_:renderer_;
        if(presented_ && !active.resize(width,height)){
            presented_=false;__android_log_print(ANDROID_LOG_ERROR,"RecompPrivateTrace","STOPPED_WINDOW_RESIZE_REJECTED");
        }
    }
    void detach(){
        std::lock_guard lock(mutex_);
        renderer_.detachWindow();presented_=false;
        if(sequencedRenderer_)sequencedRenderer_->detachWindow();
        if(window_){ANativeWindow_release(window_);window_=nullptr;}
        generation_.fetch_add(1,std::memory_order_acq_rel);
    }
    int consume(const std::uint8_t* rgba,std::uint32_t bytes,std::uint32_t width,std::uint32_t height,bool live=false) noexcept {
        try {
            std::lock_guard lock(mutex_);
            if(consumed_ || sequence_.next() || !window_ || !rgba || bytes!=1280U*720U*4U || width!=1280 || height!=720)return 0;
            consumed_=true;
            // Copy before returning across the C ABI. Renderer then owns source
            // bytes and an additional native-window reference, not a guest lease.
            std::vector<std::uint8_t> owned(rgba,rgba+bytes);
            if(!renderer_.configureRgbaDiagnostic(std::move(owned),reject_))return 0;
            ANativeWindow_acquire(window_);
            presented_=renderer_.attachWindow(window_,width_,height_);
            if(live && failAfterPresent_ && presented_){
                __android_log_print(ANDROID_LOG_INFO,"RecompPrivateTrace","LIVE_WINDOW_POST_ACCEPT_FAILURE_TEST");
                throw 1; // Debug-only fault after real QueuePresent acceptance.
            }
            // QueuePresent may already have accepted a frame even if later
            // idle/diagnostic work fails. Never return a retryable rejection
            // after acceptance. This one-shot consumer has no older frames.
            if(renderer_.hasQueuedPresentation()!=presented_)std::_Exit(4);
            __android_log_print(ANDROID_LOG_INFO,"RecompPrivateTrace","%s",live?
                (presented_?"LIVE_WINDOW_GPU_AUDIT_PASS_PRESENT_QUEUED":"LIVE_WINDOW_GPU_AUDIT_REJECTED"):
                (presented_?"STOPPED_WINDOW_GPU_AUDIT_PASS_PRESENT_QUEUED":"STOPPED_WINDOW_GPU_AUDIT_REJECTED"));
            return presented_?1:0;
        }catch(...){if(renderer_.hasQueuedPresentation())std::_Exit(4);return 0;}
    }
    int consumeSequenced(std::uint32_t sequence,const std::uint8_t* rgba,std::uint32_t bytes,
            std::uint32_t width,std::uint32_t height)noexcept{
        try {
            std::lock_guard lock(mutex_);
            if(consumed_ || !window_ || !rgba || bytes!=1280U*720U*4U || width!=1280 || height!=720 ||
                    !sequence_.begin(sequence))return 0;
            const auto acceptedBefore=sequencedRenderer_?sequencedRenderer_->queuedPresentationCount():0;
            try {
                // Keep the device, swapchain and transfer resources alive.
                // A per-attempt acceptance delta prevents an old frame's
                // success from being mistaken for this frame's completion.
                std::vector<std::uint8_t> owned(rgba,rgba+bytes);
                const bool reject=reject_ || (rejectSecond_ && sequence==1);
                if(sequencedRenderer_) {
                    presented_=sequencedRenderer_->updateRgbaDiagnostic(std::move(owned),reject);
                } else {
                    sequencedRenderer_=std::make_unique<recomp::bootstrap::VulkanSmokeRenderer>(runtime_);
                    if(!sequencedRenderer_->configureRgbaDiagnostic(std::move(owned),reject)){
                        sequence_.finish(false);return 0;
                    }
                    ANativeWindow_acquire(window_);
                    presented_=sequencedRenderer_->attachWindow(window_,width_,height_);
                }
                if(sequencedRenderer_->queuedPresentationCount()-acceptedBefore!=(presented_?1U:0U))std::_Exit(4);
                if(failAfterPresent_ && sequence==1 && presented_){
                    __android_log_print(ANDROID_LOG_INFO,"RecompPrivateTrace","LIVE_DISPLAY_POST_ACCEPT_FAILURE_TEST sequence=%u",sequence);
                    std::_Exit(4);
                }
                __android_log_print(ANDROID_LOG_INFO,"RecompPrivateTrace","%s sequence=%u",presented_?
                    "LIVE_DISPLAY_GPU_AUDIT_PASS_PRESENT_QUEUED":"LIVE_DISPLAY_GPU_AUDIT_REJECTED",sequence);
                return sequence_.finish(presented_)?1:0;
            }catch(...){
                if(sequencedRenderer_ && sequencedRenderer_->queuedPresentationCount()!=acceptedBefore)std::_Exit(4);
                sequence_.finish(false);return 0;
            }
        }catch(...){return 0;}
    }
private:
    std::mutex mutex_;
    // Graphics diagnostic state only: do NOT initialize another guest address
    // space or display its asset-free runtime footer as the running game's state.
    recomp::bootstrap::SmokeRuntime runtime_;
    recomp::bootstrap::VulkanSmokeRenderer renderer_{runtime_};
    std::unique_ptr<recomp::bootstrap::VulkanSmokeRenderer> sequencedRenderer_;
    recomp::private_trace::DisplaySequence sequence_;
    ANativeWindow* window_=nullptr;
    std::atomic<std::uint64_t> generation_{0};
    int width_=0,height_=0;
    bool consumed_=false,presented_=false,reject_=false,failAfterPresent_=false,rejectSecond_=false;
};
