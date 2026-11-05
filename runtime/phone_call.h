#pragma once
// v852 (bug-fix, 2026-10-25; owner: "make the answer/decline touch work on the duo/thor"). The phone's call state, shared by every
// edition (the ABI the Fold / Duo apps mirror: recomp_phone_state_v1). Pure: no guest memory, no platform calls (the host test runs it).
//  - RINGING = the game's own IS_PED_RINGTONE_PLAYING(PLAYER_PED_ID()) (joaat fe576ee4, handler sub_82D6CA70: one argument, the ped,
//    -> sub_82D6AB80, a BOOL result). IN CALL = IS_MOBILE_PHONE_CALL_ONGOING() (joaat 4ed1400a, sub_82D54080: no argument, BOOL).
//    Polled once per game frame at the trainer's safe point (trainer_glue.inc: after the script VM's PLAYER_PED_ID, the script thread).
//  - PHONE UP = the renderer's phone tracker (the phone screen quad, ps 325cb5da85796d1b, seen within 8 presents; wheel_replay.h).
//  - recomp_phone_state_v1() = Up | Ringing | InCall (bits 0, 1, 2). The off switch files/private-trace/diag-phone-call-off stops the
//    poll (ringing / in-call read 0; the tap goes back to v828's plan), looked for every SwitchFrames game frames.
//  - Touch while ringing or in a call (phone_touch.h plan, call=true): the phone screen's right part (display s >= BackShareX, the
//    soft-key bar's right slot = the phone's Back key, the red one) = B (decline / hang up); anywhere else on the screen = A (the
//    select key: answer). No D-pad steps. The buttons are the phone's own select / back keys (v815 / v828 device-tested).
#include <atomic>
#include <cstdint>

namespace recomp::phonecall {
enum Bits : unsigned {Up=1u,Ringing=2u,InCall=4u};
constexpr std::uint32_t joaat(const char* s) noexcept {
    std::uint32_t h=0;
    for(;*s;++s){unsigned char c=static_cast<unsigned char>(*s);if(c>='A' && c<='Z')c=static_cast<unsigned char>(c+32);
        h+=c;h+=h<<10;h^=h>>6;}
    h+=h<<3;h^=h>>11;h+=h<<15;return h;
}
constexpr std::uint32_t PlayerPedId=joaat("PLAYER_PED_ID"),IsPedRingtonePlaying=joaat("IS_PED_RINGTONE_PLAYING"),
    IsMobilePhoneCallOngoing=joaat("IS_MOBILE_PHONE_CALL_ONGOING");
static_assert(PlayerPedId==0xfa92e226u && IsPedRingtonePlaying==0xfe576ee4u && IsMobilePhoneCallOngoing==0x4ed1400au);
inline constexpr std::uint32_t UsedNatives[]{IsPedRingtonePlaying,IsMobilePhoneCallOngoing}; // (PLAYER_PED_ID: the trainer's list)
constexpr unsigned SwitchFrames=256;

inline std::atomic<unsigned>& stateWord() noexcept {static std::atomic<unsigned> s{0};return s;}
inline unsigned state() noexcept {return stateWord().load(std::memory_order_relaxed);}
inline bool callActive(unsigned s) noexcept {return (s&(Ringing|InCall))!=0;}
inline void setUp(bool up) noexcept {
    if(up)stateWord().fetch_or(Up,std::memory_order_relaxed);else stateWord().fetch_and(~unsigned(Up),std::memory_order_relaxed);
}
// The poll's result: the call bits replaced (Up kept). Returns the previous call bits.
inline unsigned setCall(bool ringing,bool inCall) noexcept {
    const unsigned want=(ringing?unsigned(Ringing):0u)|(inCall?unsigned(InCall):0u);
    unsigned cur=stateWord().load(std::memory_order_relaxed);
    while(!stateWord().compare_exchange_weak(cur,(cur&Up)|want,std::memory_order_relaxed)){}
    return cur&(Ringing|InCall);
}

// One poll through a natives object with call(hash, args, n, out) -> bool (trainer.h Natives; mocks in the test). False results and
// unavailable natives read as "not ringing" / "not in a call". Returns the new call bits.
template<class N> unsigned poll(N& natives) {
    std::uint32_t out[3]{};
    bool ringing=false,inCall=false;
    if(natives.call(PlayerPedId,nullptr,0,out) && out[0]) {
        const std::uint32_t ped=out[0];
        if(natives.call(IsPedRingtonePlaying,&ped,1,out))ringing=(out[0]&0xFFu)!=0;
    }
    if(natives.call(IsMobilePhoneCallOngoing,nullptr,0,out))inCall=(out[0]&0xFFu)!=0;
    setCall(ringing,inCall);
    return (ringing?unsigned(Ringing):0u)|(inCall?unsigned(InCall):0u);
}
} // namespace recomp::phonecall
