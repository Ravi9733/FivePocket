// boot-stall (lane boot-stall, 2026-10-06): the device's VkPipelineCache kept between runs.
//
// Why: every start compiled every pipeline again (the cache started empty and vkGetPipelineCacheData was never called), and the
// first scene's ~500 creations made one present of 13-18 s right after "in control" (REPORT.md of the lane). A pipeline cache
// only changes how long vkCreate*Pipelines takes; the driver decides by its own keys what an entry is good for.
//
// The file: <dir>/pipeline-cache.bin = our header (64 bytes, little-endian) + the payload vkGetPipelineCacheData returned.
//   0  magic "RCPCACHE"        8  format version (1)      12  header bytes (64)
//  16  payload bytes (u64)    24  checksum64(payload)     32  identity (32 bytes: the library's GNU build-id, zero padded)
// LOAD (once, at device creation, before vkCreatePipelineCache): the file is used only if every check holds - a regular file
// (never through a symbolic link), not empty, not larger than the cap, our magic / version / header size, the payload length
// equal to the file's, the identity equal to this library's, Vulkan's own header inside the payload (headerSize 32, version
// ONE, vendorID, deviceID and pipelineCacheUUID equal to the physical device's) and the payload's checksum. Anything else is
// IGNORED: the cache starts empty, exactly as before this lane. The load never writes, renames or deletes anything. What it
// hands on is exactly the bytes it read and checked (repair 1: a file rewritten in place while it is read can be shorter than
// fstat said; the room fstat asked for is not the file).
// SAVE (the saver thread only; when: decide() below): payload -> <dir>/pipeline-cache.bin.tmp (locked with flock, truncated,
// written, fsync) -> rename over pipeline-cache.bin -> fsync of the directory. A payload the load would refuse is never
// written. pipeline-cache.bin is what it was after every failure. The temporary file is removed after a failed truncate,
// write, fsync or rename. It STAYS (the next save takes it over) when the save stood back for another writer (that writer's
// file), when the descriptor could not be asked about itself (fstat), or when the removal itself failed. Above the cap
// nothing is written (the old file stays). A file the load refused is simply replaced by the next save (the save never reads
// the old file). A temporary name that is a second name of another file (a hard link put there by hand) is taken away, not
// written through (repair 1).
// GUARD (repair 1): <dir>/pipeline-cache.loading is made just before the file's data is handed to vkCreatePipelineCache and
// removed by the saver thread once that start has lived with the data (the renderer created 100 pipelines through the cache,
// or a save attempt was reached), or when a new file is in place. A start that finds it does not hand the file to the driver
// (reason last-start-unfinished): see guardSet() below.
// SWITCH: <dir>/diag-pipeline-cache-off, read once at device creation: no load, no guard, no saver thread, no save.
// LOG (tag RecompRenderer, as GLOG): RENDER_DIAG_PCACHE_LOAD once at start; RENDER_DIAG_PCACHE_SAVE once per save, per failed
// save, and once after the warm-up when there was nothing to write (reason=unchanged); RENDER_DIAG_PCACHE_GUARD once when
// the saver thread removed the guard.
//
// Threads: the saver thread ("recomp-pcache") is the only one that calls vkGetPipelineCacheData and the only one that writes
// the file. It holds the device's pipelineCacheMutex ONLY across the driver call (a size query; then one copy into a buffer
// that was allocated before the lock); checksum, write, fsync and rename run without any lock of the renderer. The renderer's
// side is pipeline_cache_signal.h (atomics). In the save's line hold_ms is the time the saver HELD that lock (what a pipeline
// creation can have had to wait for the save) and wait_ms the time it waited to get it (behind creations; repair 1: the first
// delivery reported the two as one number, hold_ms).
// Our own code; POSIX only. No Vulkan header is needed here: device.cpp hands in the driver call.
#pragma once
#include "pipeline_cache_signal.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__ANDROID__) || defined(__linux__)
#include <link.h> // dl_iterate_phdr: this library's GNU build-id
#endif
#if __has_include(<android/log.h>) // (the host tests' stand-in captures the lines)
#include <android/log.h>
#define RECOMP_PCACHE_ALOG 1
#endif
#ifndef RECOMP_PCACHE_MUTANT // host-test mutants: the check with this number is left out (0 = none; see tests/run_tests.sh)
#define RECOMP_PCACHE_MUTANT 0
#endif

namespace recomp::pcache {
inline constexpr char FileName[] = "pipeline-cache.bin";
inline constexpr char TempName[] = "pipeline-cache.bin.tmp";
inline constexpr char SwitchName[] = "diag-pipeline-cache-off";
inline constexpr std::uint8_t Magic[8] = {'R', 'C', 'P', 'C', 'A', 'C', 'H', 'E'};
inline constexpr std::uint32_t FormatVersion = 1;
inline constexpr std::size_t HeaderBytes = 64, VkHeaderBytes = 32, IdentityBytes = 32;
// The cap on the payload. Above it nothing is saved and a file is not loaded. 128 MiB = half of what a game run's process may
// write to one file (RLIMIT_FSIZE 256 MiB, private_trace_jni.cpp); effectiveCap() lowers it further to the process's own limit.
inline constexpr std::size_t DefaultCapBytes = std::size_t{128} << 20;

struct DeviceId { std::uint32_t vendorID = 0, deviceID = 0; std::uint8_t uuid[16] = {}; };
struct Identity { std::uint8_t bytes[IdentityBytes] = {}; };

enum class Reason : unsigned {
    Ok, SwitchOff, Missing, NotAFile, OpenFailed, Empty, Truncated, OverCap, ReadFailed, BadMagic, BadVersion, BadHeader, BadLength,
    WrongIdentity, VkHeaderShort, VkHeaderSize, VkHeaderVersion, VkVendor, VkDevice, VkUuid, BadChecksum, CreateFailed, Exception,
    Unfinished, GuardFailed, // (the guard: guardSet below)
    // save only
    Busy, WriteFailed, SyncFailed, RenameFailed, QueryFailed, GetFailed, Incomplete, Unchanged,
};
inline const char* name(Reason r) noexcept {
    switch (r) {
    case Reason::Ok: return "ok"; case Reason::SwitchOff: return "switch-off"; case Reason::Missing: return "missing";
    case Reason::NotAFile: return "not-a-file"; case Reason::OpenFailed: return "open-failed"; case Reason::Empty: return "empty";
    case Reason::Truncated: return "truncated"; case Reason::OverCap: return "over-cap"; case Reason::ReadFailed: return "read-failed";
    case Reason::BadMagic: return "magic"; case Reason::BadVersion: return "version"; case Reason::BadHeader: return "header";
    case Reason::BadLength: return "length"; case Reason::WrongIdentity: return "identity"; case Reason::VkHeaderShort: return "vk-header-short";
    case Reason::VkHeaderSize: return "vk-header-size"; case Reason::VkHeaderVersion: return "vk-header-version";
    case Reason::VkVendor: return "vk-vendor"; case Reason::VkDevice: return "vk-device"; case Reason::VkUuid: return "vk-uuid";
    case Reason::BadChecksum: return "checksum"; case Reason::CreateFailed: return "create-failed"; case Reason::Exception: return "exception";
    case Reason::Unfinished: return "last-start-unfinished"; case Reason::GuardFailed: return "guard-failed";
    case Reason::Busy: return "busy"; case Reason::WriteFailed: return "write-failed"; case Reason::SyncFailed: return "sync-failed";
    case Reason::RenameFailed: return "rename-failed"; case Reason::QueryFailed: return "query-failed"; case Reason::GetFailed: return "get-failed";
    case Reason::Incomplete: return "incomplete"; case Reason::Unchanged: return "unchanged";
    }
    return "?";
}

// ---- bytes (the file is little-endian whatever the host is)
inline std::uint32_t get32(const std::uint8_t* p) noexcept { return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24; }
inline std::uint64_t get64(const std::uint8_t* p) noexcept { return std::uint64_t(get32(p)) | std::uint64_t(get32(p + 4)) << 32; }
inline void put32(std::uint8_t* p, std::uint32_t v) noexcept { for (int i = 0; i < 4; ++i) p[i] = std::uint8_t(v >> (8 * i)); }
inline void put64(std::uint8_t* p, std::uint64_t v) noexcept { put32(p, std::uint32_t(v)); put32(p + 4, std::uint32_t(v >> 32)); }

// The payload's checksum: eight bytes at a time, each step a bijection of the state for a given word and of the word for a
// given state (xor, an odd multiplier, a xor-shift), so a change confined to one 8-byte word always changes the result; the
// xor-shift folds the high bits down so that equal flips in two words do not cancel. Not a cryptographic hash: it guards
// against a torn or decayed file in the app's own private storage, not against a writer who wants to deceive us.
inline std::uint64_t checksum64(const std::uint8_t* p, std::size_t n) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ull ^ (std::uint64_t(n) * 0x9E3779B97F4A7C15ull);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        h = (h ^ get64(p + i)) * 0x100000001b3ull;
#if RECOMP_PCACHE_MUTANT != 20 // mutant 20: no fold (two equal top-bit flips cancel)
        h ^= h >> 29;
#endif
    }
    for (; i < n; ++i) { h = (h ^ p[i]) * 0x100000001b3ull; h ^= h >> 29; }
    return h ^ (h >> 32);
}

// Vulkan's own header at the start of a payload (VkPipelineCacheHeaderVersionOne, always least significant byte first):
// headerSize, headerVersion, vendorID, deviceID (4 x u32), pipelineCacheUUID (16 bytes).
inline Reason checkVkHeader(const std::uint8_t* payload, std::size_t n, const DeviceId& device) noexcept {
    if (n < VkHeaderBytes) return RECOMP_PCACHE_MUTANT == 31 ? Reason::Ok : Reason::VkHeaderShort;
    if (RECOMP_PCACHE_MUTANT != 7 && get32(payload) != VkHeaderBytes) return Reason::VkHeaderSize;
    if (RECOMP_PCACHE_MUTANT != 8 && get32(payload + 4) != 1u) return Reason::VkHeaderVersion; // VK_PIPELINE_CACHE_HEADER_VERSION_ONE
    if (RECOMP_PCACHE_MUTANT != 9 && get32(payload + 8) != device.vendorID) return Reason::VkVendor;
    if (RECOMP_PCACHE_MUTANT != 10 && get32(payload + 12) != device.deviceID) return Reason::VkDevice;
    if (RECOMP_PCACHE_MUTANT != 11 && std::memcmp(payload + 16, device.uuid, 16) != 0) return Reason::VkUuid;
    return Reason::Ok;
}

// A whole file image (header + payload). The cheap checks first: a file of another library or another driver is refused
// without reading its megabytes through the checksum.
inline Reason checkFile(const std::uint8_t* file, std::size_t n, const Identity& identity, const DeviceId& device, std::size_t cap) noexcept {
    if (n == 0) return RECOMP_PCACHE_MUTANT == 29 ? Reason::Ok : Reason::Empty;
    if (n < HeaderBytes) return RECOMP_PCACHE_MUTANT == 30 ? Reason::Ok : Reason::Truncated;
    if (RECOMP_PCACHE_MUTANT != 1 && std::memcmp(file, Magic, sizeof Magic) != 0) return Reason::BadMagic;
    if (RECOMP_PCACHE_MUTANT != 2 && get32(file + 8) != FormatVersion) return Reason::BadVersion;
    if (RECOMP_PCACHE_MUTANT != 28 && get32(file + 12) != HeaderBytes) return Reason::BadHeader;
    std::uint64_t payload = get64(file + 16);
    const std::uint64_t have = n - HeaderBytes;
    if (RECOMP_PCACHE_MUTANT != 3 && payload > cap) return Reason::OverCap;
#if RECOMP_PCACHE_MUTANT == 4 // mutant 4: a file shorter than its header says is taken as far as it goes
    payload = std::min(payload, have);
#endif
    if (payload > have) return Reason::Truncated;
    if (RECOMP_PCACHE_MUTANT != 5 && payload < have) return Reason::BadLength;
    if (RECOMP_PCACHE_MUTANT != 6 && std::memcmp(file + 32, identity.bytes, IdentityBytes) != 0) return Reason::WrongIdentity;
    if (const Reason vk = checkVkHeader(file + HeaderBytes, std::size_t(payload), device); vk != Reason::Ok) return vk;
    if (RECOMP_PCACHE_MUTANT != 12 && get64(file + 24) != checksum64(file + HeaderBytes, std::size_t(payload))) return Reason::BadChecksum;
    return Reason::Ok;
}

inline void makeHeader(std::uint8_t* out, const std::uint8_t* payload, std::size_t n, const Identity& identity) noexcept {
    std::memset(out, 0, HeaderBytes);
    std::memcpy(out, Magic, sizeof Magic);
    put32(out + 8, FormatVersion); put32(out + 12, std::uint32_t(HeaderBytes));
    put64(out + 16, n); put64(out + 24, checksum64(payload, n));
    std::memcpy(out + 32, identity.bytes, IdentityBytes);
}

inline bool switchOff(const std::string& dir) noexcept {
    try { return RECOMP_PCACHE_MUTANT != 19 && ::access((dir + "/" + SwitchName).c_str(), F_OK) == 0; } catch (...) { return true; }
}

// The cap this process can honour: never more than its own file-size limit lets it write (header included).
inline std::size_t effectiveCap(std::size_t cap) noexcept {
    struct rlimit lim {};
    if (::getrlimit(RLIMIT_FSIZE, &lim) != 0 || lim.rlim_cur == RLIM_INFINITY) return cap;
    if (lim.rlim_cur <= HeaderBytes) return 0;
    return std::size_t(std::min<std::uint64_t>(cap, std::uint64_t(lim.rlim_cur) - HeaderBytes));
}

// ---- identity: the GNU build-id of the library this code is linked into (what the pipelines' shader rewrites come from).
// "none": no build-id note was found (the host; a library linked without --build-id): the identity is all zero, so the
// file then survives a library change. That is harmless (the driver keys every entry by the shader code it compiled).
inline const int identityAnchor = 0;
// The GNU build-id in the bytes of one PT_NOTE segment. A note: name size, description size, type (3 x u32 in the file's own
// byte order), the name and the description, each padded to 4 bytes. False when there is none (or the bytes end early).
inline bool buildIdFromNotes(const std::uint8_t* p, std::size_t left, Identity& id) noexcept {
    while (left >= 12) {
        std::uint32_t nameSize, descSize, type;
        std::memcpy(&nameSize, p, 4); std::memcpy(&descSize, p + 4, 4); std::memcpy(&type, p + 8, 4);
        const std::size_t name = 12, desc = name + ((std::size_t(nameSize) + 3) & ~std::size_t{3}), next = desc + ((std::size_t(descSize) + 3) & ~std::size_t{3});
        if (next > left || next < 12) return false;
        if ((RECOMP_PCACHE_MUTANT == 33 || type == 3u /* NT_GNU_BUILD_ID */) && nameSize == 4 && std::memcmp(p + name, "GNU", 4) == 0 && descSize) {
            id = Identity{};
            std::memcpy(id.bytes, p + desc, std::min<std::size_t>(descSize, IdentityBytes));
            return true;
        }
#if RECOMP_PCACHE_MUTANT == 33 // mutant 33: the first note of any kind is taken for the build-id
        if (descSize) { id = Identity{}; std::memcpy(id.bytes, p + desc, std::min<std::size_t>(descSize, IdentityBytes)); return true; }
#endif
        p += next; left -= next;
    }
    return false;
}
#if defined(__ANDROID__) || defined(__linux__)
struct IdentitySearch { std::uintptr_t anchor; Identity* id; bool found; };
// dl_iterate_phdr's callback: the loaded object that holds `anchor` (this library), then its PT_NOTE segments.
inline int identityPhdr(struct dl_phdr_info* info, std::size_t, void* data) noexcept {
    auto& s = *static_cast<IdentitySearch*>(data);
    bool inside = false;
    for (int i = 0; i < int(info->dlpi_phnum); ++i) {
        const auto& ph = info->dlpi_phdr[i];
        const std::uintptr_t base = std::uintptr_t(info->dlpi_addr) + std::uintptr_t(ph.p_vaddr);
        if (ph.p_type == PT_LOAD && s.anchor >= base && s.anchor - base < std::uintptr_t(ph.p_memsz)) inside = true;
    }
    if (!inside) return 0;
    for (int i = 0; i < int(info->dlpi_phnum) && !s.found; ++i) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type == PT_NOTE)
            s.found = buildIdFromNotes(reinterpret_cast<const std::uint8_t*>(std::uintptr_t(info->dlpi_addr) + std::uintptr_t(ph.p_vaddr)), std::size_t(ph.p_memsz), *s.id);
    }
    return 1; // this library was found: stop
}
#endif
inline Identity libraryIdentity(const char** source) noexcept {
    Identity id{};
    *source = "none";
#if defined(__ANDROID__) || defined(__linux__)
    IdentitySearch s{reinterpret_cast<std::uintptr_t>(&identityAnchor), &id, false};
    (void)::dl_iterate_phdr(identityPhdr, &s);
    if (s.found) *source = "build-id";
#endif
    return id;
}

// ---- load
struct Loaded {
    Reason reason = Reason::Missing;
    int error = 0;                     // errno of a failed open / read
    long long fileBytes = -1;          // the file's size when it could be asked
    std::vector<std::uint8_t> file;    // the whole file when reason == Ok (the payload starts at HeaderBytes)
    [[nodiscard]] const std::uint8_t* payload() const noexcept { return file.data() + HeaderBytes; }
    [[nodiscard]] std::size_t payloadBytes() const noexcept { return reason == Reason::Ok ? file.size() - HeaderBytes : 0; }
};
// A descriptor that is closed on every way out of its scope, an exception included.
struct Descriptor {
    int fd = -1;
    Descriptor() = default;
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() { if (fd >= 0) (void)::close(fd); }
};
// Reads and checks <dir>/pipeline-cache.bin. Read-only: nothing is created, changed or removed, whatever is found there.
// When the reason is Ok, `file` is exactly the bytes that were read, and every one of them passed checkFile().
[[gnu::noinline]] inline Loaded load(const std::string& dir, const Identity& identity, const DeviceId& device, std::size_t cap) noexcept {
    Loaded out;
    try {
        Descriptor file;
        const std::string path = dir + "/" + FileName;
        // O_NOFOLLOW: a symbolic link is not the file. O_NONBLOCK: a FIFO put there must not hold the start.
#if RECOMP_PCACHE_MUTANT == 14 // mutant 14: a symbolic link is followed
        file.fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
#elif RECOMP_PCACHE_MUTANT == 32 // mutant 32: a FIFO in the file's place holds the start
        file.fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
#else
        file.fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
#endif
        const int fd = file.fd;
        if (fd < 0) {
            out.error = errno;
            out.reason = out.error == ENOENT ? Reason::Missing : out.error == ELOOP || out.error == EMLINK ? Reason::NotAFile : Reason::OpenFailed;
            return out;
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0) { out.error = errno; out.reason = Reason::OpenFailed; return out; }
        if (RECOMP_PCACHE_MUTANT != 13 && !S_ISREG(st.st_mode)) { out.reason = Reason::NotAFile; return out; }
        out.fileBytes = static_cast<long long>(st.st_size);
        if (RECOMP_PCACHE_MUTANT != 29 && st.st_size == 0) { out.reason = Reason::Empty; return out; }
        if (RECOMP_PCACHE_MUTANT != 3 && std::uint64_t(st.st_size) > std::uint64_t(HeaderBytes) + cap) { out.reason = Reason::OverCap; return out; }
        std::vector<std::uint8_t> bytes;
#if RECOMP_PCACHE_MUTANT == 40 // mutant 40 (the code before repair 1): the descriptor stays open when the buffer cannot be made
        try { bytes.resize(static_cast<std::size_t>(st.st_size)); } catch (...) { file.fd = -1; throw; }
#else
        bytes.resize(static_cast<std::size_t>(st.st_size)); // (the room to read into; what the file IS, is what the reads return)
#endif
        std::size_t filled = 0;
        while (filled < bytes.size()) {
            const auto got = ::read(fd, bytes.data() + filled, bytes.size() - filled);
            if (got < 0) { if (errno == EINTR) continue; out.error = errno; break; }
            if (got == 0) break;
            filled += static_cast<std::size_t>(got);
        }
        if (out.error) { out.reason = Reason::ReadFailed; return out; }
#if RECOMP_PCACHE_MUTANT == 35 // mutant 35 (the code before repair 1): the checks see the bytes read, the caller gets the room fstat asked for
        out.reason = checkFile(bytes.data(), filled, identity, device, cap);
#else
        // Repair 1. fstat's size was only the room to read into: a file rewritten in place meanwhile (cp, adb push, a restore)
        // can be shorter by now, and whole. The buffer is cut to the bytes read, the checks see that buffer, and that buffer is
        // what the caller gets: no byte is handed on that no check covered.
        bytes.resize(filled);
        out.reason = checkFile(bytes.data(), bytes.size(), identity, device, cap);
#endif
        if (out.reason == Reason::Ok) out.file = std::move(bytes);
    } catch (...) { out = Loaded{}; out.reason = Reason::Exception; }
    return out;
}

// ---- the guard: the way out of a file the driver cannot live with (repair 1)
// The file passes every check above and the driver may still crash or hang on it (its own data, a driver bug): then every
// start would, and before this guard only the off switch (adb) ended that. <dir>/pipeline-cache.loading is made just before
// the file's data is handed to vkCreatePipelineCache. The saver thread removes it when the start has lived with the data:
// the renderer has created Timing::confirm (100) pipelines through the cache since (seen at the saver's next look), or a
// save attempt was reached (the warm-up ended, the periodic look, the orderly end) - and whenever a new file is in place.
// A start that finds it there (the start before died, hung or was ended before that point) does NOT hand the file to the
// driver: the cache starts empty, as before this lane, and that start's first save replaces the file. The cache file itself
// is never changed or removed by any of this.
// The price: a game ended before the renderer has made its first 100 pipelines (during loading, before the game is in
// control) makes the next start a slow one. What it does not cover: a driver that fails on the data later than that.
inline constexpr char GuardName[] = "pipeline-cache.loading";
// Is one there? Anything at that name counts; so does a name that cannot be asked about (the cautious answer).
inline bool guardPresent(const std::string& dir) noexcept {
    try {
        struct stat st {};
        return RECOMP_PCACHE_MUTANT != 42 && !(::lstat((dir + "/" + GuardName).c_str(), &st) != 0 && errno == ENOENT);
    } catch (...) { return true; }
}
// Makes it (it must not exist). fsync: it has to outlast a phone that had to be restarted. False: it could not be made -
// then the file's data must not be handed to the driver.
inline bool guardSet(const std::string& dir) noexcept {
#if RECOMP_PCACHE_MUTANT == 41 // mutant 41: nothing is left behind for the next start to find
    (void)dir; return true;
#else
    try {
        Descriptor made;
        made.fd = ::open((dir + "/" + GuardName).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (made.fd < 0) return false;
        (void)::fsync(made.fd);
        return true;
    } catch (...) { return false; }
#endif
}
// Removes it. True: there was one and it is gone.
inline bool guardClear(const std::string& dir) noexcept {
    try { return ::unlink((dir + "/" + GuardName).c_str()) == 0; } catch (...) { return false; }
}

// ---- save
struct SaveResult { Reason reason = Reason::WriteFailed; int error = 0; };
// Writes header + payload to the temporary file and renames it over the cache file. On any failure the cache file is what
// it was. The temporary file is removed after a failed truncate, write, fsync or rename; it stays when this save stood back
// (busy: it is the other writer's) or could not ask about its own descriptor (not-a-file), and when the removal itself fails.
// The next save takes a temporary file that was left over.
[[gnu::noinline]] inline SaveResult save(const std::string& dir, const std::uint8_t* payload, std::size_t n, const Identity& identity,
                                         const DeviceId& device, std::size_t cap) noexcept {
    SaveResult out;
    try {
        if (RECOMP_PCACHE_MUTANT != 15 && n > cap) { out.reason = Reason::OverCap; return out; }
        // never write what the load would refuse (a driver that hands back something else than its own header)
        if (const Reason vk = checkVkHeader(payload, n, device); RECOMP_PCACHE_MUTANT != 16 && vk != Reason::Ok) { out.reason = vk; return out; }
        std::uint8_t header[HeaderBytes];
        makeHeader(header, payload, n, identity);
        const std::string finalPath = dir + "/" + FileName;
#if RECOMP_PCACHE_MUTANT == 17 // mutant 17: no temporary file (the cache file itself is truncated and rewritten)
        const std::string tempPath = finalPath;
#else
        const std::string tempPath = dir + "/" + TempName;
#endif
        int fd = -1;
        for (int attempt = 0; attempt < 2 && fd < 0; ++attempt) {
            fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
            if (fd < 0) { out.error = errno; out.reason = Reason::OpenFailed; return out; }
            // One writer at a time (a second game process, a restarted game while the old one still runs): the lock dies with
            // its process. The name must still be THIS file once we hold the lock (the other writer may just have renamed it
            // into place: then our descriptor is the cache file itself, which must never be truncated).
            if (RECOMP_PCACHE_MUTANT != 18 && ::flock(fd, LOCK_EX | LOCK_NB) != 0) { out.error = errno; out.reason = Reason::Busy; (void)::close(fd); return out; }
            struct stat mine {}, named {};
            if (::fstat(fd, &mine) != 0 || !S_ISREG(mine.st_mode)) { out.error = errno; out.reason = Reason::NotAFile; (void)::close(fd); return out; }
            // (mutant 36: no such look - the descriptor of a file the other writer has just renamed into place is written to)
            if (RECOMP_PCACHE_MUTANT != 36 && (::lstat(tempPath.c_str(), &named) != 0 || named.st_ino != mine.st_ino || named.st_dev != mine.st_dev)) { (void)::close(fd); fd = -1; }
            // Repair 1: and it must have no second name. A hard link at the temporary name (to the cache file, say) would be
            // written through: the temporary NAME is taken away - the file keeps its other name and its bytes - and the next
            // round makes a file of its own. (mutant 48: it is written through)
            else if (RECOMP_PCACHE_MUTANT != 48 && mine.st_nlink > 1) { (void)::unlink(tempPath.c_str()); (void)::close(fd); fd = -1; }
        }
        if (fd < 0) { out.reason = Reason::Busy; return out; }
        const auto fail = [&](Reason why) {
            out.error = errno; out.reason = why;
#if RECOMP_PCACHE_MUTANT != 17
            (void)::unlink(tempPath.c_str());
#endif
            (void)::close(fd);
            return out;
        };
        if (::ftruncate(fd, 0) != 0) return fail(Reason::WriteFailed);
        const auto writeAll = [&](const std::uint8_t* p, std::size_t count) {
            while (count) {
                const auto put = ::write(fd, p, count);
                if (put < 0) { if (errno == EINTR) continue; return false; }
                if (put == 0) { errno = EIO; return false; }
                p += put; count -= static_cast<std::size_t>(put);
            }
            return true;
        };
        if (!writeAll(header, HeaderBytes) || !writeAll(payload, n)) return fail(Reason::WriteFailed);
        if (RECOMP_PCACHE_MUTANT != 37 && ::fsync(fd) != 0) return fail(Reason::SyncFailed); // mutant 37: renamed without its data being on the disk
#if RECOMP_PCACHE_MUTANT != 17
        if (::rename(tempPath.c_str(), finalPath.c_str()) != 0) return fail(Reason::RenameFailed);
#endif
        (void)::close(fd);
        // (the rename itself; best effort. mutant 38: the directory is not synced)
        if (const int dirFd = RECOMP_PCACHE_MUTANT == 38 ? -1 : ::open(dir.c_str(), O_RDONLY | O_CLOEXEC); dirFd >= 0) { (void)::fsync(dirFd); (void)::close(dirFd); }
        out.reason = Reason::Ok; out.error = 0;
    } catch (...) { out.reason = Reason::Exception; }
    return out;
}

// ---- when to save (a pure function: the saver thread calls it at every poll)
struct Timing {
    std::chrono::nanoseconds poll{std::chrono::seconds(2)};        // how often the saver thread looks at the signals (two atomic loads)
    std::chrono::nanoseconds minInterval{std::chrono::seconds(60)};       // growth: never two attempts closer than this ...
    std::chrono::nanoseconds burstInterval{std::chrono::seconds(15)};     // ... unless `burst` pipelines are new: then this close
    std::chrono::nanoseconds busyInterval{std::chrono::seconds(180)};     // growth while creations never pause: this late at most
    std::chrono::nanoseconds periodicInterval{std::chrono::seconds(300)}; // whatever the signals say: one look this late
    std::uint64_t growth = 16;                                             // "grown noticeably": this many new pipelines
    std::uint64_t burst = 100;                                             // a whole scene's worth (the first scene: ~200 in one frame)
    std::uint64_t confirm = 100;                                           // the guard: pipelines created through a loaded file before the start counts as lived (the first scene asks for ~310 while still loading)
};
struct PolicyState {
    std::uint64_t createdAtLastAttempt = 0, createdAtLastPoll = 0;
    std::int64_t lastAttemptNs = 0;
    bool warmupHandled = false;
};
enum class Why : unsigned { None, Warmup, Growth, Periodic, End };
inline const char* name(Why w) noexcept {
    switch (w) { case Why::None: return "none"; case Why::Warmup: return "warmup"; case Why::Growth: return "growth";
                 case Why::Periodic: return "periodic"; case Why::End: return "end"; }
    return "?";
}
// Warmup: once, as soon as the warm-up has ended (the second start already profits). Growth: at least `growth` pipelines
// since the last attempt, at least minInterval after it (burstInterval when `burst` or more are new: a start that is ended
// soon after its first scene must still leave that scene's pipelines behind), and none created during the last poll period
// (not in the middle of a burst of compiles: the save's copy needs the lock those compiles need) - or busyInterval after it
// whatever happens.
// Periodic: periodicInterval after the last attempt, whatever the signals say (a few stragglers; pipelines nothing counts:
// compute, clear and presenter pipelines, a run without recipes; a failed save to repeat). An attempt that finds the cache
// the size it was writes nothing.
inline Why decide(const PolicyState& s, const Timing& t, std::uint64_t created, bool warmupDone, std::int64_t nowNs) noexcept {
    if (warmupDone && !s.warmupHandled) return Why::Warmup;
    const std::uint64_t fresh = created - s.createdAtLastAttempt;
    const std::int64_t since = nowNs - s.lastAttemptNs;
#if RECOMP_PCACHE_MUTANT == 21 // mutant 21: no rate limit
    const bool due = true;
#elif RECOMP_PCACHE_MUTANT == 34 // mutant 34: a whole scene's worth waits as long as sixteen pipelines
    const bool due = since >= t.minInterval.count();
#else
    const bool due = since >= t.minInterval.count() || (fresh >= t.burst && since >= t.burstInterval.count());
#endif
#if RECOMP_PCACHE_MUTANT == 22 // mutant 22: a save in the middle of a burst of creations
    const bool calm = true;
#else
    const bool calm = created == s.createdAtLastPoll || since >= t.busyInterval.count();
#endif
    if (fresh >= t.growth && due && calm) return Why::Growth;
    if (RECOMP_PCACHE_MUTANT != 27 && since >= t.periodicInterval.count()) return Why::Periodic; // mutant 27: no periodic look
    return Why::None;
}

// ---- the log: tag RecompRenderer, INFO - exactly what generic_renderer.cpp's GLOG expands to (device.cpp has no GLOG);
// stdout where there is no Android log. The marker is a literal at each call (tools/iter/quiet_log_check.py lists them).
inline std::int64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void hex8(const std::uint8_t* p, char out[17]) noexcept { std::snprintf(out, 17, "%02x%02x%02x%02x%02x%02x%02x%02x", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]); }

// ---- the saver thread
class Saver final {
public:
    // vkGetPipelineCacheData for the device's cache: *size in = the buffer's bytes (ignored when data is null), out = the
    // bytes needed / written. Returns the VkResult (0 = VK_SUCCESS, 5 = VK_INCOMPLETE).
    using GetData = std::function<int(std::size_t* size, void* data)>;
    struct Config {
        std::string dir;
        Identity identity;
        DeviceId device;
        std::size_t cap = DefaultCapBytes;
        Timing timing;
        bool confirm = false; // this start handed the file's data to the driver (guardSet): the saver removes the guard once the start has lived with it
    };
    struct Stats {
        std::atomic<std::uint64_t> attempts{0}, written{0}, unchanged{0}, failed{0}, lastBytes{0}, maxHoldNs{0}, lastHoldNs{0}, lastWaitNs{0}, lastTotalNs{0};
    };
    // cacheMutex: the lock every vkCreate*Pipelines of this cache is made under (ProbeDevice::pipelineCacheMutex).
    Saver(Config config, std::mutex& cacheMutex, GetData get, Signals& signalsIn = signals)
        : config_(std::move(config)), cacheMutex_(cacheMutex), get_(std::move(get)), signals_(signalsIn), confirmPending_(config_.confirm),
          createdAtStart_(RECOMP_PCACHE_MUTANT == 47 ? 0 : signalsIn.created.load(std::memory_order_relaxed)) { // mutant 47: pipelines made before this device count for it
        // What the driver reports for the cache as it was created (empty, or made from the file): the size a later query is
        // compared with. Equal = nothing was added = nothing to write (a start whose pipelines all came from the file).
        std::size_t size = 0;
        { std::lock_guard lock(cacheMutex_); if (get_(&size, nullptr) == 0) persisted_ = size; }
        state_.lastAttemptNs = nowNs();
#if defined(RECOMP_PCACHE_MUTANT_INLINE_SAVE)
        mutantSelf() = this;
        mutantInlineSave.store([] { if (Saver* s = mutantSelf()) s->attempt(Why::Growth, 0); });
#endif
        thread_ = std::thread([this] { run(); });
    }
    // The orderly end: the thread is told to stop, looks one last time (and writes if the cache is not the size it was), and
    // is joined. After this no call of GetData can happen (the cache may be destroyed).
    ~Saver() {
#if defined(RECOMP_PCACHE_MUTANT_INLINE_SAVE)
        mutantInlineSave.store(nullptr); mutantSelf() = nullptr;
#endif
        { std::lock_guard lock(m_); stop_ = true; }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    Saver(const Saver&) = delete;
    Saver& operator=(const Saver&) = delete;
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
#if defined(RECOMP_PCACHE_MUTANT_INLINE_SAVE)
    static Saver*& mutantSelf() { static Saver* s = nullptr; return s; }
#endif
    void run() noexcept {
#if defined(__ANDROID__) || defined(__linux__)
        (void)pthread_setname_np(pthread_self(), "recomp-pcache");
#elif defined(__APPLE__)
        (void)pthread_setname_np("recomp-pcache");
#endif
        for (;;) {
            {
                std::unique_lock lock(m_);
                if (cv_.wait_for(lock, config_.timing.poll, [this] { return stop_; })) break;
            }
            const std::uint64_t created = signals_.created.load(std::memory_order_relaxed);
            const bool done = signals_.warmupDone.load(std::memory_order_acquire);
            // The guard: this start handed the file's data to the driver, and the renderer has now made that many pipelines with it.
            // (Counted from this saver's start: the signal is the process's, and a device can be made a second time in one process.)
            if (RECOMP_PCACHE_MUTANT != 46 && confirmPending_ && created - createdAtStart_ >= config_.timing.confirm) removeGuard("pipelines", created); // mutant 46: not this way
            if (const Why why = decide(state_, config_.timing, created, done, nowNs()); why != Why::None) attempt(why, created);
            if (done) state_.warmupHandled = true;
            state_.createdAtLastPoll = created;
        }
#if RECOMP_PCACHE_MUTANT != 23 // mutant 23: no save at the orderly end
        attempt(Why::End, signals_.created.load(std::memory_order_relaxed)); // (writes only if the cache is not the size it was)
#endif
    }
    // One driver call under the device's lock. waited: until the lock was ours (a creation had it). held: while it was ours.
    int underLock(std::size_t* size, void* data, std::int64_t& held, std::int64_t& waited) {
        const std::int64_t w0 = nowNs();
#if RECOMP_PCACHE_MUTANT != 24 // mutant 24: the driver is asked without the device's lock
        std::lock_guard lock(cacheMutex_);
#endif
        const std::int64_t h0 = RECOMP_PCACHE_MUTANT == 49 ? w0 : nowNs(); // mutant 49 (the code before repair 1): the wait for the lock counts as holding it
        waited += h0 - w0;
        const int r = get_(size, data);
        held += nowNs() - h0;
        return r;
    }
    // One save attempt. The device's lock is held across the two driver calls only (hold_ms in the log line is their sum;
    // wait_ms is the time it took to get the lock for them).
    void attempt(Why why, std::uint64_t created) noexcept {
        std::lock_guard serial(attemptMutex_); // (one attempt at a time, whoever calls)
        const std::int64_t t0 = nowNs();
        std::int64_t held = 0, waited = 0;
        std::size_t bytes = 0;
        SaveResult result;
        try {
            for (int round = 0; round < 3; ++round) {
                std::size_t size = 0;
                int r = underLock(&size, nullptr, held, waited);
                bytes = size;
                if (r != 0) { result.reason = Reason::QueryFailed; result.error = r; break; }
                if (size == persisted_) { result.reason = Reason::Unchanged; break; }
                if (RECOMP_PCACHE_MUTANT != 15 && size > config_.cap) { result.reason = Reason::OverCap; break; }
                // The buffer is made (and its pages touched) BEFORE the lock; some room for what the warm-up adds meanwhile.
                buffer_.assign(std::min(config_.cap, size + std::max<std::size_t>(size / 16, 64 * 1024)), 0);
                std::size_t got = buffer_.size();
#if RECOMP_PCACHE_MUTANT == 25 || RECOMP_PCACHE_MUTANT == 39 // mutant 25: the device's lock is kept for the checksum and the file too
                std::unique_lock keep(cacheMutex_, std::defer_lock);     // mutant 39: the same, but only for the saves made during play
                if (RECOMP_PCACHE_MUTANT == 25 || why == Why::Growth || why == Why::Periodic) keep.lock();
                if (keep.owns_lock()) { const std::int64_t h0 = nowNs(); r = get_(&got, buffer_.data()); held += nowNs() - h0; }
                else r = underLock(&got, buffer_.data(), held, waited);
#else
                r = underLock(&got, buffer_.data(), held, waited);
#endif
#if RECOMP_PCACHE_MUTANT == 26 // mutant 26: a cut-off copy is written as if it were the cache
                if (r == 5) r = 0;
#endif
                if (r == 5 /* VK_INCOMPLETE: it grew past the room; ask again */) { result.reason = Reason::Incomplete; continue; }
                if (r != 0) { result.reason = Reason::GetFailed; result.error = r; break; }
                bytes = got;
                result = save(config_.dir, buffer_.data(), got, config_.identity, config_.device, config_.cap);
                if (result.reason == Reason::Ok) persisted_ = got;
                break;
            }
        } catch (...) { result.reason = Reason::Exception; }
        std::vector<std::uint8_t>().swap(buffer_); // (the megabytes are not kept between saves)
        const std::int64_t t1 = nowNs();
        state_.createdAtLastAttempt = created;
        state_.lastAttemptNs = t1;
        stats_.attempts.fetch_add(1);
        (result.reason == Reason::Ok ? stats_.written : result.reason == Reason::Unchanged ? stats_.unchanged : stats_.failed).fetch_add(1);
        stats_.lastBytes.store(bytes); stats_.lastHoldNs.store(std::uint64_t(held)); stats_.lastWaitNs.store(std::uint64_t(waited)); stats_.lastTotalNs.store(std::uint64_t(t1 - t0));
        if (std::uint64_t(held) > stats_.maxHoldNs.load()) stats_.maxHoldNs.store(std::uint64_t(held));
        // The guard (guardSet): gone once the start that handed the file's data to the driver has lived to a save attempt
        // (whatever the attempt found), and whenever a new file is in place (what was suspect has been replaced).
        if (RECOMP_PCACHE_MUTANT != 43 && confirmPending_) removeGuard("attempt", created);                      // mutant 43: not this way
        else if (RECOMP_PCACHE_MUTANT != 44 && result.reason == Reason::Ok) removeGuard("new-file", created);  // mutant 44: not this way
        // One line per attempt that wrote or failed; "unchanged" is said once, after the warm-up (the start that needed nothing).
        if (result.reason == Reason::Unchanged && why != Why::Warmup) return;
        char tail[320];
        std::snprintf(tail, sizeof tail, "ok=%u why=%s reason=%s bytes=%zu ms=%.1f hold_ms=%.2f wait_ms=%.2f pipelines=%llu warmup=%llu warmup_done=%u errno=%d cap_bytes=%zu",
                      unsigned(result.reason == Reason::Ok), name(why), name(result.reason), bytes, double(t1 - t0) * 1e-6, double(held) * 1e-6, double(waited) * 1e-6,
                      static_cast<unsigned long long>(created), static_cast<unsigned long long>(signals_.warmupCompiled.load(std::memory_order_relaxed)),
                      unsigned(signals_.warmupDone.load(std::memory_order_acquire)), result.error, config_.cap);
#if defined(RECOMP_PCACHE_ALOG)
        __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", "RENDER_DIAG_PCACHE_SAVE %s", tail);
#else
        std::printf("RENDER_DIAG_PCACHE_SAVE %s\n", tail);
#endif
    }
    // The guard file goes (the saver thread only). One line when there was one: why = pipelines | attempt | new-file.
    void removeGuard(const char* why, std::uint64_t created) noexcept {
        confirmPending_ = false;
        if (!guardClear(config_.dir)) return;
#if defined(RECOMP_PCACHE_ALOG)
        __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", "RENDER_DIAG_PCACHE_GUARD removed=1 why=%s pipelines=%llu", why, static_cast<unsigned long long>(created));
#else
        std::printf("RENDER_DIAG_PCACHE_GUARD removed=1 why=%s pipelines=%llu\n", why, static_cast<unsigned long long>(created));
#endif
    }

    const Config config_;
    std::mutex& cacheMutex_;
    const GetData get_;
    Signals& signals_;
    bool confirmPending_ = false;    // saver thread: this start handed the file's data to the driver and its guard is still there
    const std::uint64_t createdAtStart_; // the renderer's count when this saver was made
    std::size_t persisted_ = 0;      // saver thread (and the constructor before the thread exists)
    PolicyState state_;              // saver thread
    std::vector<std::uint8_t> buffer_;
    Stats stats_;
    std::mutex attemptMutex_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

// The start line (device.cpp calls it once per device).
inline void logLoad(bool on, const Loaded& seed, bool handedToDriver, std::size_t cap, double ms, const Identity& identity, const char* identitySource,
                    const DeviceId& device) noexcept {
    char id[17], tail[320];
    hex8(identity.bytes, id);
    std::snprintf(tail, sizeof tail, "on=%u loaded=%u bytes=%zu reason=%s file_bytes=%lld errno=%d cap_bytes=%zu ms=%.1f identity=%s id_source=%s vendor=%x device=%x",
                  unsigned(on), unsigned(handedToDriver), handedToDriver ? seed.file.size() - HeaderBytes : std::size_t{0}, name(seed.reason), seed.fileBytes, seed.error, cap, ms,
                  id, identitySource, device.vendorID, device.deviceID);
#if defined(RECOMP_PCACHE_ALOG)
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer", "RENDER_DIAG_PCACHE_LOAD %s", tail);
#else
    std::printf("RENDER_DIAG_PCACHE_LOAD %s\n", tail);
#endif
}
} // namespace recomp::pcache
