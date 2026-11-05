// Concurrency test for NativeAudioRenderClient/NativeAudioQueue under L_audio (multicore plan WP4).
// Thread W is the native callback worker: poll, beginCallback, hand the callback to the guest,
// finishCallback. Thread S runs XAudioSubmitRenderDriverFrame's path: submit while holding a
// stand-in registry mutex (R ranks above L_audio). Thread Q runs the other import handlers
// (XAudioEnableDucker / XAudioGetVoiceCategoryVolume / registration: statistics, canActivate), also
// under R, throughout. The backend is a synthetic sink that consumes immediately. Build it with
// ThreadSanitizer (see host_support/run_wp4_host_tests.sh); the plain build checks the protocol.
#include "native_audio_render_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

using namespace recomp::audio;

namespace {
[[noreturn]] void failTest(const char* what, unsigned line) {
    std::fprintf(stderr, "NATIVE_AUDIO_CONCURRENCY_FAIL line=%u %s\n", line, what);
    std::abort();
}
#define CHECK(condition) do { if (!(condition)) failTest(#condition, __LINE__); } while (0)

// Called only by the client, under L_audio; read by main after join.
class Sink final : public NativeAudioPlaybackBackend {
public:
    recomp::bootstrap::NativeAudioConfiguration config{48000, 2, 4096, 1024};
    bool starting = false;
    std::uint64_t written = 0, read = 0, calls = 0;
    std::optional<recomp::bootstrap::NativeAudioConfiguration> configuration() const noexcept override { return config; }
    bool started() const noexcept override { return starting; }
    bool requestStart() noexcept override { starting = true; return true; }
    std::optional<std::uint64_t> framesWritten() const noexcept override { return written; }
    std::optional<std::uint64_t> framesRead() const noexcept override { return read; }
    std::int32_t writeStereo(const float*, std::uint32_t frames) noexcept override {
        ++calls;
        const auto n = std::min<std::uint64_t>(frames, 300); // partial writes
        written += n; read = written;
        return static_cast<std::int32_t>(n);
    }
};
std::array<std::uint8_t, AudioGuestPacketBytes> Frame{};
constexpr std::uint32_t Token = 0x64184000;
}

int main() {
    auto sink = std::make_shared<Sink>();
    NativeAudioRenderClient client(sink);
    std::mutex registry; // Stand-in for the registry mutex R.
    {
        std::lock_guard lock(registry);
        CHECK(client.canActivate());
        client.publishWorker(Token); // dispatchAudioRegister, under R
    }
    constexpr unsigned long long Callbacks = 20000;
    std::atomic<int> handoff{0}; // 1: worker is inside a callback and wants a packet; 2: submitted
    std::atomic<bool> stop{false};
    std::atomic<unsigned long long> waits{0}, snapshots{0};
    std::thread worker([&] {
        unsigned long long completed = 0;
        while (completed < Callbacks) {
            const auto demand = client.poll(Token);
            CHECK(demand != NativeAudioRenderClient::Demand::Failed);
            if (demand == NativeAudioRenderClient::Demand::Wait) { ++waits; std::this_thread::yield(); continue; }
            CHECK(client.beginCallback(Token));
            handoff.store(1, std::memory_order_release);
            while (handoff.load(std::memory_order_acquire) != 2) std::this_thread::yield();
            handoff.store(0, std::memory_order_relaxed);
            CHECK(client.finishCallback(Token));
            ++completed;
        }
        stop = true;
    });
    std::thread submitter([&] {
        while (!stop.load()) {
            if (handoff.load(std::memory_order_acquire) != 1) { std::this_thread::yield(); continue; }
            {
                std::lock_guard lock(registry);
                CHECK(client.submit(Token, NativeAudioRenderClient::Handle, Frame.data(), Frame.size()));
                CHECK(!client.submit(Token, NativeAudioRenderClient::Handle, Frame.data(), Frame.size()));
                CHECK(!client.submit(Token + 0x1000, NativeAudioRenderClient::Handle, Frame.data(), Frame.size()));
            }
            handoff.store(2, std::memory_order_release);
        }
    });
    std::thread handlers([&] {
        NativeAudioRenderClient::Statistics previous{};
        while (!stop.load()) {
            {
                std::lock_guard lock(registry);
                const auto s = client.statistics();
                CHECK(!s.failed && s.workerToken == Token);
                CHECK(s.callbacksCompleted <= s.callbacksEntered && s.callbacksEntered <= s.callbacksCompleted + 1);
                CHECK(s.acceptedPackets >= previous.acceptedPackets && s.callbacksCompleted >= previous.callbacksCompleted);
                CHECK(s.writtenFrames >= previous.writtenFrames && s.serviceReadFrames <= s.writtenFrames);
                CHECK(s.pendingPackets <= NativeAudioQueue::CapacityPackets);
                CHECK(!client.canActivate());
                previous = s;
                ++snapshots;
            }
            // Import handlers run between other guest work; re-locking R at once would starve the
            // submitter on an unfair mutex.
            std::this_thread::yield();
        }
    });
    worker.join();
    submitter.join();
    handlers.join();
    const auto s = client.statistics();
    CHECK(!s.failed && s.callbacksEntered == Callbacks && s.callbacksCompleted == Callbacks);
    CHECK(s.acceptedPackets == Callbacks && !s.callbackActive);
    CHECK(s.writtenFrames == sink->written && s.startRequested && sink->starting);
    std::printf("NATIVE_AUDIO_CONCURRENCY_PASS callbacks=%llu written_frames=%llu sink_calls=%llu waits=%llu snapshots=%llu\n",
        static_cast<unsigned long long>(s.callbacksCompleted), static_cast<unsigned long long>(s.writtenFrames),
        static_cast<unsigned long long>(sink->calls), waits.load(), snapshots.load());
}
