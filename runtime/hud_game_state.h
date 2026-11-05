#pragma once
// hud3 (v585): the player's health, armour and wanted level for the second screen's own HUD, read straight from guest memory
// (read-only, relaxed aligned loads; no guest code runs, nothing is written). Every address and offset below comes from our
// own recompiled code (the validated-ppc asm comments of the native handlers, found through the registration calls to
// sub_83438FE8 in ppc_recomp.*.cpp), never from game source:
//   player_ped_id fa92e226 -> sub_82DB2118: ped = *( *(0x8394B8B0) + 4 ) (0 while there is none). get_player_wanted_level
//     bdcdd163 -> sub_82DB10D8 -> sub_8234CE38(player 0, 1): the same word in single player.
//   get_entity_health 8e3222b7 -> sub_82D77520 -> sub_82D71078: float at entity + 192 (after the handle lookup and IsA check).
//   get_entity_max_health c7ae6aa1 -> sub_82D77560 -> sub_82D710F8: float at entity + 384.
//   get_ped_armour 2ce311a7 -> sub_82DD1D48 -> sub_82DC4978: float at ped + 3588.
//   get_player_wanted_level: info = *(ped + 2912) (CPlayerInfo), level = *(info + 1296 + 144).
//   entity type byte +20: 4 = ped (bot_vision_scan.h, is_entity_a_ped sub_82D71D40).
// A torn or stale read (the game thread changes these while the renderer reads) only gives one frame a wrong value.
// Special ability: no getter native for the meter exists in the registration table, so it is not shown.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace recomp::gpu::hud {
constexpr std::uint32_t PlayerSlotGlobal=0x8394B8B0u,PlayerSlotPed=4,EntityType=20,EntityHealth=192,EntityMaxHealth=384,PedArmour=3588,
    PedPlayerInfo=2912,InfoWantedLevel=1296+144;
constexpr std::uint8_t TypePed=4;
constexpr float ArmourMax=100.0f; // (get_player_max_armour's default; armour above it fills the bar)

// The guest memory base, set once by trace_runner.cpp after the address space is reserved (null: never read).
inline std::atomic<const std::uint8_t*>& guestBase() noexcept {static std::atomic<const std::uint8_t*> b{nullptr};return b;}

inline bool guestPointer(std::uint32_t a,std::uint32_t align) noexcept {return a>=0x10000u && a<=0xFFFF0000u && !(a&(align-1));}

// Big-endian guest words through relaxed loads (the whole 4 GB range is mapped read/write: guest_address_space.cpp).
struct GuestMemory {
    const std::uint8_t* base=nullptr;
    std::uint32_t u32(std::uint32_t a) const noexcept {
        if(!guestPointer(a,4))return 0;
        return __builtin_bswap32(__atomic_load_n(reinterpret_cast<const std::uint32_t*>(base+a),__ATOMIC_RELAXED));
    }
    std::uint8_t u8(std::uint32_t a) const noexcept {return guestPointer(a,1)?__atomic_load_n(base+a,__ATOMIC_RELAXED):0;}
    float f32(std::uint32_t a) const noexcept {const auto w=u32(a);float v;std::memcpy(&v,&w,4);return v;}
};

struct GameState {
    bool ped=false;          // a player ped was found (false while loading)
    float health=0,healthMax=0,armour=0;
    int wanted=-1;           // 0..5; -1 = unknown (no player info)
};
inline bool sane(float v,float hi) noexcept {return std::isfinite(v) && v>=0.0f && v<=hi;}
// M: anything with u32 / u8 / f32 (GuestMemory on the device, a fake in the host tests).
template<class M> GameState readGameState(const M& m) noexcept {
    GameState s;
    const std::uint32_t slot=m.u32(PlayerSlotGlobal);
    if(!guestPointer(slot,4))return s;
    const std::uint32_t ped=m.u32(slot+PlayerSlotPed);
    if(!guestPointer(ped,4) || m.u8(ped+EntityType)!=TypePed)return s;
    const float hp=m.f32(ped+EntityHealth),hpMax=m.f32(ped+EntityMaxHealth),armour=m.f32(ped+PedArmour);
    if(!sane(hp,100000.0f) || !sane(hpMax,100000.0f) || hpMax<=0.0f)return s;
    s.ped=true;s.health=hp;s.healthMax=hpMax;s.armour=sane(armour,100000.0f)?armour:0.0f;
    const std::uint32_t info=m.u32(ped+PedPlayerInfo);
    if(guestPointer(info,4)) {
        const std::uint32_t w=m.u32(info+InfoWantedLevel);
        s.wanted=w<=5u?int(w):-1;
    }
    return s;
}
// thor-char (v685): whether the game runs a player SWITCH (the character wheel's sky-camera transition). From our own recompiled code:
// native is_player_switch_in_progress (0x56135acc, registered in ppc_recomp.197.cpp at 0x82E09F64.. -> handler sub_82DB6558) calls
// sub_823951D8(0x837D05E0): t = u32 at the object; p = t==3 ? object+384 : object+16; the result is the byte at p+16. The next
// native (get_player_switch_type 0x280dc015, sub_82DB6598) returns that first word. Read-only, like the bars' reads.
constexpr std::uint32_t SwitchObject=0x837D05E0u,SwitchTypeLong=3,SwitchStateA=16,SwitchStateB=384,SwitchActive=16;
template<class M> bool switchInProgress(const M& m) noexcept {
    const std::uint32_t t=m.u32(SwitchObject);
    return m.u8(SwitchObject+(t==SwitchTypeLong?SwitchStateB:SwitchStateA)+SwitchActive)!=0;
}
// RC1.1 (thor-radio, owner 2026-10-24: "when dying or busted the loading screen with the spinner should be active"): the player's
// death and arrest, from our own recompiled natives (touch-controls-spec/tools/native_table.py, fn_asm.py):
//   IS_PLAYER_DEAD 140ca5a8 -> sub_82DB0FF0: the ped (sub_8234CE38(player, 1)), then u32 (u32 (ped + 2912) + 252) == 1.
//   IS_PLAYER_BEING_ARRESTED 7f6a60d3 -> sub_82DB1DF0 -> sub_822534E0(player, flag): with flag 0 (and as the fallback with flag 1, where a
//   task test comes first) (u32 (ped + 2900) & 0x00E00000) == 0x00200000 (rlwinm 0,8,10; lis 32).
// The ped is the player slot's (as readGameState). Read-only, like the bars.
constexpr std::uint32_t InfoPlayerState=252,PlayerStateDead=1,PedArrestWord=2900,ArrestMask=0x00E00000u,ArrestValue=0x00200000u;
struct PlayerFate {bool ped=false,dead=false,arrested=false;};
template<class M> PlayerFate readPlayerFate(const M& m) noexcept {
    PlayerFate f;
    const std::uint32_t slot=m.u32(PlayerSlotGlobal);
    if(!guestPointer(slot,4))return f;
    const std::uint32_t ped=m.u32(slot+PlayerSlotPed);
    if(!guestPointer(ped,4) || m.u8(ped+EntityType)!=TypePed)return f;
    f.ped=true;
    const std::uint32_t info=m.u32(ped+PedPlayerInfo);
    f.dead=guestPointer(info,4) && m.u32(info+InfoPlayerState)==PlayerStateDead;
    f.arrested=(m.u32(ped+PedArrestWord)&ArrestMask)==ArrestValue;
    return f;
}
// v817 (thor-radio, mission text on the lower screen): whether the game shows its help box / a printed objective, from our own
// recompiled natives: IS_HELP_MESSAGE_BEING_DISPLAYED a65f262a -> sub_82D619F8 -> sub_82268730(0): u8 0x83A7DAB8 (+ 600 * slot) != 0
// (lis -31832, addi -9544); IS_MESSAGE_BEING_DISPLAYED 6a77fe8d -> sub_82D61060: u32 0x83A7EAB0 (0x83A7EA50 + 96) != 0.
constexpr std::uint32_t HelpShown=0x83A7DAB8u,MessageShown=0x83A7EAB0u;
struct TextState {bool help=false,message=false;};
template<class M> TextState readTextState(const M& m) noexcept {return {m.u8(HelpShown)!=0,m.u32(MessageShown)!=0};}
// What the bars show (0..1). Health: the player is dead at 100 of the game's points, so the bar spans 100..max (the usual
// 200 for the three protagonists); a maximum at or below 100 spans 0..max.
inline float healthFraction(const GameState& s) noexcept {
    if(!s.ped || !(s.healthMax>0.0f))return 0.0f;
    const float lo=s.healthMax>100.0f?100.0f:0.0f;
    const float f=(s.health-lo)/(s.healthMax-lo);
    return std::isfinite(f)?std::fmin(1.0f,std::fmax(0.0f,f)):0.0f;
}
inline float armourFraction(const GameState& s) noexcept {
    if(!s.ped)return 0.0f;
    const float f=s.armour/ArmourMax;
    return std::isfinite(f)?std::fmin(1.0f,std::fmax(0.0f,f)):0.0f;
}
}
