// Concurrency test for the multicore XMA pool (local-analysis/attempts/v12/src/native_xma_context_pool.cpp).
// Several guest-like threads kick, poll (the 7FEA1818 continuous-decode pump), drain and refill
// their own contexts while other threads pause/resume the engine, lock/clear a context, and
// allocate/release contexts while holding a stand-in registry mutex (as XMACreateContext and
// XMAReleaseContext do). One context is released and reallocated while other threads pump it.
// Deterministic checks: map_ is never called with an XMA lock held (the registry mutex R ranks
// above L_xma); a guest-owned tag in context word 9 is never rewritten by decoder publication;
// every access succeeds; counters and storage are consistent after join. Build it with
// ThreadSanitizer (see host_support/run_wp4_host_tests.sh); the codec is the host stand-in.
#include "native_xma_context_pool.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace recomp::audio { unsigned nativeXmaLocksHeldForTest() noexcept; }
using namespace recomp::audio;
using recomp::bootstrap::GuestXmaBackend;

namespace {
[[noreturn]] void failTest(const char* what, unsigned line) {
    std::fprintf(stderr, "NATIVE_XMA_CONCURRENCY_FAIL line=%u %s\n", line, what);
    std::abort();
}
#define CHECK(condition) do { if (!(condition)) failTest(#condition, __LINE__); } while (0)

std::uint32_t* word(std::uint8_t* p) { return reinterpret_cast<std::uint32_t*>(p); }
// Guest-side context access: aligned 32-bit big-endian atomics, as the multicore memory model does.
std::uint32_t load(std::uint8_t* p) { return __builtin_bswap32(__atomic_load_n(word(p), __ATOMIC_ACQUIRE)); }
void store(std::uint8_t* p, std::uint32_t v) { __atomic_store_n(word(p), __builtin_bswap32(v), __ATOMIC_RELEASE); }
void setBits(std::uint8_t* p, std::uint32_t bits) { __atomic_fetch_or(word(p), __builtin_bswap32(bits), __ATOMIC_ACQ_REL); }
template<class F> void update(std::uint8_t* p, F change) {
    auto raw = __atomic_load_n(word(p), __ATOMIC_ACQUIRE);
    while (!__atomic_compare_exchange_n(word(p), &raw, __builtin_bswap32(change(__builtin_bswap32(raw))),
            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
}

constexpr std::uint32_t PoolVirtual = 0xA1000000, PoolPhysical = 0x1000000;
constexpr unsigned CapacityBlocks = 8, FrameBytes = 1024; // mono: one frame is four 256-byte blocks

// One guest-owned stream: its output ring and two single-packet input buffers.
struct Stream {
    std::uint32_t outputPhysical = 0;
    std::array<std::uint32_t, 2> inputPhysical{};
    std::vector<std::uint8_t> output = std::vector<std::uint8_t>(CapacityBlocks * 256);
    std::array<std::vector<std::uint8_t>, 2> input{std::vector<std::uint8_t>(2048), std::vector<std::uint8_t>(2048)};
};

// Authored silent mono frame at bit 32 of a one-packet buffer: 15-bit length 31, packet metadata 1.
void author(std::vector<std::uint8_t>& packet) {
    const std::uint8_t silent[]{0, 0x3F, 0, 0};
    packet[0] = 4; packet[2] = 1;
    for (unsigned i = 0; i < 31; ++i) {
        const unsigned bit = (silent[i / 8] >> (7 - i % 8)) & 1U, at = 32 + i;
        packet[at / 8] = std::uint8_t((packet[at / 8] & ~(1U << (7 - at % 8))) | (bit << (7 - at % 8)));
    }
}

struct Harness {
    std::vector<std::uint8_t> storage = std::vector<std::uint8_t>(GuestXmaBackend::PoolBytes);
    std::array<Stream, 6> streams;
    std::mutex registry; // Stand-in for the registry mutex R.
    std::atomic<unsigned long long> mapCalls{0};
    NativeXmaContextPool pool{PoolVirtual, PoolPhysical, storage.data(), [this](std::uint32_t physical, std::uint32_t bytes, bool writable) {
        return map(physical, bytes, writable);
    }};
    Harness() {
        for (unsigned s = 0; s < streams.size(); ++s) {
            streams[s].outputPhysical = 0x2000000 + s * 0x100000;
            for (unsigned b = 0; b < 2; ++b) {
                streams[s].inputPhysical[b] = 0x3000000 + s * 0x100000 + b * 0x10000;
                author(streams[s].input[b]);
            }
        }
    }
    std::uint8_t* map(std::uint32_t physical, std::uint32_t bytes, bool writable) {
        if (nativeXmaLocksHeldForTest() != 0) failTest("map_ called with an XMA lock held", __LINE__);
        std::lock_guard lock(registry); // registry.mapNativeAudioPhysical takes R
        ++mapCalls;
        for (auto& s : streams) {
            if (writable && physical == s.outputPhysical && bytes <= s.output.size()) return s.output.data();
            for (unsigned b = 0; b < 2; ++b)
                if (!writable && physical == s.inputPhysical[b] && bytes <= s.input[b].size()) return s.input[b].data();
        }
        return nullptr;
    }
    std::uint8_t* context(std::uint32_t address) { return storage.data() + (address - PoolVirtual); }
    bool access(std::uint32_t address, bool write, std::uint32_t value = 0) {
        std::uint32_t v = write ? __builtin_bswap32(value) : 0;
        return pool.access(address, write, v);
    }
    std::uint32_t read(std::uint32_t address) {
        std::uint32_t v = 0;
        CHECK(pool.access(address, false, v));
        return __builtin_bswap32(v);
    }
    bool command(std::uint32_t base, std::uint32_t address) {
        const auto index = (address - PoolVirtual) / 64;
        return access(base + (index / 32) * 4, true, 1U << (index % 32));
    }
    std::uint32_t allocate() { std::lock_guard lock(registry); return pool.allocate(); }
    bool release(std::uint32_t address) { std::lock_guard lock(registry); return pool.release(address); }
    // Guest configuration of a locked/fresh context for stream s (tag in guest-owned word 9 bits).
    void configure(std::uint32_t address, const Stream& s, std::uint32_t tag) {
        auto* c = context(address);
        store(c + 0, (CapacityBlocks << 22) | (3U << 20) | 1U);
        store(c + 4, 0x80000000U | 1U);
        store(c + 8, 0); store(c + 12, 0); store(c + 16, 0);
        store(c + 20, s.inputPhysical[0]); store(c + 24, s.inputPhysical[1]); store(c + 28, s.outputPhysical);
        store(c + 36, (tag & 0xFFFF) << 8);
    }
};

// The guest side of one stream: kick, poll, drain what was decoded, refill consumed buffers.
struct Driver {
    Harness& h; Stream& s; std::uint32_t address = 0; std::uint32_t tag = 1;
    unsigned long long consumed = 0;
    void start(std::uint32_t context) { address = context; h.configure(address, s, tag); CHECK(h.command(0x7FEA1940, address)); }
    void drain() {
        auto* c = h.context(address);
        // The tag is guest-owned: decoder publication must never have restored an older word 9.
        CHECK(((load(c + 36) >> 8) & 0xFFFF) == (tag & 0xFFFF));
        const auto write = load(c) >> 27 & 31, readOffset = load(c + 36) & 31;
        const bool valid = load(c + 4) >> 31;
        const auto blocks = write == readOffset ? (valid ? 0 : CapacityBlocks) : (write + CapacityBlocks - readOffset) % CapacityBlocks;
        consumed += blocks * 256ULL;
        ++tag;
        update(c + 36, [&](std::uint32_t v) { return (v & ~((0xFFFFU << 8) | 31U)) | ((tag & 0xFFFF) << 8) | write; });
        setBits(c + 4, 0x80000000U);
        for (unsigned b = 0; b < 2; ++b) if (!(load(c) >> (20 + b) & 1)) setBits(c, 1U << (20 + b));
    }
    void step(unsigned polls) {
        CHECK(h.command(0x7FEA1940, address));
        for (unsigned p = 0; p < polls; ++p) (void)h.read(0x7FEA1818);
        drain();
    }
};
}

int main() {
    Harness h;
    CHECK(h.pool.ready());
    std::array<std::uint32_t, 4> initial{};
    for (auto& c : initial) { c = h.allocate(); CHECK(c); }
    constexpr unsigned long long Target = 256 * FrameBytes;
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        for (unsigned t = 0; t < 1200 && !done.load(); ++t) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!done.load()) failTest("no progress within 120 s (deadlock or lost context update)", __LINE__);
    });
    std::vector<std::thread> threads;
    std::atomic<unsigned long long> pausedKicks{0}, clears{0}, reallocations{0}, registryCycles{0};
    // Two streaming "audio" threads on their own contexts.
    for (unsigned k = 0; k < 2; ++k) threads.emplace_back([&, k] {
        Driver d{h, h.streams[k]};
        d.start(initial[k]);
        while (d.consumed < Target) d.step(3);
    });
    // A third stream that is paused/resumed, locked and cleared by its own thread.
    threads.emplace_back([&] {
        Driver d{h, h.streams[2]};
        d.start(initial[2]);
        for (unsigned round = 0; d.consumed < Target; ++round) {
            if (round % 7 == 0) {
                CHECK(h.access(0x7FEA1804, true, 2));
                CHECK(h.command(0x7FEA1940, d.address)); // Deferred while paused.
                ++pausedKicks;
                const auto control = h.read(0x7FEA1804);
                CHECK(control == 2 || control == 3);
                CHECK(h.access(0x7FEA1804, true, 3));   // Resume decodes pending contexts.
            }
            if (round % 11 == 5) {
                CHECK(h.command(0x7FEA1A40, d.address)); // Lock waits for an in-flight decode.
                CHECK(h.command(0x7FEA1940, d.address));
            }
            if (round % 13 == 9) {
                CHECK(h.command(0x7FEA1A80, d.address)); // Clear: offsets and valid bits reset.
                ++clears;
                h.configure(d.address, d.s, d.tag);
                CHECK(h.command(0x7FEA1940, d.address));
                continue;
            }
            d.step(2);
        }
    });
    // A stream whose context is released and reallocated while the other threads pump it.
    threads.emplace_back([&] {
        Driver d{h, h.streams[3]};
        d.start(initial[3]);
        for (unsigned round = 0; d.consumed < Target; ++round) {
            d.step(1);
            if (round % 5 == 4) {
                CHECK(h.release(d.address));
                const auto next = h.allocate();
                CHECK(next);
                ++reallocations;
                d.start(next);
            }
        }
    });
    // Registry-side allocate/release under R, plus read-only registers, while decodes run.
    threads.emplace_back([&] {
        while (!done.load()) {
            const auto c = h.allocate();
            CHECK(c);
            CHECK(h.read(0x7FEA1800) == PoolPhysical);
            (void)h.read(0x7FEA1804);
            {
                // Both releases under R: otherwise another thread may legitimately reallocate the
                // slot in between, and the second release would free that thread's context.
                std::lock_guard lock(h.registry);
                CHECK(h.pool.release(c));
                CHECK(!h.pool.release(c));
            }
            ++registryCycles;
        }
    });
    for (std::size_t t = 0; t + 1 < threads.size(); ++t) threads[t].join();
    done = true;
    threads.back().join();
    watchdog.join();
    CHECK(h.pool.failure().empty());
    CHECK(h.pool.decodedFrames() >= 4 * (Target / FrameBytes));
    CHECK(h.pool.writtenPcmBytes() % 256 == 0 && h.pool.writtenPcmBytes() <= h.pool.decodedFrames() * FrameBytes);
    CHECK(h.mapCalls.load() > 0);
    for (unsigned i = 0; i < GuestXmaBackend::ContextCount; ++i) (void)h.release(PoolVirtual + i * 64);
    for (auto b : h.storage) CHECK(b == 0);
    std::printf("NATIVE_XMA_CONCURRENCY_PASS threads=5 frames=%llu pcm_bytes=%llu map_calls=%llu paused_kicks=%llu clears=%llu "
                "reallocations=%llu registry_cycles=%llu codec=host-stand-in\n",
        static_cast<unsigned long long>(h.pool.decodedFrames()), static_cast<unsigned long long>(h.pool.writtenPcmBytes()),
        h.mapCalls.load(), pausedKicks.load(), clears.load(), reallocations.load(), registryCycles.load());
}
