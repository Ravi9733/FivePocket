#pragma once
// topology lane (2026-10-04; scheduling only - no guest-visible state changes; no switch file: on the AYN Thor the result is
// today's masks by construction): the core ROLES the runtime places its threads on, read once at start-up from the kernel's
// own description instead of the Thor's literal cpu numbers (cpu7 = prime, cpu3-6 = mid, cpu0-2 = little), so the same build
// places its threads on other chips. Reading only (/sys/devices/system/cpu); no setting is changed; no allocation.
//
// What is read (readSysfs): online, possible; per cpu: cpu_capacity, cpufreq/cpuinfo_max_freq (the hardware maximum - NOT
// scaling_max_freq, which the policy / thermal limit moves: the Thor's cpu3-6 read 2707 of 2803 MHz there), and
// cpufreq/related_cpus (the cpufreq policy the cpu belongs to: reported in the log line, not used by the rules).
//
// The rules (resolve):
//   R0 usable cpus = online (and possible) at start-up, numbers below MaxCpus. An offline cpu is in no mask.
//   R1 order: cpu_capacity, then cpuinfo_max_freq. A source counts only when EVERY usable cpu has it (sources are never
//      mixed); with neither, the roles are not valid. Cpus equal in both form a cluster; clusters are sorted fastest first.
//   R2 prime  = ONE cpu: the highest-numbered cpu of the fastest cluster. The role is a seat for one thread at a time (the
//      render thread; main while the render thread waits), so it never holds more; the rest of a multi-cpu fastest cluster
//      (Snapdragon 8 Elite's second prime core, three of four big cores on a 4+4 chip) is ordinary fast capacity (mid).
//   R3 little = the slowest cluster, without prime. On a chip with no little cores this is simply its slowest cluster (the six
//      performance cores of a 2+6 chip; every non-prime cpu of a chip with identical cores): "little" then only means
//      "not the prime seat", never a literal cpu0-2.
//   R4 mid    = every usable cpu that is neither prime nor little; with fewer than MidMin (3: the ranking puts its three
//      busiest threads there) such cpus, mid = every usable cpu but prime.
//   R5 allButPrime = usable without prime; midPrime = mid + prime.
//   R6 not valid (online unreadable, a cpu number >= MaxCpus, neither source complete, fewer than MinCpus (4) usable cpus):
//      every role is NoPinning (every bit, cpus 0-31: the kernel reduces it to the cpus the process may use), i.e. the
//      threads are not pinned at all - never the Thor's numbers on a chip that is not the Thor.
// A role is never empty: valid roles hold at least one usable cpu each (prime 1, mid >= 3, little >= 1), NoPinning holds all.
// Thor (cpu0-2 A510 2016 MHz, cpu3-6 A715/A710 2803 MHz, cpu7 X3 3187 MHz; capacities cpu7 1024, cpu3 855): prime 0x80,
// mid 0x78, little 0x07, allButPrime 0x7f, midPrime 0xf8 - today's literals, whether or not the A715 and A710 pairs report
// different capacities (R4 takes everything between the two ends).
//
// Use: recomp_topology::roles() is a plain global, detected once while the library loads (ensureDetected(), called by
// trace_runner.cpp's start-up initialiser before any of our threads exists); reading a role is one load - no guard, no
// call, no branch. A role is its mask (the runtime's 32-bit unsigned masks) and
// the same cpus as a list (for building a cpu_set_t: cpuSet() / RECOMP_TOPOLOGY_ADD_CPUS below). formatLine() + formatSets()
// make the start-up line RENDER_DIAG_TOPOLOGY, which trace_runner.cpp's placement thread logs once: the description, the
// roles' masks, and the sets the sites' own builders make of them, read back.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <array>
#include <bit>
#include <sched.h>
#endif

namespace recomp_topology {
constexpr unsigned MaxCpus = 32;              // the runtime's masks are 32-bit (prime_swap_v245.h, dp_jobs_big.h)
constexpr unsigned MidMin = 3;                // R4: the ranking's three busiest threads
constexpr unsigned MinCpus = 4;               // R6: one prime seat + MidMin
constexpr std::uint32_t NoPinning = ~0u;      // R6
constexpr const char* SysfsCpuRoot = "/sys/devices/system/cpu";

enum Source : std::uint8_t { SourceNone = 0, SourceCapacity = 1, SourceMaxFreq = 2, SourceBoth = 3 };

// What the kernel reported (bit c = cpu c).
struct Input {
    bool haveOnline = false, havePossible = false;
    bool overflow = false;                    // a listed cpu number is >= MaxCpus
    std::uint32_t online = 0, possible = 0;
    std::uint32_t haveCapacity = 0, haveMaxFreq = 0, havePolicy = 0; // per cpu: the file was read and is a positive number / a list
    std::uint32_t capacity[MaxCpus] = {};     // cpu_capacity (the fastest cpu is 1024)
    std::uint32_t maxFreqKhz[MaxCpus] = {};   // cpufreq/cpuinfo_max_freq
    std::uint32_t policy[MaxCpus] = {};       // cpufreq/related_cpus as a mask
};

struct Cluster { std::uint32_t cpus = 0, capacity = 0, maxFreqKhz = 0; };

// A role: an affinity mask (bit c = cpu c) and the same cpus as a list, ascending (cpu[0 .. count-1]). Never empty.
struct Role {
    std::uint32_t mask = 0, count = 0;
    std::uint8_t cpu[MaxCpus] = {};
};
constexpr Role makeRole(std::uint32_t mask) noexcept {
    Role r;
    r.mask = mask;
    for (unsigned c = 0; c < MaxCpus; ++c)
        if (mask >> c & 1u) r.cpu[r.count++] = std::uint8_t(c);
    return r;
}

struct Topology {
    // The roles. NoPinning (every cpu) until resolve() has valid ones.
    Role prime = makeRole(NoPinning), mid = makeRole(NoPinning), little = makeRole(NoPinning),
         allButPrime = makeRole(NoPinning), midPrime = makeRole(NoPinning);
    // For diagnostics that classify a cpu (never used as an affinity mask): the prime cpu and the slowest cluster; 0 when
    // the roles are not valid (every cpu then counts as "other").
    std::uint32_t fastest = 0, slowest = 0;
    std::uint32_t online = 0, possible = 0, usable = 0;
    std::uint8_t valid = 0, source = SourceNone, clusterCount = 0, policyCount = 0;
    const char* why = "not-read";             // "-" when valid
    Cluster clusters[MaxCpus] = {};           // fastest first
    std::uint32_t policies[MaxCpus] = {};     // the distinct cpufreq policies of the usable cpus (ascending first cpu)

    // The renderer thread's cpu histogram bucket: 0 = slowest cluster, 2 = the prime cpu, 1 = anything else.
    unsigned bucket(int cpu) const noexcept {
        const std::uint32_t bit = 1u << (unsigned(cpu) & (MaxCpus - 1));
        return (bit & slowest) ? 0u : (bit & fastest) ? 2u : 1u;
    }
};

namespace detail {
inline unsigned count(std::uint32_t mask) noexcept { return unsigned(__builtin_popcount(mask)); }
inline std::uint32_t highestBit(std::uint32_t mask) noexcept { return mask ? 1u << (31 - __builtin_clz(mask)) : 0u; }

// A small sysfs file as text. False when it cannot be opened or read, is empty, or does not fit (a cut list is not parsed).
inline bool readText(const char* root, const char* name, char* buf, std::size_t size) noexcept {
    char path[256];
    const int length = std::snprintf(path, sizeof path, "%s/%s", root, name);
    if (length <= 0 || std::size_t(length) >= sizeof path) return false;
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    std::size_t used = 0;
    for (;;) {
        const ssize_t n = ::read(fd, buf + used, size - 1 - used);
        if (n <= 0) break;                    // end of file (or an error: what was read so far is what there is)
        used += std::size_t(n);
        if (used == size - 1) break;
    }
    ::close(fd);
    buf[used] = 0;
    return used != 0 && used != size - 1;
}

// One decimal number (trailing white space allowed). False for anything else, 0 included.
inline bool parseNumber(const char* text, std::uint32_t& value) noexcept {
    std::uint64_t v = 0;
    const char* p = text;
    if (*p < '0' || *p > '9') return false;
    for (; *p >= '0' && *p <= '9'; ++p) { v = v * 10 + std::uint64_t(*p - '0'); if (v > 0xFFFFFFFFull) return false; }
    for (; *p; ++p) if (*p != '\n' && *p != ' ' && *p != '\t' && *p != '\r') return false;
    value = std::uint32_t(v);
    return v != 0;
}

// The kernel's cpu list ("0-7", "0-2,4-7", "3 4 5 6", "0"). False when malformed or empty; overflow when a number is
// >= MaxCpus (the bits below MaxCpus are still set).
inline bool parseList(const char* text, std::uint32_t& mask, bool& overflow) noexcept {
    mask = 0;
    const char* p = text;
    bool any = false;
    for (;;) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') ++p;
        if (!*p) break;
        if (*p < '0' || *p > '9') return false;
        unsigned long first = 0;
        for (; *p >= '0' && *p <= '9'; ++p) { first = first * 10 + unsigned(*p - '0'); if (first > 100000) return false; }
        unsigned long last = first;
        if (*p == '-') {
            ++p;
            if (*p < '0' || *p > '9') return false;
            last = 0;
            for (; *p >= '0' && *p <= '9'; ++p) { last = last * 10 + unsigned(*p - '0'); if (last > 100000) return false; }
            if (last < first) return false;
        }
        for (unsigned long c = first; c <= last; ++c) {
            if (c >= MaxCpus) { overflow = true; break; }
            mask |= 1u << c;
        }
        any = true;
    }
    return any;
}
} // namespace detail

// Reads the description under root (SysfsCpuRoot on the device; a synthetic tree in the host tests).
[[gnu::noinline]] inline Input readSysfs(const char* root) noexcept {
    Input in;
    char text[512], name[64];
    if (detail::readText(root, "online", text, sizeof text)) in.haveOnline = detail::parseList(text, in.online, in.overflow);
    if (detail::readText(root, "possible", text, sizeof text)) in.havePossible = detail::parseList(text, in.possible, in.overflow);
    for (std::uint32_t m = in.online | in.possible; m; m &= m - 1) {
        const unsigned c = unsigned(__builtin_ctz(m));
        std::snprintf(name, sizeof name, "cpu%u/cpu_capacity", c);
        if (detail::readText(root, name, text, sizeof text) && detail::parseNumber(text, in.capacity[c])) in.haveCapacity |= 1u << c;
        std::snprintf(name, sizeof name, "cpu%u/cpufreq/cpuinfo_max_freq", c);
        if (detail::readText(root, name, text, sizeof text) && detail::parseNumber(text, in.maxFreqKhz[c])) in.haveMaxFreq |= 1u << c;
        std::snprintf(name, sizeof name, "cpu%u/cpufreq/related_cpus", c);
        bool ignored = false;
        if (detail::readText(root, name, text, sizeof text) && detail::parseList(text, in.policy[c], ignored)) in.havePolicy |= 1u << c;
    }
    return in;
}

// The roles of a description (pure: the host tests' subject together with readSysfs).
[[gnu::noinline]] inline Topology resolve(const Input& in) noexcept {
    Topology t;
    t.online = in.online; t.possible = in.possible;
    if (!in.haveOnline) { t.why = "online-unreadable"; return t; }
    if (in.overflow) { t.why = "cpu-number-above-31"; return t; }
    const std::uint32_t usable = in.havePossible ? in.online & in.possible : in.online; // R0
    t.usable = usable;
    const bool capacity = (in.haveCapacity & usable) == usable, maxFreq = (in.haveMaxFreq & usable) == usable; // R1
    t.source = std::uint8_t((capacity ? SourceCapacity : 0) | (maxFreq ? SourceMaxFreq : 0));
    if (!capacity && !maxFreq) { t.why = "no-capacity-no-max-freq"; return t; }
    for (std::uint32_t m = usable; m; m &= m - 1) { // clusters: equal (capacity, max frequency)
        const unsigned c = unsigned(__builtin_ctz(m));
        const std::uint32_t cap = capacity ? in.capacity[c] : 0, khz = maxFreq ? in.maxFreqKhz[c] : 0;
        unsigned k = 0;
        while (k < t.clusterCount && (t.clusters[k].capacity != cap || t.clusters[k].maxFreqKhz != khz)) ++k;
        if (k == t.clusterCount) { t.clusters[k].capacity = cap; t.clusters[k].maxFreqKhz = khz; ++t.clusterCount; }
        t.clusters[k].cpus |= 1u << c;
        if (in.havePolicy & (1u << c)) { // (reported only)
            unsigned p = 0;
            while (p < t.policyCount && t.policies[p] != in.policy[c]) ++p;
            if (p == t.policyCount) t.policies[t.policyCount++] = in.policy[c];
        }
    }
    for (unsigned i = 1; i < t.clusterCount; ++i) // fastest first (insertion sort: at most MaxCpus entries)
        for (unsigned j = i; j > 0; --j) {
            const Cluster &a = t.clusters[j - 1], &b = t.clusters[j];
            if (a.capacity > b.capacity || (a.capacity == b.capacity && a.maxFreqKhz > b.maxFreqKhz)) break;
            const Cluster swap = t.clusters[j - 1]; t.clusters[j - 1] = t.clusters[j]; t.clusters[j] = swap;
        }
    if (detail::count(usable) < MinCpus) { t.why = "fewer-than-4-cpus"; return t; } // R6
    const std::uint32_t prime = detail::highestBit(t.clusters[0].cpus);             // R2
    const std::uint32_t rest = usable & ~prime;
    const std::uint32_t little = t.clusters[t.clusterCount - 1].cpus & ~prime;      // R3
    std::uint32_t mid = rest & ~little;                                             // R4
    if (detail::count(mid) < MidMin) mid = rest;
    t.prime = makeRole(prime); t.mid = makeRole(mid); t.little = makeRole(little);
    t.allButPrime = makeRole(rest); t.midPrime = makeRole(mid | prime);                 // R5
    t.fastest = prime; t.slowest = little;
    t.valid = 1; t.why = "-";
    return t;
}

[[gnu::noinline]] inline Topology detect(const char* root) noexcept { return resolve(readSysfs(root)); }

// The process's roles: a plain global, constant-initialised to NoPinning. ensureDetected() fills it once; the library's
// start-up initialiser calls it while the library loads (trace_runner.cpp topologySwapMasks(): single-threaded, before any
// of our threads exists), so nothing depends on the order of static initialisers and no other translation unit gains an
// initialiser of its own. Everything else just reads roles(). A program that never calls it keeps NoPinning (why=not-read).
namespace detail {
inline constinit Topology instance;
inline constinit bool detected = false;
}
[[gnu::noinline]] inline void ensureDetected() noexcept {
    if (detail::detected) return;
    detail::instance = detect(SysfsCpuRoot);
    detail::detected = true;
}
inline const Topology& roles() noexcept { return detail::instance; }

// The Thor's literal masks, for the start-up line only (thor_masks=1: this chip resolves to exactly the masks every
// build up to v458 hard-coded).
constexpr std::uint32_t ThorPrime = 0x80u, ThorMid = 0x78u, ThorLittle = 0x07u, ThorAllButPrime = 0x7Fu, ThorMidPrime = 0xF8u;
inline bool thorMasks(const Topology& t) noexcept {
    return t.prime.mask == ThorPrime && t.mid.mask == ThorMid && t.little.mask == ThorLittle &&
           t.allButPrime.mask == ThorAllButPrime && t.midPrime.mask == ThorMidPrime;
}

// The start-up line (masks in hex; clusters fastest first as cpus:capacity:max kHz, 0 = that source is not used).
[[gnu::noinline]] inline int formatLine(const Topology& t, char* out, std::size_t size) noexcept {
    static const char* const sources[4] = {"none", "capacity", "max-freq", "capacity+max-freq"};
    if (!size) return 0;
    std::size_t used = 0; // (never past size - 1: a line that does not fit is cut, not overrun)
    const auto took = [&](int n) noexcept { if (n > 0) used = used + std::size_t(n) < size ? used + std::size_t(n) : size - 1; };
    took(std::snprintf(out, size, "RENDER_DIAG_TOPOLOGY valid=%u why=%s src=%s cpus=%u online=%x possible=%x clusters=%u",
                       unsigned(t.valid), t.why, sources[t.source & 3], detail::count(t.usable), t.online, t.possible,
                       unsigned(t.clusterCount)));
    for (unsigned k = 0; k < t.clusterCount; ++k)
        took(std::snprintf(out + used, size - used, " c%u=%x:%u:%u", k, t.clusters[k].cpus, t.clusters[k].capacity,
                           t.clusters[k].maxFreqKhz));
    took(std::snprintf(out + used, size - used, " policies=%u", unsigned(t.policyCount)));
    for (unsigned k = 0; k < t.policyCount; ++k) took(std::snprintf(out + used, size - used, "%c%x", k ? ',' : ':', t.policies[k]));
    took(std::snprintf(out + used, size - used, " prime=%x mid=%x little=%x all_but_prime=%x mid_prime=%x thor_masks=%u",
                       t.prime.mask, t.mid.mask, t.little.mask, t.allButPrime.mask, t.midPrime.mask, unsigned(thorMasks(t))));
    return int(used);
}

// The second half of the start-up line: the SETS the placement sites hand the kernel. thor_masks above is computed from the
// roles' masks; the sites do not pass those masks, they build a cpu_set_t (cpuSet() / RECOMP_TOPOLOGY_ADD_CPUS below). So the
// caller (trace_runner.cpp logTopology) builds each role's set with the sites' own builder and reads it back: bits = the
// set's cpus 0-31 as a mask, count = EVERY bit of the set (a cpu above 31 would show here). Order: prime, mid, little,
// allButPrime, midPrime. sets_ok=1: each set is exactly its role's mask - nothing lost, nothing added.
struct SetReadback { std::uint32_t bits[5] = {}, count[5] = {}; };
inline bool setsOk(const Topology& t, const SetReadback& r) noexcept {
    const std::uint32_t want[5] = {t.prime.mask, t.mid.mask, t.little.mask, t.allButPrime.mask, t.midPrime.mask};
    for (unsigned i = 0; i < 5; ++i)
        if (r.bits[i] != want[i] || r.count[i] != detail::count(want[i])) return false;
    return true;
}
[[gnu::noinline]] inline int formatSets(const Topology& t, const SetReadback& r, char* out, std::size_t size) noexcept {
    if (!size) return 0;
    const int n = std::snprintf(out, size, " sets=%x,%x,%x,%x,%x set_bits=%u,%u,%u,%u,%u sets_ok=%u", r.bits[0], r.bits[1], r.bits[2],
                                r.bits[3], r.bits[4], r.count[0], r.count[1], r.count[2], r.count[3], r.count[4], unsigned(setsOk(t, r)));
    return n <= 0 ? 0 : std::size_t(n) < size ? n : int(size - 1); // (cut, not overrun)
}

#if defined(__linux__)
// A role as the kernel's affinity set. Two forms, only because a site must keep the control flow its literal had (the
// build's IR-PGO profile is matched by a function's blocks, their order and their branches):
//   cpuSet(mask)   straight-line; stands where the literal was one CPU_SET of one cpu (`CPU_SET(7,&set)`).
//   RECOMP_TOPOLOGY_ADD_CPUS(set, role)   one CPU_SET per cpu of the role; stands where the literal was a counted loop over
//       a cpu range (`for(int c=3;c<=6;++c)CPU_SET(c,&set)`). A macro, not a function: the same loop statement at the same
//       place gives the same blocks in the same order, an inlined function's blocks land elsewhere.
#if defined(__ANDROID__)
// (compile-time proof for the device target: each of cpus 0-31 is that bit of the set's first word - CPU_SET's and the
// kernel's layout, which cpuSet() relies on)
constexpr bool cpuSetLayout() noexcept {
    for (unsigned c = 0; c < MaxCpus; ++c) {
        cpu_set_t set{};
        CPU_SET(c, &set);
        if (std::bit_cast<std::array<unsigned long, sizeof(cpu_set_t) / sizeof(unsigned long)>>(set)[0] != 1ul << c) return false;
    }
    return true;
}
static_assert(cpuSetLayout(), "cpuSet(): cpus 0-31 are not the low bits of cpu_set_t's first word");
#endif
inline cpu_set_t cpuSet(std::uint32_t mask) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    const unsigned long word = mask; // cpus 0-31 are the low bits of the set's first word
    std::memcpy(&set, &word, sizeof word);
    return set;
}
#define RECOMP_TOPOLOGY_ADD_CPUS(set, role) \
    for (unsigned recompTopologyCpu = 0; recompTopologyCpu < (role).count; ++recompTopologyCpu) CPU_SET((role).cpu[recompTopologyCpu], &(set))
// Every bit of a set - the whole object the kernel is handed, cpus above 31 included (the start-up line's set_bits=).
inline std::uint32_t setBitCount(const cpu_set_t& set) noexcept {
    unsigned long words[sizeof(cpu_set_t) / sizeof(unsigned long)];
    std::memcpy(words, &set, sizeof words);
    std::uint32_t n = 0;
    for (const unsigned long w : words) n += std::uint32_t(__builtin_popcountl(w));
    return n;
}
#endif
} // namespace recomp_topology
