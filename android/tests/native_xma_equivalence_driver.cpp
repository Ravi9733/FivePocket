// Cooperative-equivalence driver for the XMA pool: one thread, a seeded random mix of context
// configurations and register operations (kicks, locks, clears, pause/resume, the 7FEA1818 pump,
// allocate/release, guest drains and refills, invalid accesses), printing after every step the
// access result, failure(), failureContext(), counters, a digest of pool storage and every guest
// buffer, and the captured diagnostic log lines. Build it once against the previous pool and once
// against the multicore pool (cooperative build, no RECOMP_GUEST_THREADS) and diff the output:
// host_support/run_wp4_host_tests.sh does this. Uses the host codec stand-in.
#include "native_xma_context_pool.h"
#include "host_log_capture.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace recomp::audio { void diagnosticDumpXma(); }
using namespace recomp::audio;
using recomp::bootstrap::GuestXmaBackend;

namespace {
std::uint64_t rng = 0x9E3779B97F4A7C15ULL;
std::uint32_t next(std::uint32_t bound) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return static_cast<std::uint32_t>(rng % bound);
}
void be(std::uint8_t* p, std::uint32_t v) { p[0] = std::uint8_t(v >> 24); p[1] = std::uint8_t(v >> 16); p[2] = std::uint8_t(v >> 8); p[3] = std::uint8_t(v); }
std::uint32_t word(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }
std::uint64_t hash(const std::vector<std::uint8_t>& bytes, std::uint64_t h = 1469598103934665603ULL) {
    for (auto b : bytes) { h ^= b; h *= 1099511628211ULL; }
    return h;
}
void putBits(std::vector<std::uint8_t>& dest, unsigned at, std::uint32_t value, unsigned count) {
    for (unsigned i = 0; i < count && at + i < dest.size() * 8; ++i) {
        const unsigned bit = (value >> (count - 1 - i)) & 1U, pos = at + i;
        dest[pos / 8] = std::uint8_t((dest[pos / 8] & ~(1U << (7 - pos % 8))) | (bit << (7 - pos % 8)));
    }
}
// Packets with a random first-frame offset, metadata (mostly valid), skip count and frame chain.
void authorPackets(std::vector<std::uint8_t>& buffer) {
    std::fill(buffer.begin(), buffer.end(), 0);
    for (unsigned packet = 0; packet < buffer.size() / 2048; ++packet) {
        auto* p = &buffer[packet * 2048];
        const unsigned firstOffset = next(4) ? next(64) : 0x3FFF >> next(3);
        const unsigned metadata = next(10) ? 1 : next(8);
        p[0] = std::uint8_t((next(4) << 2) | ((firstOffset >> 13) & 3));
        p[1] = std::uint8_t(firstOffset >> 5);
        p[2] = std::uint8_t(((firstOffset & 31) << 3) | metadata);
        p[3] = std::uint8_t(next(3));
        unsigned at = packet * 2048 * 8 + 32 + firstOffset;
        for (unsigned frame = 0; frame < 6 && at < (packet + 1) * 2048 * 8; ++frame) {
            const unsigned length = next(12) ? 31 + next(900) : next(40);
            putBits(buffer, at, length, 15);
            putBits(buffer, at + length - 1, next(2), 1); // "follows" bit
            at += std::max(length, 16U);
        }
    }
}
struct Buffers {
    std::array<std::vector<std::uint8_t>, 4> output, input0, input1;
};
}

int main(int argc, char** argv) {
    const unsigned steps = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 4000;
    if (argc > 2) rng = std::strtoull(argv[2], nullptr, 0);
    constexpr std::uint32_t V = 0xA1000000, P = 0x1000000;
    std::vector<std::uint8_t> storage(GuestXmaBackend::PoolBytes, 0xAB);
    Buffers b;
    for (unsigned s = 0; s < 4; ++s) {
        b.output[s].assign(12 * 256, 0xCD);
        b.input0[s].assign(2048 * (1 + s % 2), 0);
        b.input1[s].assign(2048 * 2, 0);
    }
    NativeXmaContextPool pool(V, P, storage.data(), [&](std::uint32_t a, std::uint32_t n, bool w) -> std::uint8_t* {
        for (unsigned s = 0; s < 4; ++s) {
            if (w && a == 0x2000000 + s * 0x10000 && n <= b.output[s].size()) return b.output[s].data();
            if (!w && a == 0x3000000 + s * 0x10000 && n <= b.input0[s].size()) return b.input0[s].data();
            if (!w && a == 0x4000000 + s * 0x10000 && n <= b.input1[s].size()) return b.input1[s].data();
        }
        return nullptr;
    });
    std::array<std::uint32_t, 4> contexts{};
    for (auto& c : contexts) c = pool.allocate();
    const auto configure = [&](unsigned s) {
        if (!contexts[s]) return;
        auto* c = storage.data() + (contexts[s] - V);
        const unsigned capacity = 4 + next(9);
        const unsigned loops = next(5) ? 0 : (next(2) ? 255 : 1 + next(3));
        const unsigned channels = next(3) ? 1 : 2;
        const unsigned packets0 = b.input0[s].size() / 2048, packets1 = next(2) + 1;
        be(c + 0, (next(capacity) << 27) | (capacity << 22) | (next(4) << 20) | (loops << 12) | (next(8) ? packets0 : 0));
        be(c + 4, (next(6) ? 0x80000000U : 0) | ((channels - 1) << 29) | (next(4) << 27) | (next(4) << 24) |
                  (next(3) << 20) | (next(5) << 17) | (next(4) << 12) | packets1);
        be(c + 8, next(4) ? 0 : next(2048 * 8));
        const unsigned loopStart = 32 + next(2048 * 8 - 32), loopEnd = loopStart + next(4096);
        be(c + 12, loopStart); be(c + 16, loopEnd | (next(2) << 31));
        be(c + 20, next(12) ? 0x3000000 + s * 0x10000 : 0xDEAD0000);
        be(c + 24, next(12) ? 0x4000000 + s * 0x10000 : 0xBEEF0000);
        be(c + 28, next(12) ? 0x2000000 + s * 0x10000 : 0xCAFE0000);
        be(c + 36, next(capacity) | (next(30) ? 0 : 1U << 30));
        authorPackets(b.input0[s]);
        authorPackets(b.input1[s]);
    };
    for (unsigned s = 0; s < 4; ++s) configure(s);
    const auto access = [&](std::uint32_t address, bool write, std::uint32_t value) {
        std::uint32_t v = write ? __builtin_bswap32(value) : 0;
        const bool ok = pool.access(address, write, v);
        return std::pair<bool, std::uint32_t>(ok, __builtin_bswap32(v));
    };
    for (unsigned step = 0; step < steps; ++step) {
        const unsigned s = next(4);
        std::string op;
        std::pair<bool, std::uint32_t> result{true, 0};
        const auto mask = [&] { const auto i = (contexts[s] - V) / 64; return std::pair(i / 32 * 4, 1U << (i % 32)); };
        switch (next(20)) {
        case 0: case 1: case 2: case 3:
            if (contexts[s]) { auto [o, m] = mask(); op = "kick"; result = access(0x7FEA1940 + o, true, m); }
            break;
        case 4: case 5: case 6: op = "poll"; result = access(0x7FEA1818, false, 0); break;
        case 7: if (contexts[s]) { auto [o, m] = mask(); op = "lock"; result = access(0x7FEA1A40 + o, true, m); } break;
        case 8: if (contexts[s]) { auto [o, m] = mask(); op = "clear"; result = access(0x7FEA1A80 + o, true, m); } break;
        case 9: op = "control"; result = access(0x7FEA1804, true, next(5) ? 2 + next(2) : next(8)); break;
        case 10: op = "read1804"; result = access(0x7FEA1804, false, 0); break;
        case 11: op = "configure"; configure(s); break;
        case 12: { // Guest drain: read offset to the write offset, output valid again.
            if (!contexts[s]) break;
            auto* c = storage.data() + (contexts[s] - V);
            be(c + 36, (word(c + 36) & ~31U) | ((word(c) >> 27) & 31));
            be(c + 4, word(c + 4) | 0x80000000U);
            op = "drain";
            break;
        }
        case 13: { // Guest refill: both input buffers valid again.
            if (!contexts[s]) break;
            auto* c = storage.data() + (contexts[s] - V);
            be(c, word(c) | (3U << 20));
            op = "refill";
            break;
        }
        case 14: op = "release"; result.first = contexts[s] && pool.release(contexts[s]); contexts[s] = 0; break;
        case 15: op = "allocate"; if (!contexts[s]) { contexts[s] = pool.allocate(); configure(s); } result.second = contexts[s]; break;
        case 16: op = "misaligned"; result = access(0x7FEA1802, false, 0); break;
        case 17: op = "unknown"; result = access(next(2) ? 0x7FEA1810 : 0x7FEA1C00, next(2), 1); break;
        case 18: op = "unallocated"; result = access(0x7FEA1940 + 36, true, 0x80000000U); break;
        default: op = "read1800"; result = access(0x7FEA1800, false, 0); break;
        }
        if (op.empty()) continue;
        std::uint64_t digest = hash(storage);
        for (unsigned k = 0; k < 4; ++k) digest = hash(b.output[k], digest);
        std::uint64_t failureDigest = 1469598103934665603ULL;
        for (auto w : pool.failureContext()) { failureDigest ^= w; failureDigest *= 1099511628211ULL; }
        std::printf("%u s%u %s ok=%u value=%08x frames=%llu pcm=%llu failure=\"%s\" failure_context=%016llx state=%016llx\n",
            step, s, op.c_str(), unsigned(result.first), result.second,
            static_cast<unsigned long long>(pool.decodedFrames()), static_cast<unsigned long long>(pool.writtenPcmBytes()),
            pool.failure().c_str(), static_cast<unsigned long long>(failureDigest), static_cast<unsigned long long>(digest));
        bool pumpFailed = false;
        for (const auto& line : recomp::testing::hostLogLines()) {
            std::printf("  log %s\n", line.text.c_str());
            pumpFailed |= line.text.find("PUMP_FAILED") != std::string::npos;
        }
        recomp::testing::clearHostLogLines();
        // A failed access stops a real run; the guest never continues a context past a decode
        // failure. Clear every context so both builds continue from defined decoder state (the
        // previous pool overflows its frame buffer when a rejected zero length is kicked again).
        if ((!result.first && (op == "kick" || op == "control")) || pumpFailed) {
            for (unsigned k = 0; k < 4; ++k) if (contexts[k]) {
                const auto i = (contexts[k] - V) / 64;
                (void)access(0x7FEA1A80 + i / 32 * 4, true, 1U << (i % 32));
            }
            recomp::testing::clearHostLogLines();
            std::printf("  recover\n");
        }
    }
    recomp::audio::diagnosticDumpXma();
    for (const auto& line : recomp::testing::hostLogLines()) {
        const auto age = line.text.find(" age_ms=");
        std::printf("  dump %s\n", line.text.substr(0, age).c_str()); // Wall-clock ages differ run to run.
    }
    std::printf("NATIVE_XMA_EQUIVALENCE_DONE steps=%u frames=%llu pcm=%llu\n", steps,
        static_cast<unsigned long long>(pool.decodedFrames()), static_cast<unsigned long long>(pool.writtenPcmBytes()));
}
