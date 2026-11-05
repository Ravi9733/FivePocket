// play-config-2 (v527; play_build.h): the play build's muted line types and its log switch. Included once, from
// diag_log_sample.h, on the blank line after quietSkip(), that is INSIDE namespace recomp::diag (no include guard;
// nothing at all without -DRECOMP_PLAY_BUILD=1, so diag_log_sample.h is v524's then).
//
// With the define:
//  - `quiet` (diag_log_sample.h) starts true: from the first instruction the play build writes none of the quiet-gated
//    line types and every 16th of the sampled ones, as diag-quiet-log does in a development build. Lines that report a
//    failure, a refusal or a stop were never gated and are not gated here.
//  - files/private-trace/diag-play-loud, polled where diag-quiet-log is polled (quietLogWindow, the renderer's 60-present
//    window), turns the gate off for the next window: the build then logs what v524 logs, except the counts the play
//    build does not keep (they read 0) and the registry's 5 s block (its timer is never due). diag-quiet-log
//    keeps its meaning while loud. Before the first 60 presents of a run the build is quiet whatever the files say.
//  - Two lines per window, written whatever the state:
//      RENDER_GENERIC_V210Z play_loud=N     1 = the file was present at both ends of the window that just ended
//                                           (the convention of every V210Z switch: ab_windows.py / ab_joint.py)
//      RENDER_DIAG_PLAY build=1 loud=N stable=N gated=N
//                                           loud: the state for the window that starts now; stable: 1 = the file's
//                                           presence did not change across the window that just ended (cut a run by
//                                           it); gated: quiet-gated log calls that found the gate closed in that window
//                                           (the lines not written, plus the surviving every-16th sampled ones).
//    RENDER_GENERIC_V210Z quiet_log= (quietLogWindow's own line) then reads 1 for a window the gate was closed at both
//    ends of, for either reason.
// quietSkip() is replaced, for the code below this include, by a version that also counts: the same relaxed load, then a
// relaxed add of 0 or 1 (no branch: the gates' control flow, and their profile records, are v524's).
#if defined(RECOMP_PLAY_BUILD) && RECOMP_PLAY_BUILD
// Line types (the format's first word) that the play build does not write unless diag-play-loud is present. Every entry
// is a per-frame line or one of the per-window / per-10-second diagnostic lines that no measuring tool reads (the readers
// were searched for in tools/iter, tools/host_guest_trace and the Fold harness: the lane's tools/readers.py). Never here:
// RENDER_GENERIC_TIMING, RENDER_GENERIC_V210 / V210X / V210Y / V210Z, RENDER_DIAG_PACE, RENDER_DIAG_PACE_FR, RENDER_DIRECT_*,
// RENDER_DIAG_TASK_HOST, RENDER_DIAG_FRAME_HIST, RENDER_GENERIC_ASYNC, RENDER_GENERIC_FRAME, any line that reports a
// failure, a refusal or a stop, and any line whose format ends with a capability marker (recomp-cap:...).
inline constexpr const char* const PlayMutedLines[] = {
    // generic_renderer.cpp, the graphics worker: once per frame while frame generation is on (fg_gen_renderer.inc)
    "RENDER_POSTFX",
    // generic_renderer.cpp, the 60-present window of the graphics worker: one line each per window
    "RENDER_DIAG_AU", "RENDER_DIAG_MW_CS", "RENDER_DIAG_OCQ", "RENDER_GENERIC_VFETCH_INDEX", "RENDER_DIAG_AW", "RENDER_DIAG_MS",
    "RENDER_DIAG_MW2", "RENDER_DIAG_SW", "RENDER_DIAG_PX", "RENDER_DIAG_WF", "RENDER_DIAG_VKW", "RENDER_DIAG_VKW2",
    "RENDER_DIAG_VKW3", "RENDER_DIAG_VKW4", "RENDER_DIAG_SCALE", "RENDER_DIAG_GC", "RENDER_GENERIC_V260",
    "RENDER_GENERIC_V73_GATHER", "RENDER_GENERIC_V113", "RENDER_GENERIC_V123", "RENDER_GENERIC_V204", "RENDER_DIAG_V266_POLL",
    "RENDER_DIAG_V209", "RENDER_GENERIC_WRITEBACK_STATS", "RENDER_DIAG_V294_MODULES", "RENDER_DIAG_V293", "RENDER_DIAG_V338",
    "RENDER_DIAG_V341", "RENDER_DIAG_V361", "RENDER_DIAG_V369", "RENDER_DIAG_V343", "RENDER_DIAG_V342", "RENDER_DIAG_V363",
    "RENDER_DIAG_V376", "RENDER_DIAG_DF", "RENDER_DIAG_V382", "RENDER_DIAG_V301", "RENDER_DIAG_V310", "RENDER_DIAG_V303_PENDING",
    "RENDER_DIAG_V291", "RENDER_GENERIC_UI", "RENDER_GENERIC_BLEND_FLOAT_DRAWS", "RENDER_GENERIC_SHADOW",
    "RENDER_GENERIC_V85_DRAWS", "RENDER_GENERIC_VERTEX_ADAPT", "RENDER_GENERIC_V78", "RENDER_SE", "RENDER_GENERIC_V74",
    "RENDER_GENERIC_RESOURCES",
    // generic_renderer.cpp, the 60-present window of the drawing thread (afterPresent)
    "RENDER_DIAG_V321", "RENDER_DIAG_V295", "RENDER_GENERIC_V177", "RENDER_DIAG_V366",
    // guest_cooperative_executor.cpp, the supervisor thread's block every 10 s
    "RENDER_DIAG_EXEC", "RENDER_DIAG_EXEC_RUNNING", "RENDER_DIAG_EXEC_GATE", "RENDER_DIAG_EXEC_YIELD", "RENDER_DIAG_EXEC_PERMIT",
    "RENDER_DIAG_EXCL_SITE", "RENDER_DIAG_V224_STOPS", "RENDER_DIAG_V224_LAST_STOPPER", "RENDER_DIAG_EXCL_NESTED",
    "RENDER_DIAG_EXEC_SCHED_TOTAL",
};
// Does `format` begin with the whole word `name`? (RENDER_DIAG_V361 does not mute RENDER_DIAG_V361_INIT.)
constexpr bool playSameLine(const char* format, const char* name) noexcept {
    while (*name) if (*format++ != *name++) return false;
    return *format == ' ' || *format == 0;
}
constexpr bool playMuted(const char* format) noexcept {
    for (const char* name : PlayMutedLines) if (playSameLine(format, name)) return true;
    return false;
}
// RECOMP_PLAY_COUNTER (play_build.h): a counter that only log lines and end-of-run statistics read. It is a std::atomic of
// the same size and layout, so everything that reads it (load, the log lambdas that take a std::atomic reference) works
// unchanged and reads 0; fetch_add, the one operation the counted paths use, does nothing. No decision may read such a
// counter: tests/static_check.py lists every one and the only lines allowed to read it.
template <class T> struct PlayCounter : std::atomic<T> {
    using std::atomic<T>::atomic;
    using std::atomic<T>::operator=;
    T fetch_add(T, std::memory_order = std::memory_order_seq_cst) noexcept { return T{}; }
};
static_assert(sizeof(PlayCounter<unsigned long long>) == sizeof(std::atomic<unsigned long long>) &&
              alignof(PlayCounter<unsigned long long>) == alignof(std::atomic<unsigned long long>));
inline std::atomic<unsigned long long> playGatedCalls{0};
inline bool playQuietSkip() noexcept {
    const bool closed = quiet.load(std::memory_order_relaxed);
    playGatedCalls.fetch_add(static_cast<unsigned long long>(closed), std::memory_order_relaxed);
    return closed;
}
#define quietSkip playQuietSkip
// For an argument of a muted line that costs work to make (RECOMP_PLAY_WHILE_LOUD, play_build.h): `real` while the gate is
// open, otherwise one empty object of its type, so that the code that formats it finds nothing to format. Out of line and
// noexcept: the calling function gains a call and no branch. Today's one user is the renderer's RENDER_POSTFX line, whose
// text of up to 14 constant registers is otherwise built for every frame while frame generation is on.
template <class T> [[gnu::noinline]] const T* playWhileLoud(const T* real) noexcept {
    static constinit const T none{};
    return quiet.load(std::memory_order_relaxed) ? &none : real;
}
[[gnu::format(printf, 3, 4)]] inline void quietLogAlways(int priority, const char* tag, const char* format, ...) noexcept; // (defined below this include)
#if defined(RECOMP_PLAY_LOG_TEST_FILE) // host test: the switch file's presence
bool RECOMP_PLAY_LOG_TEST_FILE() noexcept;
#endif
// quietLogWindow's play part: the switch for the next window and the two lines. True: stay quiet.
[[gnu::noinline]] inline bool playWindowQuiet() noexcept {
    static bool loud = false;
    static unsigned long long counted = 0;
    const bool window = loud;
#if defined(RECOMP_PLAY_LOG_TEST_FILE)
    loud = RECOMP_PLAY_LOG_TEST_FILE();
#else
    loud = ::access(RECOMP_APP_PATH("/private-trace/diag-play-loud"), F_OK) == 0;
#endif
    const auto now = playGatedCalls.load(std::memory_order_relaxed);
    quietLogAlways(4 /* ANDROID_LOG_INFO */, "RecompRenderer", "RENDER_GENERIC_V210Z play_loud=%u", unsigned(window & loud));
    quietLogAlways(4 /* ANDROID_LOG_INFO */, "RecompRenderer", "RENDER_DIAG_PLAY build=1 loud=%u stable=%u gated=%llu",
                   unsigned(loud), unsigned(window == loud), now - counted);
    counted = now;
    return !loud;
}
#endif
