// loading-race (agent lane, base v459): the script render-target phase before the generic takeover.
//
// What stops v459 (replays lqntz0k2 on v398 and 1rzo_s_w on v459, see agents/loading-race/ROOT-CAUSE.md):
// CRenderPhaseScript2d's draw-list callback sub_82921898(count, entries) runs in the first game frame BEFORE the first
// tiled scene. For every live entry it locks a script render target (256x256 in both boots), clears it and draws the
// script's 2D items into it. The loading-screen inline path owns the presentation image only: its startup shader pair
// bound to another colour target cannot complete, and EndVertices stops the game with
// "Native EndVertices did not complete its real draw".
//
// The rule the port already follows is "take over where native stops" (v17 native-present-invalid, v167
// first-tiled-scene-unrecorded: the first tiled scene belongs to the generic renderer). This header applies it to the
// script render targets: when the callback is about to work on at least one target and the generic renderer has not
// taken over yet, it takes over there, so the lock, the clear, the draws and the resolve of that target all take the
// path they take in every later frame. With no live entry, after the takeover, or without diag-generic-first-scene (the
// configuration in which the first tiled scene hands over) nothing is done: the callback runs as in v459.
//
// UNOBSERVED (review, 2026-10-05): no log or profile on disk shows this port's generic renderer drawing a script render
// target. The hand-over replaces a certain stop with work that has not been seen on the phone; see DESIGN.md.
//
// Pure functions over a guest-word reader and callbacks, so the host test links this exact code.
#pragma once
#include <atomic>
#include <cstdint>
#include <optional>

namespace recomp::loading_handover {

inline constexpr char TakeoverReason[]="script-render-target-before-first-scene";
// sub_82921898's entries are 12 bytes; more than this many is not a list this code will walk (the callback runs as v459).
inline constexpr std::uint32_t MaxEntries=256;
// The hook logs its first calls (before and after the takeover): the callback has never been seen to run in an ordinary
// session, so the line is the only evidence that it ran and with what. One atomic load per call afterwards.
inline constexpr std::uint32_t NotedCalls=8;

struct LiveEntry {
    std::uint32_t index=0;  // the first entry the callback will work on
    std::uint32_t target=0; // its word +0 (the render-target holder the callback locks)
    std::uint32_t owner=0;  // its word +8 (the id the three draw routines are called with)
};
// The callback's own three tests, in its order (829218F0..8292192C): r3 = count, r4 = entries;
//   lwz r31,-4(r29) / beq next      entry word +0 is null
//   lwz r11,0(r29)  / beq next      entry word +4 is null
//   lwz r3,0(r11)   / beq next      the word that +4 points at is null
// An entry that passes them is locked, cleared and drawn into, unconditionally (82921930..829219E8).
// `load(address)` reads one big-endian guest word, as the callback does.
template<class Load> std::optional<LiveEntry> firstLiveEntry(std::uint32_t count,std::uint32_t entries,Load&& load) {
    if(!count || count>MaxEntries || !entries)return std::nullopt;
    for(std::uint32_t i=0;i<count;++i) {
        const std::uint32_t at=entries+12*i;
        const std::uint32_t target=load(at);
        if(!target)continue;
        const std::uint32_t holder=load(at+4);
        if(!holder)continue;
        if(!load(holder))continue;
        return LiveEntry{i,target,load(at+8)};
    }
    return std::nullopt;
}

// The decision at the callback's entry. `firstSceneGeneric` is called only when everything else says "take over".
template<class Load,class FirstSceneGeneric> std::optional<LiveEntry> handOverAt(bool takenOver,std::uint32_t count,
        std::uint32_t entries,Load&& load,FirstSceneGeneric&& firstSceneGeneric) {
    if(takenOver)return std::nullopt;
    const std::optional<LiveEntry> live=firstLiveEntry(count,entries,load);
    if(!live || !firstSceneGeneric())return std::nullopt;
    return live;
}

// The whole decision and its two effects, in the order the hook needs them: take over FIRST, then log. Returns whether it
// handed over. The caller runs the original callback after this returns, never before.
template<class Load,class FirstSceneGeneric,class TakeOver,class Log> bool handOver(bool takenOver,std::uint32_t count,
        std::uint32_t entries,Load&& load,FirstSceneGeneric&& firstSceneGeneric,TakeOver&& takeOver,Log&& log) {
    const std::optional<LiveEntry> live=handOverAt(takenOver,count,entries,load,firstSceneGeneric);
    if(!live)return false;
    takeOver(TakeoverReason);
    log(*live);
    return true;
}

// 1..NotedCalls for the first NotedCalls calls, then 0 (and the counter is no longer written). The callback runs on the
// render thread only; two racing callers could at worst print the same call number twice.
inline std::uint32_t notedCall(std::atomic<std::uint32_t>& calls) {
    const std::uint32_t n=calls.load(std::memory_order_relaxed);
    if(n>=NotedCalls)return 0;
    calls.store(n+1,std::memory_order_relaxed);
    return n+1;
}

}
