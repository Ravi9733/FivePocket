// Memory-model tests for the guest scalar paths (guest_scalar_memory.h, trace_prelude.h).
// Build one binary per model and run each. Useful on any weakly ordered host, e.g. an Arm Mac:
//   clang++ -std=c++20 -O2 -pthread [-DRECOMP_GUEST_MEMORY_ATOMIC=1 | -DRECOMP_GUEST_MEMORY_STRONG=1]
//     [-DRECOMP_DCBZ_128=1] [-fsanitize=thread -DRECOMP_TEST_ATOMIC_RESERVED_LOAD=1]
//     -Iandroid/app/src/main/cpp -Itools/host_guest_trace
//     android/app/src/main/cpp/guest_execution_guard.cpp android/tests/guest_memory_model_test.cpp
//   ./a.out [--bench]
// Hard checks: round trips, eieio and dcbz hooks, and lwarx/stwcx. counters in every model;
// no tearing and spin termination in models 1 and 2; message-passing and store-buffering
// litmus results in model 2 (strong). The other results are positive controls. A reordering
// that is allowed but not observed proves nothing, so those are reported as INFO lines.
#include "guest_execution_guard.h"
#include "trace_prelude.h"

// ppc_context.h's defaults: generated code sees these when the prelude leaves them undefined.
#ifndef PPC_MM_STORE_U8
#define PPC_MM_STORE_U8(x, y) PPC_STORE_U8(x, y)
#define PPC_MM_STORE_U16(x, y) PPC_STORE_U16(x, y)
#define PPC_MM_STORE_U32(x, y) PPC_STORE_U32(x, y)
#define PPC_MM_STORE_U64(x, y) PPC_STORE_U64(x, y)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <random>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__SANITIZE_THREAD__)
#define RECOMP_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define RECOMP_TEST_TSAN 1
#endif
#endif
#ifndef RECOMP_TEST_TSAN
#define RECOMP_TEST_TSAN 0
#endif

namespace {

alignas(4096) std::uint8_t arena[1 << 20]; // guest addresses are offsets, all "Ordinary" RAM

constexpr int model = RECOMP_GUEST_MEMORY_MODEL;
constexpr bool concurrentModel = model != 0;
constexpr bool strongModel = model == 2;

constexpr bool defined_dcbz_128() {
#if defined(RECOMP_DCBZ_128) && RECOMP_DCBZ_128
    return true;
#else
    return false;
#endif
}
constexpr std::uint32_t scale = RECOMP_TEST_TSAN ? 20 : 1; // ThreadSanitizer is much slower

int failures = 0;

const char* modelName() {
    return model == 2 ? "strong" : model == 1 ? "atomic" : "default";
}

void report(bool hard, bool ok, const char* name, const char* detail) {
    if (ok) {
        std::printf("PASS %s %s\n", name, detail);
    } else if (hard) {
        std::printf("FAIL %s %s\n", name, detail);
        ++failures;
    } else {
        std::printf("INFO %s %s\n", name, detail);
    }
}

// Runs body on its own thread and waits at most limit. On timeout the thread is detached;
// the process ends with std::_Exit, so a spinning thread cannot outlive its state.
template<class Body> bool finishesWithin(std::chrono::milliseconds limit, Body body) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread worker([done, body]() mutable {
        body();
        done->store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done->load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!done->load(std::memory_order_acquire)) {
        worker.detach();
        return false;
    }
    worker.join();
    return true;
}

// The split special ranges as an updated guard maintains them when nothing is bound, so the
// inline fast path is exercised. Their header default, [0, 2^32), sends every access to the
// cold path.
void useExactEmptySpecialRanges() {
    using namespace recomp::bootstrap;
    guestScalarDataSlotLo = guestScalarDataSlotHi = 0;
    guestScalarUptimeLo = guestScalarUptimeHi = 0;
}

// The fast predicate may only admit addresses that the exact logic (classification plus
// both special ranges) treats as plain RAM, and it must admit all of those outside the
// MMIO window. Checked at every boundary, for every width.
template<class UInt> bool fastPathMatchesExactLogic() {
    using namespace recomp::bootstrap;
    constexpr std::uint32_t width = sizeof(UInt);
    const std::uint32_t slotLo = 0x82A01230, slotHi = 0x82A01300, bundle = 0x8001F000;
    guestScalarDataSlotLo = slotLo;
    guestScalarDataSlotHi = slotHi;
    guestScalarUptimeLo = bundle;
    guestScalarUptimeHi = std::uint64_t{bundle} + 24;
    auto meets = [](std::uint32_t address, std::uint64_t lo, std::uint64_t hi) {
        return address < hi && std::uint64_t{address} + width > lo;
    };
    std::vector<std::uint32_t> points = {0, 1, 0x7FC80000, 0x7FC90000, 0x7FEA0000, 0x7FEB0000, slotLo, slotHi,
            bundle, bundle + 24, 0xFFFFFFF0};
    bool ok = true;
    for (const auto point : points)
        for (std::int64_t delta = -9; delta <= 9; ++delta) {
            const auto address = static_cast<std::uint32_t>(std::int64_t{point} + delta);
            const bool plain = classifyGuestScalarAccess(address, width) == GuestScalarRegion::Ordinary &&
                    !meets(address, slotLo, slotHi) && !meets(address, bundle, std::uint64_t{bundle} + 24);
            const bool inMmioWindow = meets(address, 0x7FC80000, 0x7FEB0000);
            const bool fast = isFastGuestScalarAccess<UInt>(address);
            ok = ok && (!fast || plain) && (fast == (plain && !inMmioWindow));
        }
    guestScalarDataSlotLo = 0; // conservative "not maintained": nothing is fast
    guestScalarDataSlotHi = std::uint64_t{1} << 32;
    ok = ok && !isFastGuestScalarAccess<UInt>(0x1000) && !isFastGuestScalarAccess<UInt>(0x82000000);
    useExactEmptySpecialRanges();
    return ok && isFastGuestScalarAccess<UInt>(0x82000000) && !isFastGuestScalarAccess<UInt>(0x7FC80000);
}

void fastPathPredicate() {
    const bool ok = fastPathMatchesExactLogic<std::uint8_t>() && fastPathMatchesExactLogic<std::uint16_t>() &&
            fastPathMatchesExactLogic<std::uint32_t>() && fastPathMatchesExactLogic<std::uint64_t>();
    report(true, ok, "fast_path_predicate", "boundary equivalence with classify + split ranges, widths 1-8");
}

void roundTrips() {
    std::uint8_t* const base = arena;
    bool ok = true;
    for (std::uint32_t offset = 0; offset < 16; ++offset) {
        const std::uint32_t at = 0x100 + offset;
        std::fill(arena + 0x100, arena + 0x140, std::uint8_t{0xAA});
        PPC_STORE_U64(at, 0x0123456789ABCDEFULL);
        ok = ok && PPC_LOAD_U64(at) == 0x0123456789ABCDEFULL && arena[at] == 0x01 && arena[at + 7] == 0xEF;
        PPC_STORE_U32(at, 0x89ABCDEFU);
        ok = ok && PPC_LOAD_U32(at) == 0x89ABCDEFU && arena[at] == 0x89 && arena[at + 3] == 0xEF;
        PPC_STORE_U16(at, 0xBEEF);
        ok = ok && PPC_LOAD_U16(at) == 0xBEEF && arena[at] == 0xBE;
        PPC_STORE_U8(at, 0x5A);
        ok = ok && PPC_LOAD_U8(at) == 0x5A;
        ok = ok && arena[0x100 + 0x3F] == 0xAA && (offset == 0 || arena[0x100] == 0xAA);
    }
    report(true, ok, "round_trips", "aligned and misaligned, widths 1-8");
}

void eieioAndDcbz() {
    std::uint8_t* const base = arena;
    PPC_MM_STORE_U32(0x200, 0x11223344U);
    PPC_MM_STORE_U64(0x208, 0x5566778899AABBCCULL);
    PPC_MM_STORE_U16(0x210, 0xDDEE);
    PPC_MM_STORE_U8(0x212, 0xFF);
    report(true, PPC_LOAD_U32(0x200) == 0x11223344U && PPC_LOAD_U64(0x208) == 0x5566778899AABBCCULL &&
            PPC_LOAD_U16(0x210) == 0xDDEE && PPC_LOAD_U8(0x212) == 0xFF, "eieio_mm_store", "values stored");

    auto zeroed = [&](std::uint32_t from, std::uint32_t bytes) {
        for (std::uint32_t i = 0; i < 0x200; ++i) {
            const bool inside = i >= from && i < from + bytes;
            if (arena[0x400 + i] != (inside ? 0 : 0xAA)) return false;
        }
        return true;
    };
    constexpr std::uint32_t dcbzBytes = (defined_dcbz_128() ? 128 : 32);
    std::fill(arena + 0x400, arena + 0x600, std::uint8_t{0xAA});
    PPC_DCBZ(0x400 + 0x80 + 0x25); // an unaligned address inside the block
    const std::uint32_t dcbzFrom = 0x80 + (0x25 & ~(dcbzBytes - 1));
    report(true, zeroed(dcbzFrom, dcbzBytes), "dcbz_hook", dcbzBytes == 128 ? "128 bytes" : "32 bytes");
    std::fill(arena + 0x400, arena + 0x600, std::uint8_t{0xAA});
    PPC_DCBZ128(0x400 + 0x100 + 0x7F);
    report(true, zeroed(0x100, 128), "dcbz128_hook", "128 bytes");
}

// XenonRecomp's lwarx / addi / stwcx. / bne translation, statement for statement (the
// shape at ppc_recomp.0.cpp:8340-8353). With RECOMP_TEST_ATOMIC_RESERVED_LOAD the load
// becomes the relaxed atomic that a regenerated corpus would emit; ThreadSanitizer
// reports the generated plain load as racing with the CAS.
struct TemplateRegister {
    union {
        std::uint32_t u32;
        std::int32_t s32;
        std::uint64_t u64;
        std::int64_t s64;
    };
};

template<class UInt> UInt reservedLoad(std::uint8_t* base, std::uint32_t ea) {
#if defined(RECOMP_TEST_ATOMIC_RESERVED_LOAD) && RECOMP_TEST_ATOMIC_RESERVED_LOAD
    return __atomic_load_n(reinterpret_cast<UInt*>(base + ea), __ATOMIC_RELAXED);
#else
    return *(UInt*)(base + ea);
#endif
}

void increment32(std::uint8_t* base, std::uint32_t ea) {
    TemplateRegister reserved{}, r10{};
    bool eq = false;
    do {
        reserved.u32 = reservedLoad<std::uint32_t>(base, ea);
        r10.u64 = __builtin_bswap32(reserved.u32);
        r10.s64 = r10.s64 + 1;
        eq = __sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(base + ea), reserved.s32, __builtin_bswap32(r10.s32));
    } while (!eq);
}

void add64(std::uint8_t* base, std::uint32_t ea, std::int64_t addend) {
    TemplateRegister reserved{}, r10{}, r11{};
    bool eq = false;
    do {
        reserved.u64 = reservedLoad<std::uint64_t>(base, ea);
        r11.u64 = __builtin_bswap64(reserved.u64);
        r10.s64 = addend + r11.s64;
        eq = __sync_bool_compare_and_swap(reinterpret_cast<uint64_t*>(base + ea), reserved.s64, __builtin_bswap64(r10.s64));
    } while (!eq);
}

// Compare-exchange with the compare-failure store-back (ppc_recomp.11.cpp:36874-36896 shape).
bool compareExchange32(std::uint8_t* base, std::uint32_t ea, std::uint32_t expected, std::uint32_t desired) {
    TemplateRegister reserved{}, r6{};
    for (;;) {
        reserved.u32 = reservedLoad<std::uint32_t>(base, ea);
        r6.u64 = __builtin_bswap32(reserved.u32);
        if (r6.u32 != expected) {
            (void)__sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(base + ea), reserved.s32, __builtin_bswap32(r6.s32));
            return false;
        }
        if (__sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(base + ea), reserved.s32, __builtin_bswap32(static_cast<std::int32_t>(desired))))
            return true;
    }
}

void llscCounters() {
    std::uint8_t* const base = arena;
    constexpr std::uint32_t counter32 = 0x1000, counter64 = 0x1008, counterCas = 0x1080;
    const std::uint32_t threads = 8, iterations = 200000 / scale;
    PPC_STORE_U32(counter32, 0);
    PPC_STORE_U64(counter64, 0);
    PPC_STORE_U32(counterCas, 0);
    std::vector<std::thread> workers;
    for (std::uint32_t t = 0; t < threads; ++t) {
        workers.emplace_back([=] {
            for (std::uint32_t i = 0; i < iterations; ++i) {
                increment32(base, counter32);
                add64(base, counter64, 3);
                std::uint32_t observed;
                do {
                    observed = PPC_LOAD_U32(counterCas);
                } while (!compareExchange32(base, counterCas, observed, observed + 1));
            }
        });
    }
    for (auto& worker : workers) worker.join();
    const std::uint64_t n = std::uint64_t{threads} * iterations;
    char detail[160];
    std::snprintf(detail, sizeof detail, "threads=%u iterations=%u add32=%u add64=%llu cmpxchg32=%u expected=%llu", threads,
            iterations, PPC_LOAD_U32(counter32), static_cast<unsigned long long>(PPC_LOAD_U64(counter64)),
            PPC_LOAD_U32(counterCas), static_cast<unsigned long long>(n));
    report(true, PPC_LOAD_U32(counter32) == n && PPC_LOAD_U64(counter64) == 3 * n && PPC_LOAD_U32(counterCas) == n,
            "llsc_counters", detail);
}

void tearing() {
    std::uint8_t* const base = arena;
    constexpr std::uint32_t word64 = 0x2000, word32 = 0x2080;
    static constexpr std::uint64_t patterns64[4] = {0, ~0ULL, 0x0123456789ABCDEFULL, ~0x0123456789ABCDEFULL};
    static constexpr std::uint32_t patterns32[4] = {0, ~0U, 0x01234567U, ~0x01234567U};
    const std::uint32_t iterations = 4000000 / scale;
    PPC_STORE_U64(word64, 0);
    PPC_STORE_U32(word32, 0);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    std::thread writer([=] {
        for (std::uint32_t i = 0; !stop->load(std::memory_order_relaxed); ++i) {
            PPC_STORE_U64(word64, patterns64[i & 3]);
            PPC_STORE_U32(word32, patterns32[i & 3]);
        }
    });
    std::uint64_t torn = 0;
    for (std::uint32_t i = 0; i < iterations; ++i) {
        const auto v = PPC_LOAD_U64(word64);
        const auto w = PPC_LOAD_U32(word32);
        torn += std::find(std::begin(patterns64), std::end(patterns64), v) == std::end(patterns64);
        torn += std::find(std::begin(patterns32), std::end(patterns32), w) == std::end(patterns32);
    }
    stop->store(true, std::memory_order_relaxed);
    writer.join();
    char detail[96];
    std::snprintf(detail, sizeof detail, "reads=%u torn=%llu", 2 * iterations, static_cast<unsigned long long>(torn));
    report(concurrentModel, torn == 0, "no_tearing", detail);
}

__attribute__((noinline)) void spinUntilNonZero(std::uint8_t* base, std::uint32_t address) {
    while (PPC_LOAD_U32(address) == 0) {
    }
}

void spinTermination() {
    std::uint8_t* const base = arena;
    constexpr std::uint32_t flag = 0x3000;
    PPC_STORE_U32(flag, 0);
    std::thread writer([=] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        PPC_STORE_U32(flag, 1);
    });
    const bool finished = finishesWithin(std::chrono::milliseconds(3000), [=] { spinUntilNonZero(base, flag); });
    writer.join();
    report(concurrentModel, finished, "spin_terminates", finished ? "exited after the store" : "still spinning after 3 s");
}

// Message passing: data=i then flag=i; a reader that sees flag=f must see data>=f.
void messagePassing() {
    std::uint8_t* const base = arena;
    constexpr std::uint32_t data = 0x4000, flag = 0x4080; // separate 128-byte lines
    const std::uint32_t iterations = 20000000 / scale;
    PPC_STORE_U32(data, 0);
    PPC_STORE_U32(flag, 0);
    auto violations = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto reads = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto readerStarted = std::make_shared<std::atomic<bool>>(false);
    std::thread writer([=] {
        while (!readerStarted->load(std::memory_order_acquire)) {
        }
        for (std::uint32_t i = 1; i <= iterations; ++i) {
            PPC_STORE_U32(data, i);
            PPC_STORE_U32(flag, i);
        }
    });
    const bool finished = finishesWithin(std::chrono::milliseconds(60000), [=] {
        readerStarted->store(true, std::memory_order_release);
        std::uint64_t bad = 0, count = 0;
        for (std::uint32_t f = 0; f < iterations;) {
            f = PPC_LOAD_U32(flag);
            const std::uint32_t d = PPC_LOAD_U32(data);
            bad += d < f;
            ++count;
        }
        violations->store(bad);
        reads->store(count);
    });
    writer.join();
    char detail[128];
    std::snprintf(detail, sizeof detail, "reads=%llu forbidden(data<flag)=%llu%s",
            static_cast<unsigned long long>(reads->load()), static_cast<unsigned long long>(violations->load()),
            finished ? "" : " reader-timeout");
    report(strongModel, finished && violations->load() == 0, "mp_litmus", detail);
}

// Store buffering: x=i; r0=y || y=i; r1=x. Sequential consistency (PowerPC sync) forbids both
// loads missing the other thread's store for the same round.
template<bool UseGuestPath> void storeBuffering(const char* name, bool hard) {
    std::uint8_t* const base = arena;
    constexpr std::uint32_t x = 0x5000, y = 0x5080;
    const std::uint32_t rounds = 500000 / scale;
    PPC_STORE_U32(x, 0);
    PPC_STORE_U32(y, 0);
    struct Shared {
        std::atomic<std::uint32_t> go{0};
        std::atomic<std::uint32_t> arrived{0};
        std::atomic<std::uint32_t> r0{0}, r1{0};
    };
    auto shared = std::make_shared<Shared>();
    auto side = [=](bool first) {
        for (std::uint32_t round = 1; round <= rounds; ++round) {
            while (shared->go.load(std::memory_order_acquire) != round) {
            }
            std::uint32_t seen;
            if constexpr (UseGuestPath) {
                PPC_STORE_U32(first ? x : y, round);
                seen = PPC_LOAD_U32(first ? y : x);
            } else { // raw release store / acquire load: the "acquire/release" alternative
                __atomic_store_n(reinterpret_cast<std::uint32_t*>(base + (first ? x : y)), round, __ATOMIC_RELEASE);
                seen = __atomic_load_n(reinterpret_cast<std::uint32_t*>(base + (first ? y : x)), __ATOMIC_ACQUIRE);
            }
            (first ? shared->r0 : shared->r1).store(seen, std::memory_order_relaxed);
            shared->arrived.fetch_add(1, std::memory_order_acq_rel);
        }
    };
    std::thread t0(side, true), t1(side, false);
    std::uint64_t forbidden = 0;
    for (std::uint32_t round = 1; round <= rounds; ++round) {
        shared->go.store(round, std::memory_order_release);
        while (shared->arrived.load(std::memory_order_acquire) != 2 * round) {
        }
        // Both paths read back the value in the representation they wrote it in.
        forbidden += shared->r0.load(std::memory_order_relaxed) < round && shared->r1.load(std::memory_order_relaxed) < round;
    }
    t0.join();
    t1.join();
    char detail[96];
    std::snprintf(detail, sizeof detail, "rounds=%u forbidden(r0,r1 both stale)=%llu", rounds,
            static_cast<unsigned long long>(forbidden));
    report(hard, forbidden == 0, name, detail);
}

// ---- cost estimate (--bench): the same patterns in each model ----
using Clock = std::chrono::steady_clock;

__attribute__((noinline)) std::uint64_t benchStream(std::uint8_t* base, std::uint32_t rounds) {
    std::uint64_t sum = 0;
    for (std::uint32_t r = 0; r < rounds; ++r)
        for (std::uint32_t i = 0; i < 4096; ++i) {
            PPC_STORE_U32(0x40000 + i * 4, i + r);
            sum += PPC_LOAD_U32(0x50000 + i * 4);
        }
    return sum;
}

__attribute__((noinline)) std::uint64_t benchSpillReload(std::uint8_t* base, std::uint32_t rounds) {
    std::uint64_t sum = 0; // generated prologues/epilogues: stw rN,-x(r1) ... lwz rN,-x(r1)
    for (std::uint32_t r = 0; r < rounds; ++r)
        for (std::uint32_t i = 0; i < 4096; ++i) {
            const std::uint32_t slot = 0x60000 + (i & 15) * 8;
            PPC_STORE_U64(slot, sum + i);
            sum += PPC_LOAD_U64(slot) & 0xFF;
        }
    return sum;
}

__attribute__((noinline)) std::uint64_t benchChase(std::uint8_t* base, std::uint32_t steps) {
    std::uint32_t node = 0x80000;
    std::uint64_t sum = 0;
    for (std::uint32_t i = 0; i < steps; ++i) {
        sum += PPC_LOAD_U32(node + 4);
        node = PPC_LOAD_U32(node);
    }
    return sum + node;
}

__attribute__((noinline)) std::uint64_t benchCopy(std::uint8_t* base, std::uint32_t rounds) {
    for (std::uint32_t r = 0; r < rounds; ++r)
        for (std::uint32_t i = 0; i < 0x8000; i += 8)
            PPC_STORE_U64(0xC0000 + i, PPC_LOAD_U64(0xA0000 + i) + r);
    return PPC_LOAD_U64(0xC0000);
}

template<class Run> double bestNanosecondsPerAccess(Run run, double accesses) {
    double best = 1e30;
    for (int attempt = 0; attempt < 7; ++attempt) {
        const auto start = Clock::now();
        volatile std::uint64_t sink = run();
        (void)sink;
        const double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        best = std::min(best, ns / accesses);
    }
    return best;
}

void bench() {
    std::uint8_t* const base = arena;
    std::vector<std::uint32_t> order(2048); // 64-byte nodes in 0x80000-0x9FFFF, random cycle
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin() + 1, order.end(), std::mt19937{12345});
    for (std::size_t i = 0; i < order.size(); ++i) {
        const std::uint32_t at = 0x80000 + order[i] * 64, next = 0x80000 + order[(i + 1) % order.size()] * 64;
        PPC_STORE_U32(at, next);
        PPC_STORE_U32(at + 4, static_cast<std::uint32_t>(i));
    }
    const double stream = bestNanosecondsPerAccess([&] { return benchStream(base, 200); }, 200.0 * 4096 * 2);
    const double spill = bestNanosecondsPerAccess([&] { return benchSpillReload(base, 200); }, 200.0 * 4096 * 2);
    const double chase = bestNanosecondsPerAccess([&] { return benchChase(base, 2000000); }, 2000000.0 * 2);
    const double copy = bestNanosecondsPerAccess([&] { return benchCopy(base, 100); }, 100.0 * 4096 * 2);
    std::printf("BENCH model=%s ns_per_access stream=%.3f spill_reload=%.3f pointer_chase=%.3f copy64=%.3f\n",
            modelName(), stream, spill, chase, copy);
}

} // namespace

int main(int argc, char** argv) {
    const bool runBench = argc > 1 && std::string_view(argv[1]) == "--bench";
    std::printf("model=%s dcbz=%u tsan=%d\n", modelName(), defined_dcbz_128() ? 128u : 32u, RECOMP_TEST_TSAN);
    useExactEmptySpecialRanges();
    fastPathPredicate();
    roundTrips();
    eieioAndDcbz();
    llscCounters();
    tearing();
    spinTermination();
    messagePassing();
    storeBuffering<true>("sb_litmus", strongModel);
    storeBuffering<false>("sb_acqrel_control", false);
    if (runBench) bench();
    std::printf(failures ? "RESULT FAIL %d\n" : "RESULT PASS\n", failures);
    std::fflush(stdout);
    std::_Exit(failures ? 1 : 0); // never join a model-0 spin reader that is allowed to hang
}
