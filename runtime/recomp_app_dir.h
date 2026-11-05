#pragma once
// data-paths (v486, 2026-10-06; from switch-inventory's item 4): the app's files directory, derived ONCE from what the loader
// passes, instead of the hard-coded user-0 directory (wrong for a second Android user or a work profile, whose files
// directory is /data/user/<id>/dev.recompbootstrap.smoke/files, and for an app moved to adopted storage).
//
// Where it comes from: the loader (android/app/src/debug/cpp/private_trace_jni.cpp, runTrace) starts the library only with an
// argument list that recomp::private_trace::parseArguments (tools/host_guest_trace/private_trace_arguments.h) accepted for
// root = <files>/private-trace, <files> being the directory string the app passed (its getFilesDir()); parseArguments requires
// argv[0] == <root>/run-<32 lower-case hex>/host_guest_trace. privateTraceEntry (trace_runner.cpp) calls setFromArgv0(argv[0])
// before runTraceMain starts any thread.
//
// How a site gets its path: RECOMP_APP_PATH("/rel") is the address of one buffer per distinct literal (a variable template
// keyed by the literal, so every site and every translation unit naming that literal shares it), constant-initialized with
// today's literal (the fall-back directory + rel). setFromArgv0 rewrites every such buffer once (they are collected in one
// linker section) when the loader's directory differs from the fall-back. A site therefore compiles to a constant address,
// as the literal did: no call, no branch, no lock and no allocation at the site (per draw, per frame or per poll), and the
// caller's control flow, call count and inlining stay as in v474 (its IR-PGO record still applies).
//
// The fall-back is today's behaviour: without that call (host builds, the ABI check), when argv[0] does not have the
// loader's shape, when the directory is too long or holds a character outside [A-Za-z0-9._-/], and when it equals the
// fall-back, no buffer is touched and every site sees exactly v474's string.
//
// Line numbers: the changed files keep v474's (__LINE__ names diagnostics), so the include shares a line with a comment.
//
// Ordering (the one rule this relies on): a site read before setFromArgv0 sees the fall-back. Nothing reads one before:
// privateTraceEntry is the library's first code after its static initializers (the v6 entry first stores the window
// callbacks: Presenter::setWindow, which reads no site and starts no thread, checked on the built object by the lane's
// tests/test_entry_call.py), and no static initializer reads a site
// (checked on the built objects: the lane's tests/static_init_sites.py). setFromArgv0 runs on the entry thread before
// runTraceMain creates any thread, so every later reader is ordered after the rewrite by its thread's creation.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// The one remaining spelling of the old directory (the fall-back; a test greps the sources for any other).
#define RECOMP_APP_FALLBACK_DIR "/data/user/0/dev.recompbootstrap.smoke/files"

#if defined(__APPLE__) // (host tests on macOS: Mach-O names a section by segment,section)
#define RECOMP_APP_SITE_SECTION "__DATA,__recomp_paths"
#else
#define RECOMP_APP_SITE_SECTION "recomp_app_paths"
#endif

namespace recomp::appdir {
inline constexpr char Fallback[] = RECOMP_APP_FALLBACK_DIR;
inline constexpr std::size_t FallbackLength = sizeof Fallback - 1;
// A derived directory is shorter than Cap bytes (longer: refused, the fall-back stays). 95: every fixed buffer that v474 fills
// from a path format (snprintf into char[128..256]) still holds its longest output with a 94-byte directory, so those
// buffers keep v474's sizes and the functions holding them keep v474's code and stack frames (tests/test_buffer_room.py
// computes the room per buffer; the tightest is gcFile's char[128] with its longest name). 94 bytes cover
// /data/user/<any id>/dev.recompbootstrap.smoke/files (<= 56) and adopted storage /mnt/expand/<uuid>/user/<id>/... (88 for
// a two-digit id, 94 for an eight-digit one).
inline constexpr std::size_t Cap = 95;
inline constexpr std::size_t RelCap = 128; // a site's relative part is shorter than RelCap bytes (checked at compile time)

// One site buffer: the path, NUL-terminated (fixed size, so the section is an array of them).
struct alignas(16) Site { char path[Cap + RelCap]; };
static_assert(sizeof(Site) % alignof(Site) == 0);

// A site's literal at compile time: the fall-back followed by "" or by a relative part starting with '/'.
template <std::size_t N> struct Literal {
    char text[N] = {};
    consteval Literal(const char (&s)[N]) {
        static_assert(N >= sizeof Fallback && N - sizeof Fallback < RelCap, "RECOMP_APP_PATH: relative path too long");
        for (std::size_t i = 0; i < N; ++i) text[i] = s[i];
        for (std::size_t i = 0; i < FallbackLength; ++i)
            if (text[i] != Fallback[i]) throw "RECOMP_APP_PATH: not the fall-back directory";
        if (N > sizeof Fallback && text[FallbackLength] != '/') throw "RECOMP_APP_PATH: the relative path starts with '/'";
    }
};
template <Literal L> consteval Site initial() {
    Site site{};
    for (std::size_t i = 0; i < sizeof L.text; ++i) site.path[i] = L.text[i];
    return site;
}
// The buffer of one literal (one per library: an inline variable, merged across translation units).
template <Literal L> [[gnu::section(RECOMP_APP_SITE_SECTION)]] inline constinit Site site = initial<L>();

enum Why : unsigned { NotCalled = 0, WhyDerived, WhySame, WhyTwice, WhyShape, WhyTooLong, WhyCharacter };
inline const char* whyName(unsigned w) noexcept {
    switch (w) {
    case WhyDerived: return "derived"; case WhySame: return "same-as-fallback"; case WhyTwice: return "second-call";
    case WhyShape: return "not-loader-argv0"; case WhyTooLong: return "too-long"; case WhyCharacter: return "character";
    default: return "not-called";
    }
}

// The section's bounds (every site buffer of the library, in link order).
#if defined(__APPLE__)
extern Site siteStart[] __asm("section$start$__DATA$__recomp_paths");
extern Site siteStop[] __asm("section$end$__DATA$__recomp_paths");
inline Site* sitesBegin() noexcept { return siteStart; }
inline Site* sitesEnd() noexcept { return siteStop; }
#else
} // namespace recomp::appdir
extern "C" {
[[gnu::weak, gnu::visibility("hidden")]] extern ::recomp::appdir::Site __start_recomp_app_paths[];
[[gnu::weak, gnu::visibility("hidden")]] extern ::recomp::appdir::Site __stop_recomp_app_paths[];
}
namespace recomp::appdir {
inline Site* sitesBegin() noexcept { return __start_recomp_app_paths; }
inline Site* sitesEnd() noexcept { return __stop_recomp_app_paths; }
#endif

struct State {
    std::atomic<unsigned> calls{0};
    std::atomic<unsigned> why{NotCalled};
    std::atomic<unsigned> rewritten{0}, foreign{0}; // site buffers rewritten / found not to hold a fall-back path (tests)
};
inline constinit State state;

// argv[0] = <files>/private-trace/run-<32 hex>/host_guest_trace (parseArguments' rule). Returns a Why; only the first call
// can change anything (WhyDerived: every site buffer now names <files>; WhySame: <files> is the fall-back, nothing changed).
inline unsigned setFromArgv0(const char* argv0) noexcept {
    static constexpr char Marker[] = "/private-trace/run-";
    static constexpr char Tail[] = "/host_guest_trace";
    unsigned why = WhyShape;
    const bool first = state.calls.fetch_add(1, std::memory_order_acq_rel) == 0;
    const char* at = argv0 && argv0[0] == '/' ? std::strstr(argv0, Marker) : nullptr;
    bool shape = at && at != argv0 && !std::strstr(at + 1, Marker);
    if (shape) {
        const char* id = at + sizeof Marker - 1;
        for (int i = 0; i < 32 && shape; ++i)
            shape = (id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f');
        shape = shape && std::strcmp(id + 32, Tail) == 0;
    }
    if (!first) why = WhyTwice;
    else if (shape) {
        const std::size_t n = static_cast<std::size_t>(at - argv0);
        bool plain = true; // (the directory becomes part of format strings: no '%'; parseArguments' character set)
        for (std::size_t i = 0; i < n; ++i) {
            const char c = argv0[i];
            plain = plain && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                              c == '-' || c == '.' || c == '/');
        }
        if (n >= Cap) why = WhyTooLong;
        else if (!plain) why = WhyCharacter;
        else if (n == FallbackLength && std::memcmp(argv0, Fallback, n) == 0) why = WhySame;
        else {
            unsigned rewritten = 0, foreign = 0;
            for (Site* s = sitesBegin(); s != sitesEnd(); ++s) {
                if (std::memcmp(s->path, Fallback, FallbackLength) != 0 || !std::memchr(s->path, 0, sizeof s->path)) {
                    ++foreign; // (not a site buffer as initialized: left alone)
                    continue;
                }
                char rel[RelCap];
                const std::size_t r = std::strlen(s->path + FallbackLength);
                if (r >= RelCap) { ++foreign; continue; }
                std::memcpy(rel, s->path + FallbackLength, r + 1);
                std::memcpy(s->path, argv0, n);
                std::memcpy(s->path + n, rel, r + 1);
                ++rewritten;
            }
            state.rewritten.store(rewritten, std::memory_order_relaxed);
            state.foreign.store(foreign, std::memory_order_relaxed);
            why = WhyDerived;
        }
    }
    if (first) state.why.store(why, std::memory_order_release);
    return why;
}
} // namespace recomp::appdir

// The path of one site: a constant address; its text is today's literal, or <files>/rel once setFromArgv0 derived <files>.
#define RECOMP_APP_PATH(rel_) \
    (static_cast<const char*>(::recomp::appdir::site<::recomp::appdir::Literal{RECOMP_APP_FALLBACK_DIR rel_}>.path))
// The files directory in effect (the bare-directory site).
#define RECOMP_APP_FILES \
    (static_cast<const char*>(::recomp::appdir::site<::recomp::appdir::Literal{RECOMP_APP_FALLBACK_DIR}>.path))
namespace recomp::appdir {
inline const char* files() noexcept { return RECOMP_APP_FILES; }
}

// Host tests that name their own directories (RECOMP_DIAG_SWITCH_DIR, RECOMP_WL_FILES_DIR: string literals) keep them; the
// files that use these macros define the device form in their own #ifndef blocks, which such a test skips.
#if defined(RECOMP_DIAG_SWITCH_DIR) && !defined(RECOMP_DIAG_SWITCH)
#define RECOMP_DIAG_SWITCH(name) (RECOMP_DIAG_SWITCH_DIR name)
#endif
#if defined(RECOMP_WL_FILES_DIR) && !defined(RECOMP_WL_PATH)
#define RECOMP_WL_PATH(rel) (RECOMP_WL_FILES_DIR rel)
#endif

#if defined(RECOMP_PRIVATE_TRACE_LIBRARY)
#if defined(__ANDROID__)
#include <android/log.h>
#endif
namespace recomp::appdir {
// privateTraceEntry's one call (trace_runner.cpp), after its single-entry guard: derive, then one line in the log
// (RENDER_APP_DIR derived=0|1 why=... sites=<buffers rewritten> files=<directory in effect>). Returns 0 to run, or
// RunSetupDirectory when argv[0] has the loader's shape but names a directory this library refuses (too long, or a
// character outside [A-Za-z0-9._-/]): the entry then returns it and the app shows REJECT_RUN_SETUP_4
// (PrivateTraceActivity.java:280; LaunchAdvice.refusal: "the game code stopped"), instead of a run on user 0's folder that
// another user cannot open. Not loader-shaped (the ABI check's "/private-abi-fixture") returns 0: the run goes on to its
// own argument checks, as in v474. Out of line: the entry gains one call and one test.
inline constexpr int RunSetupDirectory = 4; // (privateTraceEntry returns 2 and 3; runTraceMain 0 and 2)
[[gnu::noinline]] inline int fromLoader(const char* argv0) noexcept {
    const unsigned why = setFromArgv0(argv0);
#if defined(__ANDROID__)
    const bool derived = why == WhyDerived || why == WhySame;
    __android_log_print(derived ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, "RecompRenderer",
                        "RENDER_APP_DIR derived=%u why=%s sites=%u files=%s", unsigned(derived), whyName(why),
                        state.rewritten.load(std::memory_order_relaxed), files());
#endif
    return why == WhyTooLong || why == WhyCharacter ? RunSetupDirectory : 0;
}
}
#endif
