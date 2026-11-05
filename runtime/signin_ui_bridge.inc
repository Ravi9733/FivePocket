// signin-ui lane (2026-10-05, repaired after review the same day): __imp__XamShowSigninUI as on a console where the
// player opens the sign-in panel and closes it without signing in. Included at the end of
// generated_guest_import_bridge.cpp (boundRegistry, boundGuestBase and bridgeTrap are that file's). It replaces the
// guarded stop of remaining_xbox_import_guarded_stops.cpp. It is a new function and only calls the registry's public
// postNotification (a function nothing else calls), so no function with a PGO record changes.
//
// Evidence (the generated chunks the v465 build compiles; the full chain is in agents/ship-week/signin-ui/REPORT.md):
// - The only caller of the import thunk sub_8363AA28 (ppc_recomp.329.cpp:16783-16792, `b 0x837531bc`) is sub_83275D78
//   (validated-ppc/ppc_recomp.275.cpp:14706-14769). It calls XamShowSigninUI(dwPanes = 1, dwFlags = r4 & 2) only while
//   the "system UI showing" byte 0x83C00D61 is clear and returns (result == 0); every caller read discards that value.
// - That byte is written by sub_83231210 (ppc_recomp.269.cpp:44666-44763): from the game's system-area listener, whose
//   handle the game keeps at 0x83C00C30 (stored by sub_8322FCF8, ppc_recomp.269.cpp:41535), it dequeues ONE XN_SYS_UI
//   (id 9) notification per call and stores (parameter != 0).
// - sub_82FB97F8 (ppc_recomp.223.cpp:16557-16855) waits for the byte to clear after the call before it continues.
// Model: ERROR_SUCCESS (the panel opened), then XN_SYS_UI 1 (opened) and XN_SYS_UI 0 (closed) to every listener of the
// system area, both or neither per listener (postNotification's pair request). No XN_SYS_SIGNINCHANGED and no sign-in
// state change: the player did not sign in.
//
// Refused call path (stays the guarded stop): the call made from sub_8299EA88 (return address 0x8299ECCC,
// ppc_recomp.106.cpp:6226). That loop (106.cpp:6135-6246) only exits when some user is signed in to Xbox LIVE
// (XamUserGetSigninState == 2), which this runtime never reports; it has no cancel. Served, it would re-open the panel
// on every press forever (a soft-lock); the stop is kept there. Under v465 the loop is not entered (REPORT.md section 8).
//
// Switch (read once, at the first call): files/private-trace/diag-signin-ui-stop PRESENT = the old guarded stop, the
// same two calls as the removed stub (FIRST_STOP reason=5 detail=__imp__XamShowSigninUI). RECOMP_PRIVATE_TRACE_DIR
// replaces files/private-trace (used by the host tests; it is compiled into the Android build too, where it is unset).
// Markers, tag RecompRenderer (the launcher's logcat filter keeps RecompRenderer, RecompPrivateTrace, RecompNativeInput
// and RecompSaves only: tools/host_guest_trace/loading_workflow.py:148-150):
//   SIGNIN_UI_SERVED panes= flags= result=0 game_listener=queued|not-queued|absent caller=%08x n=   per served call
//   SIGNIN_UI_STOP reason=switch|live-required-loop panes= flags= caller=%08x                       before a stop
#include <android/log.h>
#include <unistd.h>
#include "recomp_app_dir.h" // data-paths: the app's files directory.
#include <cstdlib>
#include <string>

namespace signin_ui {
constexpr const char* DeviceSwitchDirectory = RECOMP_APP_PATH("/private-trace");
constexpr std::uint32_t GameListenerSlot = 0x83C00C30;  // the game's XN_SYS_UI listener handle (269.cpp:41535)
constexpr std::uint32_t LiveRequiredLoopReturn = 0x8299ECCC; // sub_8299EA88's call (106.cpp:6226)
}

void __imp__XamShowSigninUI(PPCContext& ctx, std::uint8_t* base) {
    static const bool stopSwitch = [] {
        const char* dir = std::getenv("RECOMP_PRIVATE_TRACE_DIR");
        const std::string path = std::string(dir ? dir : signin_ui::DeviceSwitchDirectory) + "/diag-signin-ui-stop";
        return ::access(path.c_str(), F_OK) == 0;
    }();
    static std::atomic<std::uint64_t> served{0};
    const recomp::bootstrap::GuestImportArguments arguments{ctx.r1.u64, {{
            ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64}}};
    const auto panes = ctx.r3.u32, flags = ctx.r4.u32;
    if (stopSwitch) {
        __android_log_print(ANDROID_LOG_INFO, "RecompRenderer",
                            "SIGNIN_UI_STOP reason=switch panes=%u flags=%u caller=00000000", panes, flags);
        recomp::bootstrap::GuestExecutionGuard::captureImportArguments(arguments);
        recomp::bootstrap::GuestExecutionGuard::stopForUnimplementedImport("__imp__XamShowSigninUI");
    }
    auto* registry = boundRegistry.load(std::memory_order_acquire);
    if (registry == nullptr || base == nullptr || base != boundGuestBase.load(std::memory_order_acquire)) {
        bridgeTrap();
    }
    // The game passes exactly these (sub_83275D78: li r3,1; r4 & 2). Anything else is outside the evidence: refuse it
    // the way the bridge refuses any import (reason 6).
    if (panes != 1 || (flags & ~2U) != 0) {
        constexpr std::string_view why{"Sign-in panel arguments outside the evidenced call (panes 1, flags 0 or 2)"};
        std::array<char, why.size() * 2 + 1> hex{};
        for (std::size_t i = 0; i < why.size(); ++i) {
            hex[i * 2] = "0123456789abcdef"[static_cast<unsigned char>(why[i]) >> 4];
            hex[i * 2 + 1] = "0123456789abcdef"[static_cast<unsigned char>(why[i]) & 15];
        }
        std::printf("PRIVATE_IMPORT_REJECT name=__imp__XamShowSigninUI state=%u diagnostic_hex=%s\n",
                    static_cast<unsigned>(recomp::bootstrap::GuestImportDispatchState::Rejected), hex.data());
        recomp::bootstrap::GuestExecutionGuard::captureImportArguments(arguments);
        recomp::bootstrap::GuestExecutionGuard::stopForRejectedImport("__imp__XamShowSigninUI");
    }
    const auto load = [&](std::uint32_t at) {
        const auto* p = base + at;
        return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
    };
    // The caller's return address, when the import runs in sub_83275D78's 96-byte frame (back chain at r1 = r1 + 96, the
    // saved LR of its caller at r1 + 88): which code asked. 0 when the frame does not have that shape.
    const auto sp = ctx.r1.u32;
    const auto caller = sp != 0 && sp <= 0xFFFFFF00U && load(sp) == sp + 96 ? load(sp + 88) : 0U;
    if (caller == signin_ui::LiveRequiredLoopReturn) {
        __android_log_print(ANDROID_LOG_INFO, "RecompRenderer",
                            "SIGNIN_UI_STOP reason=live-required-loop panes=%u flags=%u caller=%08x", panes, flags, caller);
        recomp::bootstrap::GuestExecutionGuard::captureImportArguments(arguments);
        recomp::bootstrap::GuestExecutionGuard::stopForUnimplementedImport("__imp__XamShowSigninUI");
    }
    if (auto* hook = recomp::bootstrap::guestImportEntryHook.load(std::memory_order_acquire)) hook(); // v211, as every bridged import
    // XN_SYS_UI opened, then closed, to every system-area listener; the result reported is the game's own listener's.
    const auto gameListener = load(signin_ui::GameListenerSlot);
    const bool queued = registry->postNotification(0x80000000U | 0x00000009U, gameListener);
    __android_log_print(ANDROID_LOG_INFO, "RecompRenderer",
                        "SIGNIN_UI_SERVED panes=%u flags=%u result=0 game_listener=%s caller=%08x n=%llu", panes, flags,
                        gameListener == 0 ? "absent" : queued ? "queued" : "not-queued", caller,
                        static_cast<unsigned long long>(served.fetch_add(1, std::memory_order_relaxed) + 1));
    ctx.r3.u64 = 0; // ERROR_SUCCESS: the panel was shown
}
