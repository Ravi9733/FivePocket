// play-config-2 (v527; play_build.h): the play build's route for guest_cooperative_executor.cpp's executorLog. Included
// once, from the blank line after executorLogQuiet's definition, so that both functions are declared and neither
// definition is touched. No include guard and nothing at all without -DRECOMP_PLAY_BUILD=1.
// With the define, a call executorLog("RENDER_DIAG_EXEC_PERMIT ...", ...) whose line type is in recomp::diag::PlayMutedLines (play_log.h)
// (the supervisor thread's block every 10 s: about 45 lines for the permits alone) becomes a call of executorLogQuiet,
// which lane quiet-log made for the high-rate types: out of line, noexcept like executorLog, one relaxed load and nothing
// more while the play build is quiet. Every other executorLog call (the freeze dump, the stall watchdog's native stacks,
// stops, the once-per-thread RENDER_DIAG_TASK_HOST, and executorLog("%s", line)) is the call it was: a function-like macro
// is not expanded inside its own replacement, so the second arm names the function. The condition is a constant
// expression of the format literal and the front end emits one arm only: no branch, the callers' control flow as in v524.
#if defined(RECOMP_PLAY_BUILD) && RECOMP_PLAY_BUILD
#define executorLog(format, ...) (::recomp::diag::playMuted(format) \
    ? executorLogQuiet(format __VA_OPT__(,) __VA_ARGS__) : executorLog(format __VA_OPT__(,) __VA_ARGS__))
#endif
