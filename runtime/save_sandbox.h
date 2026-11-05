#pragma once

// Save sandbox for test runs (agent save-sandbox, 2026-10-04).
//
// BOOT switch <files>/private-trace/diag-save-sandbox, read once when the saves state is first made
// (guest_import_registry.cpp savesStateFor, whose only change is that its `root` comes from filesRootFor() below).
// Absent: filesRootFor() returns the files root it was given, so v458's own root + "/saves" and root + "/profile" are
// the strings v458 built, and nothing else is touched. Present: it returns <files>/saves-sandbox/<name>, so the
// saved-game store and the local profile of this process live below that directory (saves/ and profile/) and the real
// <files>/saves and <files>/profile are opened read-only at most (for the initial copy), never created, written,
// renamed, truncated or deleted.
//
// Switch file content (ASCII, at most two whitespace-separated words, at most 256 bytes):
//   copy  [name]   new sandbox; saves/ starts as a verified copy of the real saves (a normal load works)
//   empty [name]   new sandbox; saves/ starts empty (the game finds no save); the real saves are not opened at all
//   reuse  name    continue an existing, completed sandbox (e.g. load the autosave an earlier sandbox run wrote)
// profile/ is always a verified copy of the real profile (identity.txt names the XUID that owns the saves; title
// settings and achievements.txt are written by the game, so they are sandboxed too).
// name: 1..64 of [A-Za-z0-9._-], first character not '.'. Without a name, copy/empty use the run id the caller passes
// (run-YYYYMMDD-HHMMSS-pPID). copy/empty refuse an existing directory: this file contains no code that deletes or
// overwrites anything, in the sandbox or outside it.
//
// Anything else (empty or unknown content, an unreadable switch, a bad name, an existing directory, a symlink on the
// way, a source entry that is not a regular file or directory, a copy whose size or SHA-256 does not verify, a reused
// sandbox that holds a symlink or a hard-linked file) FAILS: Roots::ok is false, savesRoot and profileDir are empty and
// filesRootFor() returns FailedRoot, a path below /dev/null under which no directory can exist. v458's own
// loadOrCreateIdentity then fails (SAVES_INIT_FAILED) and the run has no saves state: there is no fall back to the
// real saves.
//
// -DRECOMP_SAVE_SANDBOX_REQUIRED=1 (for a test attempt only; default off): an ABSENT switch also fails, so a build made
// for a sandboxed run cannot use the real saves because the switch file was forgotten or misnamed.
//
// POSIX only. The log goes to the Android log when <android/log.h> can be included, to stderr otherwise (the plain
// host tests in agents/save-sandbox/tests compile this file as is).

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if __has_include(<android/log.h>)
#include <android/log.h>
#define RECOMP_SAVE_SANDBOX_ANDROID_LOG 1
#endif

namespace recomp::bootstrap::save_sandbox {

inline constexpr const char* SwitchRelative = "/private-trace/diag-save-sandbox";
inline constexpr const char* SandboxParent = "/saves-sandbox";
inline constexpr const char* ReadyMarker = "SANDBOX_READY";
inline constexpr std::size_t MaximumSwitchBytes = 256;
inline constexpr std::size_t MaximumNameBytes = 64;
inline constexpr int MaximumCopyDepth = 32;
inline constexpr std::uint64_t MaximumCopyBytes = 2ULL * 1024 * 1024 * 1024;
// What filesRootFor() returns when the sandbox failed: /dev/null is a character device on Android and on the host, so
// no directory can be created or opened below it, by v458's profile loader or by anything else.
inline constexpr const char* FailedRoot = "/dev/null/save-sandbox-failed";
#if defined(RECOMP_SAVE_SANDBOX_REQUIRED)
inline constexpr bool SandboxRequired = true;
#else
inline constexpr bool SandboxRequired = false;
#endif

enum class Mode : std::uint8_t { Off, Copy, Empty, Reuse, Failed };

inline const char* modeName(Mode mode) noexcept {
    switch (mode) {
    case Mode::Off: return "off";
    case Mode::Copy: return "copy";
    case Mode::Empty: return "empty";
    case Mode::Reuse: return "reuse";
    default: return "failed";
    }
}

// ---- the path decision (pure: no file system, no clock) ----------------------------------------------------------

struct Decision {
    Mode mode{Mode::Failed};
    std::string savesRoot, profileDir; // what the store / the profile use; both empty when mode is Failed
    std::string sandboxDir;            // <files>/saves-sandbox/<name>; empty unless Copy, Empty or Reuse
    std::string error;                 // why mode is Failed
};

inline bool isValidName(std::string_view name) noexcept {
    if (name.empty() || name.size() > MaximumNameBytes || name.front() == '.') { return false; }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

inline Decision decide(const std::string& filesRoot, bool switchPresent, std::string_view content, std::string_view runId) {
    Decision out;
    if (!switchPresent) {
        out.mode = Mode::Off;
        out.savesRoot = filesRoot + "/saves";
        out.profileDir = filesRoot + "/profile";
        return out;
    }
    const auto fail = [&](std::string why) { out = Decision{}; out.error = std::move(why); return out; };
    if (content.size() > MaximumSwitchBytes) { return fail("switch content is longer than 256 bytes"); }
    std::vector<std::string_view> words;
    for (std::size_t at = 0; at < content.size();) {
        const auto isSpace = [&](std::size_t i) { return content[i] == ' ' || content[i] == '\t' || content[i] == '\n' || content[i] == '\r'; };
        if (isSpace(at)) { ++at; continue; }
        std::size_t end = at;
        while (end < content.size() && !isSpace(end)) { ++end; }
        words.push_back(content.substr(at, end - at));
        at = end;
    }
    if (words.empty()) { return fail("switch content is empty: write copy, empty or reuse <name>"); }
    if (words.size() > 2) { return fail("switch content has more than two words"); }
    if (words[0] == "copy") { out.mode = Mode::Copy; }
    else if (words[0] == "empty") { out.mode = Mode::Empty; }
    else if (words[0] == "reuse") { out.mode = Mode::Reuse; }
    else { return fail("switch content names no mode: write copy, empty or reuse <name>"); }
    if (out.mode == Mode::Reuse && words.size() != 2) { return fail("reuse needs the name of an existing sandbox"); }
    const std::string_view name = words.size() == 2 ? words[1] : runId;
    if (!isValidName(name)) { return fail("sandbox name must be 1..64 of [A-Za-z0-9._-] and not start with a dot"); }
    if (filesRoot.empty() || filesRoot.front() != '/') { return fail("files root is not an absolute path"); }
    out.sandboxDir = filesRoot + SandboxParent + "/" + std::string(name);
    out.savesRoot = out.sandboxDir + "/saves";
    out.profileDir = out.sandboxDir + "/profile";
    return out;
}

// ---- SHA-256 (FIPS 180-4), for the copy verification and the tree digests ---------------------------------------

class Sha256 {
public:
    void update(const std::uint8_t* data, std::size_t size) noexcept {
        total_ += size;
        while (size > 0) {
            const std::size_t take = std::min(size, block_.size() - fill_);
            std::memcpy(block_.data() + fill_, data, take);
            fill_ += take; data += take; size -= take;
            if (fill_ == block_.size()) { compress(); fill_ = 0; }
        }
    }
    [[nodiscard]] std::string hex() noexcept {
        const std::uint64_t bits = total_ * 8;
        const std::uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (fill_ != 56) { update(&zero, 1); }
        std::array<std::uint8_t, 8> length{};
        for (int i = 0; i < 8; ++i) { length[std::size_t(i)] = std::uint8_t(bits >> (56 - 8 * i)); }
        update(length.data(), length.size());
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        for (const std::uint32_t word : state_) {
            for (int shift = 28; shift >= 0; shift -= 4) { out.push_back(digits[(word >> shift) & 15]); }
        }
        return out;
    }

private:
    static std::uint32_t rotr(std::uint32_t v, int n) noexcept { return (v >> n) | (v << (32 - n)); }
    void compress() noexcept {
        static constexpr std::uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const auto* p = block_.data() + 4 * i;
            w[i] = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d; state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }
    std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> block_{};
    std::size_t fill_{0};
    std::uint64_t total_{0};
};

// ---- the copy (the source is only ever opened O_RDONLY) ---------------------------------------------------------

struct TreeSummary {
    std::uint64_t files{0}, bytes{0};
    // SHA-256 of the text "<sha256 of file>  ./<relative path>\n" for every regular file, sorted by path bytes: what
    //   (cd <tree> && find . -type f | LC_ALL=C sort | xargs sha256sum | sha256sum)
    // prints for a tree with at least one file, so a log line can be compared with a listing taken over adb.
    std::string digest;
    std::vector<std::pair<std::string, std::string>> entries; // relative path, file sha256
};

namespace detail {

inline std::string errnoText(const std::string& what, int error) { return what + ": " + std::strerror(error); }

inline bool isRealDirectory(const std::string& path) noexcept {
    struct stat st{};
    return ::lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Reads a whole regular file read-only; returns its size and SHA-256.
inline bool hashFile(const std::string& path, std::uint64_t& size, std::string& hash, std::string& error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { error = errnoText("cannot open " + path, errno); return false; }
    Sha256 sha; size = 0;
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;) {
        const auto got = ::read(fd, buffer.data(), buffer.size());
        if (got < 0) { if (errno == EINTR) { continue; } error = errnoText("cannot read " + path, errno); (void)::close(fd); return false; }
        if (got == 0) { break; }
        sha.update(buffer.data(), std::size_t(got)); size += std::uint64_t(got);
    }
    (void)::close(fd);
    hash = sha.hex();
    return true;
}

// The copy's check: the file at `path` must have exactly this size and this SHA-256.
inline bool verifyFile(const std::string& path, std::uint64_t expectedSize, const std::string& expectedHash, std::string& error) {
    std::uint64_t size = 0; std::string hash;
    if (!hashFile(path, size, hash, error)) { return false; }
    if (size != expectedSize) { error = "copy verification failed (size) for " + path; return false; }
    if (hash != expectedHash) { error = "copy verification failed (sha256) for " + path; return false; }
    return true;
}

// source: O_RDONLY | O_NOFOLLOW. target: created O_EXCL (never an existing file), fsynced, then re-read and verified.
inline bool copyFile(const std::string& source, const std::string& target, std::uint64_t& size, std::string& hash, std::string& error) {
    const int in = ::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0) { error = errnoText("cannot open " + source, errno); return false; }
    struct stat st{};
    if (::fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) { error = "not a regular file: " + source; (void)::close(in); return false; }
    const int out = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) { error = errnoText("cannot create " + target, errno); (void)::close(in); return false; }
    Sha256 sha; size = 0; bool ok = true;
    std::vector<std::uint8_t> buffer(64 * 1024);
    while (ok) {
        const auto got = ::read(in, buffer.data(), buffer.size());
        if (got < 0) { if (errno == EINTR) { continue; } error = errnoText("cannot read " + source, errno); ok = false; break; }
        if (got == 0) { break; }
        sha.update(buffer.data(), std::size_t(got)); size += std::uint64_t(got);
        for (std::size_t done = 0; ok && done < std::size_t(got);) {
            const auto put = ::write(out, buffer.data() + done, std::size_t(got) - done);
            if (put < 0) { if (errno == EINTR) { continue; } error = errnoText("cannot write " + target, errno); ok = false; break; }
            done += std::size_t(put);
        }
    }
    if (ok && ::fsync(out) != 0) { error = errnoText("cannot sync " + target, errno); ok = false; }
    if (::close(out) != 0 && ok) { error = errnoText("cannot close " + target, errno); ok = false; }
    (void)::close(in);
    if (!ok) { return false; }
    hash = sha.hex();
    if (size != std::uint64_t(st.st_size)) { error = "source changed size while it was copied: " + source; return false; }
    return verifyFile(target, size, hash, error);
}

inline bool sortedNames(const std::string& path, std::vector<std::string>& names, std::string& error) {
    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) { error = errnoText("cannot list " + path, errno); return false; }
    errno = 0;
    while (const dirent* entry = ::readdir(dir)) {
        const std::string name(entry->d_name);
        if (name != "." && name != "..") { names.push_back(name); }
        errno = 0;
    }
    const int readError = errno;
    (void)::closedir(dir);
    if (readError != 0) { error = errnoText("cannot list " + path, readError); return false; }
    std::sort(names.begin(), names.end());
    return true;
}

inline bool copyLevel(const std::string& source, const std::string& target, const std::string& relative, int depth,
                      TreeSummary& summary, std::string& error) {
    if (depth > MaximumCopyDepth) { error = "source tree is deeper than 32 levels: " + source; return false; }
    if (::mkdir(target.c_str(), 0700) != 0) { error = errnoText("cannot create " + target, errno); return false; }
    std::vector<std::string> names;
    if (!sortedNames(source, names, error)) { return false; }
    for (const auto& name : names) {
        const auto from = source + "/" + name, to = target + "/" + name, rel = relative + "/" + name;
        struct stat st{};
        if (::lstat(from.c_str(), &st) != 0) { error = errnoText("cannot stat " + from, errno); return false; }
        if (S_ISDIR(st.st_mode)) {
            if (!copyLevel(from, to, rel, depth + 1, summary, error)) { return false; }
        } else if (S_ISREG(st.st_mode)) {
            std::uint64_t size = 0; std::string hash;
            if (!copyFile(from, to, size, hash, error)) { return false; }
            summary.files += 1; summary.bytes += size;
            if (summary.bytes > MaximumCopyBytes) { error = "source tree is larger than 2 GiB: " + source; return false; }
            summary.entries.emplace_back(rel, hash);
        } else {
            error = "source entry is neither a regular file nor a directory (not copied, not followed): " + from;
            return false;
        }
    }
    return true;
}

inline void finishDigest(TreeSummary& summary) {
    std::sort(summary.entries.begin(), summary.entries.end());
    Sha256 sha;
    for (const auto& [path, hash] : summary.entries) {
        const std::string line = hash + "  " + path + "\n";
        sha.update(reinterpret_cast<const std::uint8_t*>(line.data()), line.size());
    }
    summary.digest = sha.hex();
}

// Copies the directory `source` to the new directory `target` (which must not exist). A missing source gives an empty
// target. Nothing is ever removed: a failed copy leaves its partial target behind (without the ready marker).
inline bool copyTree(const std::string& source, const std::string& target, TreeSummary& summary, std::string& error) {
    summary = TreeSummary{};
    struct stat st{};
    if (::lstat(source.c_str(), &st) != 0) {
        if (errno != ENOENT) { error = errnoText("cannot stat " + source, errno); return false; }
        if (::mkdir(target.c_str(), 0700) != 0) { error = errnoText("cannot create " + target, errno); return false; }
    } else if (!S_ISDIR(st.st_mode)) {
        error = "source is not a real directory: " + source; return false;
    } else if (!copyLevel(source, target, ".", 0, summary, error)) {
        return false;
    }
    finishDigest(summary);
    return true;
}

// reuse: everything inside an existing sandbox must be a real directory or a regular file with one link. A symlink or a
// hard link (made by hand; neither the game nor this file makes one) could lead a write into the real saves.
inline bool checkOwnTree(const std::string& path, int depth, std::string& error) {
    if (depth > MaximumCopyDepth) { error = "sandbox is deeper than 32 levels: " + path; return false; }
    std::vector<std::string> names;
    if (!sortedNames(path, names, error)) { return false; }
    for (const auto& name : names) {
        const auto entry = path + "/" + name;
        struct stat st{};
        if (::lstat(entry.c_str(), &st) != 0) { error = errnoText("cannot stat " + entry, errno); return false; }
        if (S_ISDIR(st.st_mode)) {
            if (!checkOwnTree(entry, depth + 1, error)) { return false; }
        } else if (!S_ISREG(st.st_mode)) {
            error = "reuse: sandbox entry is neither a regular file nor a directory (a symlink is refused): " + entry; return false;
        } else if (st.st_nlink != 1) {
            error = "reuse: sandbox file has more than one hard link (refused): " + entry; return false;
        }
    }
    return true;
}

inline bool writeMarker(const std::string& path, const std::string& text, std::string& error) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { error = errnoText("cannot create " + path, errno); return false; }
    bool ok = true;
    for (std::size_t done = 0; ok && done < text.size();) {
        const auto put = ::write(fd, text.data() + done, text.size() - done);
        if (put < 0) { if (errno == EINTR) { continue; } ok = false; break; }
        done += std::size_t(put);
    }
    ok = ok && ::fsync(fd) == 0;
    if (!ok) { error = errnoText("cannot write " + path, errno); }
    (void)::close(fd);
    return ok;
}

} // namespace detail

// ---- what the registry calls -------------------------------------------------------------------------------------

struct Roots {
    bool ok{false};        // false: do not create any saves state for this run
    bool active{false};    // true: the roots are inside a sandbox
    Mode mode{Mode::Failed};
    std::string savesRoot, profileDir; // both empty unless ok; always filesRoot + "/saves" and filesRoot + "/profile"
    std::string filesRoot; // what savesStateFor uses as its root: the files root (off), the sandbox directory, or FailedRoot
    std::string line;      // SAVES_SANDBOX ... (one log line; empty when the switch is absent)
};

// run-YYYYMMDD-HHMMSS-pPID (UTC): the default sandbox name of this process.
inline std::string runIdNow() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    (void)::gmtime_r(&now, &utc);
    char text[64];
    std::snprintf(text, sizeof text, "run-%04d%02d%02d-%02d%02d%02d-p%ld", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<long>(::getpid()));
    return text;
}

// Creates / checks the sandbox a Decision names. Returns "" or the reason it cannot be used.
inline std::string prepare(const Decision& decision, const std::string& filesRoot, TreeSummary& saves, TreeSummary& profile) {
    std::string error;
    const std::string parent = filesRoot + SandboxParent;
    if (::mkdir(parent.c_str(), 0700) != 0 && errno != EEXIST) { return detail::errnoText("cannot create " + parent, errno); }
    // lstat: neither files/saves-sandbox nor the sandbox itself may be a symlink (which could lead into the real saves).
    if (!detail::isRealDirectory(parent)) { return "not a real directory (a symlink is refused): " + parent; }
    if (decision.mode == Mode::Reuse) {
        if (!detail::isRealDirectory(decision.sandboxDir)) { return "reuse: no such sandbox: " + decision.sandboxDir; }
    } else if (::mkdir(decision.sandboxDir.c_str(), 0700) != 0) {
        return errno == EEXIST ? "sandbox already exists (use reuse, or another name; nothing is ever overwritten): " + decision.sandboxDir
                               : detail::errnoText("cannot create " + decision.sandboxDir, errno);
    }
    const std::string marker = decision.sandboxDir + "/" + ReadyMarker;
    if (decision.mode == Mode::Reuse) {
        struct stat st{};
        if (::lstat(marker.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) { return "reuse: sandbox was never completed (no SANDBOX_READY): " + decision.sandboxDir; }
        if (!detail::isRealDirectory(decision.savesRoot) || !detail::isRealDirectory(decision.profileDir)) {
            return "reuse: sandbox has no saves/ and profile/ directories: " + decision.sandboxDir;
        }
        if (!detail::checkOwnTree(decision.sandboxDir, 0, error)) { return error; }
        return {};
    }
    if (decision.mode == Mode::Copy) {
        if (!detail::copyTree(filesRoot + "/saves", decision.savesRoot, saves, error)) { return error; }
    } else {
        if (::mkdir(decision.savesRoot.c_str(), 0700) != 0) { return detail::errnoText("cannot create " + decision.savesRoot, errno); }
        detail::finishDigest(saves);
    }
    if (!detail::copyTree(filesRoot + "/profile", decision.profileDir, profile, error)) { return error; }
    const std::string text = std::string("mode=") + modeName(decision.mode) + "\nsaves_files=" + std::to_string(saves.files)
            + "\nsaves_bytes=" + std::to_string(saves.bytes) + "\nsaves_sha256=" + saves.digest + "\nprofile_files="
            + std::to_string(profile.files) + "\nprofile_sha256=" + profile.digest + "\n";
    if (!detail::writeMarker(marker, text, error)) { return error; }
    return {};
}

// Reads the switch and makes / checks the sandbox: the host roots of the saved-game store and of the local profile.
// Not cached, no logging (filesRootFor does both). fixedRunId: the default sandbox name; null = runIdNow(), which is
// only evaluated when the switch is present.
inline Roots rootsFor(const std::string& filesRoot, const char* fixedRunId = nullptr) {
    Roots out;
    out.filesRoot = FailedRoot;
    const std::string switchPath = filesRoot + SwitchRelative;
    std::string content;
    bool present = true, readable = false;
    const int fd = ::open(switchPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char buffer[MaximumSwitchBytes + 1];
        std::size_t filled = 0;
        readable = true;
        while (filled < sizeof buffer) {
            const auto got = ::read(fd, buffer + filled, sizeof buffer - filled);
            if (got < 0) { if (errno == EINTR) { continue; } readable = false; break; }
            if (got == 0) { break; }
            filled += std::size_t(got);
        }
        (void)::close(fd);
        content.assign(buffer, filled);
    } else if (errno == ENOENT || errno == ENOTDIR) { // (ENOTDIR: private-trace is not a directory, so no switch can exist)
        present = false;
    }
    Decision decision;
    if (present && !readable) { decision.error = "switch file exists but cannot be read: " + switchPath; }
    else if (!present && SandboxRequired) { decision.error = "this build requires the save sandbox (RECOMP_SAVE_SANDBOX_REQUIRED) and the switch file is absent: " + switchPath; }
    else { decision = decide(filesRoot, present, content, !present ? std::string() : fixedRunId != nullptr ? std::string(fixedRunId) : runIdNow()); }
    if (decision.mode == Mode::Off) {
        out.ok = true; out.mode = Mode::Off;
        out.savesRoot = std::move(decision.savesRoot); out.profileDir = std::move(decision.profileDir); out.filesRoot = filesRoot;
        return out;
    }
    TreeSummary saves, profile;
    std::string error = decision.error;
    if (decision.mode != Mode::Failed) { error = prepare(decision, filesRoot, saves, profile); }
    if (decision.mode == Mode::Failed || !error.empty()) {
        out.line = "SAVES_SANDBOX_FAILED " + (error.empty() ? std::string("no reason recorded") : error) + " (no saves state this run; the real saves are not used)";
        return out;
    }
    out.ok = true; out.active = true; out.mode = decision.mode;
    out.savesRoot = decision.savesRoot; out.profileDir = decision.profileDir; out.filesRoot = decision.sandboxDir;
    out.line = std::string("SAVES_SANDBOX mode=") + modeName(decision.mode) + " dir=" + decision.sandboxDir
            + " saves_root=" + decision.savesRoot + " profile_dir=" + decision.profileDir;
    if (decision.mode != Mode::Reuse) {
        out.line += " saves_files=" + std::to_string(saves.files) + " saves_bytes=" + std::to_string(saves.bytes)
                + " saves_sha256=" + saves.digest + " profile_files=" + std::to_string(profile.files)
                + " profile_sha256=" + profile.digest;
    }
    out.line += " real_saves=" + filesRoot + "/saves (not written)";
    return out;
}

// The one call of guest_import_registry.cpp (savesStateFor: `const std::string root = save_sandbox::filesRootFor(...)`).
// Returns the root below which v458 puts /saves and /profile: `filesRoot` itself (switch absent), the sandbox directory,
// or FailedRoot. Resolved once per process and files root (a second registry of the same process gets the same answer
// and no second copy); logs the SAVES_SANDBOX / SAVES_SANDBOX_FAILED line the first time. Never inlined, so the
// function that calls it keeps v458's control flow (and its PGO record).
// Takes a C string on purpose: the call then replaces v458's std::string constructor call one for one, with no
// temporary whose clean-up would add a branch to savesStateFor.
[[gnu::noinline, gnu::cold]] inline std::string filesRootFor(const char* filesRootText) {
    const std::string filesRoot(filesRootText);
    static std::mutex guard;
    static std::map<std::string, std::string> resolved;
    const std::lock_guard<std::mutex> lock(guard);
    const auto found = resolved.find(filesRoot);
    if (found != resolved.end()) { return found->second; }
    const Roots roots = rootsFor(filesRoot);
    if (!roots.line.empty()) {
#if defined(RECOMP_SAVE_SANDBOX_ANDROID_LOG)
        __android_log_print(roots.ok ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "RecompSaves", "%s", roots.line.c_str());
#else
        std::fprintf(stderr, "RecompSaves %s\n", roots.line.c_str());
#endif
    }
    // (ok: the files root or the sandbox directory, never empty. Not ok: FailedRoot, whatever else went wrong.)
    return resolved.emplace(filesRoot, roots.ok && !roots.filesRoot.empty() ? roots.filesRoot : std::string(FailedRoot)).first->second;
}

} // namespace recomp::bootstrap::save_sandbox
