// play-config-2 (v527; play_build.h): the play build's switch-off of the registry's hold accounting. Included once, from
// guest_registry_concurrency.inc, on the blank line after registryAccount's definition (no include guard; nothing at all
// without -DRECOMP_PLAY_BUILD=1).
// With the define, registryAccount(...) below this line is no call: RegistrySection::publishHold, its one caller, then does
// nothing, and the per-thread batch table (ten values per section, flushed to six shared atomics every 64 sections) is
// never written. Only RENDER_DIAG_REGISTRY_HOLD / _HOLD_TOTAL / _WAIT / _BLOCKER read that table, and the registry's log
// block is never due in the play build (registryMaybeLog). The arguments are plain members of the section: not
// evaluating them changes nothing. The call site keeps its two source lines and columns, so guest_import_registry.cpp
// without the define is v524's object byte for byte.
// What stays, for the profile records' sake: the two counter reads of noteAcquired / noteReleasedInterval and the
// branches behind them. v483 compiled `acquiredAt = registryTicks()` out; registryRelock, which inlines noteAcquired,
// then ended on a branch, its blocks merged, and its record (12 M counts) no longer matched.
#if defined(RECOMP_PLAY_BUILD) && RECOMP_PLAY_BUILD
#define registryAccount(...) ((void)0)
#endif
