#pragma once
// play-config-2 (attempt v527; the approach of lane play-config's v483, redone on v524's sources): the play configuration.
// -DRECOMP_PLAY_BUILD=1 on a translation unit compiles out work that exists only to count, time or log, on the paths that
// run once or more per frame, per draw, per import call or per submission. The list, with the rate of every item on the
// Fold's street scene and what reads it, is local-analysis/agents/play-config-2/inventory.tsv.
//
// This header holds macros only (it may be included anywhere, also inside a namespace, as guest_timebase.cpp does); the
// play build's declarations are in play_log.h, which diag_log_sample.h includes.
//
// Without the define nothing here produces code: RECOMP_DIAG_ONLY(x) expands to exactly x, RECOMP_PLAY_ONLY(x) to
// nothing, RECOMP_PLAY_GLOG(...) to the call GLOG has always been, RECOMP_PLAY_COUNTER(T) to std::atomic<T>, and play_log.h,
// play_executor_log.h and play_registry_account.h are empty. Each unit then preprocesses to v524's code; the headers
// are included from lines that were comment lines or blank, so no line moves and every __LINE__-keyed lock site keeps
// its number, and no existing token changes its column (the lane's tests/identity.py: the objects are v524's byte for byte).
//
// With the define three things happen, and only these:
//  1. Counting stops where only diagnostics read the count. A counter declared with RECOMP_PLAY_COUNTER is a std::atomic
//     whose fetch_add does nothing (the statements that count stay as they are, so their lines and columns are v524's);
//     registryAccount(...) is no call (play_registry_account.h); a statement wrapped in RECOMP_DIAG_ONLY is gone (the
//     time base's read count, the registry block's interval). Each of these is straight-line work: no branch, loop or
//     select is removed, so the functions keep their control flow and with it their IR profile records (PGO). None writes
//     guest memory, none is read by a decision of the runtime (the lane's tests/static_check.py holds the list of
//     counters and of the lines that may read them, and refuses anything else).
//  2. The line types named in recomp::diag::PlayMutedLines (play_log.h) go through recomp::diag::quietLog instead of
//     __android_log_print (RECOMP_PLAY_GLOG below; play_executor_log.h for executorLog). quietLog is the out-of-line gate
//     lane quiet-log built (diag_log_sample.h): one relaxed load, then nothing while `quiet` is set. The call keeps the
//     shape it had (a call stays a call, an invoke an invoke), so the caller's control flow and profile record are
//     unchanged; the arguments are still evaluated, the formatting, the write to the log socket and the log daemon's work
//     are not done.
//  3. `quiet` starts true and stays true: the play build is quiet-log's policy as the compiled default, plus the types of
//     PlayMutedLines. files/private-trace/diag-play-loud turns every one of these lines back on at the next 60-present
//     window (RENDER_GENERIC_V210Z play_loud=, RENDER_DIAG_PLAY): the part of the change that can be measured inside one
//     run. What it cannot bring back is item 1 (the counts read 0) and the registry's 5 s block (never due).
//
// A line type is muted by its first word, at compile time: playMuted("RENDER_DIAG_V361 on=%u") is a constant expression,
// and the conditional operator of the route folds to one arm in the front end (no branch is emitted). A format that is
// not a literal would make the route a run-time test: tests/static_check.py refuses a GLOG whose format is not a literal.
#if defined(RECOMP_PLAY_BUILD) && RECOMP_PLAY_BUILD
#define RECOMP_DIAG_ONLY(...)
#define RECOMP_PLAY_ONLY(...) __VA_ARGS__
#define RECOMP_PLAY_QUIET_AT_START true
// generic_renderer.cpp's GLOG (that unit includes diag_log_sample.h, and with it play_log.h, before its first GLOG).
#define RECOMP_PLAY_GLOG(format, ...) (::recomp::diag::playMuted(format) \
    ? ::recomp::diag::quietLog(ANDROID_LOG_INFO,"RecompRenderer",format __VA_OPT__(,) __VA_ARGS__) \
    : __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",format __VA_OPT__(,) __VA_ARGS__))
// An argument of a muted line that costs work to make: the real object while diag-play-loud is on, an empty one otherwise
// (play_log.h playWhileLoud: out of line, a call only at the site).
#define RECOMP_PLAY_WHILE_LOUD(pointer) ::recomp::diag::playWhileLoud(pointer)
// The type of a counter that only diagnostics read (play_log.h PlayCounter: a std::atomic whose fetch_add does nothing).
// RECOMP_PLAY_COUNTER_UINT64 is as long as the text it stands for, std::atomic<std::uint64_t>: a member declared with it
// keeps its columns, and with them the debug line rows of the constructor that initialises it.
#define RECOMP_PLAY_COUNTER(type) ::recomp::diag::PlayCounter<type>
#define RECOMP_PLAY_COUNTER_UINT64 ::recomp::diag::PlayCounter<std::uint64_t>
#else
#define RECOMP_DIAG_ONLY(...) __VA_ARGS__
#define RECOMP_PLAY_ONLY(...)
#define RECOMP_PLAY_QUIET_AT_START false
#define RECOMP_PLAY_WHILE_LOUD(pointer) pointer
#define RECOMP_PLAY_COUNTER(type) std::atomic<type>
#define RECOMP_PLAY_COUNTER_UINT64 std::atomic<std::uint64_t>
#define RECOMP_PLAY_GLOG(...) __android_log_print(ANDROID_LOG_INFO,"RecompRenderer",__VA_ARGS__)
#endif
