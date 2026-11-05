#pragma once
// qol-touch (v702, 2026-10-24; spec: local-analysis/touch-controls-spec/SPEC.md section 1): where the player is - on foot or in which kind of
// vehicle - for the app's context touch controls (owner: "custom touch controls for each action like a normal phone game that dynamically
// changes when in a vehichle or aircraft"). Read straight from guest memory (read-only, relaxed aligned big-endian loads, no guest code),
// as hud_game_state.h reads the HUD bars. Every offset comes from our own recompiled code (script-native handlers found through their
// registrations, sub_83438FE8(joaat(name), handler); touch-controls-spec/tools/native_table.py, fn_asm.py):
//   IS_PED_IN_ANY_VEHICLE 3b0171ee -> sub_82DD12F8 -> sub_82460EF0: in = bit 30 of u32 ped+3444 (rlwinm 2,31,31) and u32 ped+3620 != 0.
//   IS_PED_IN_ANY_PLANE / HELI / ON_ANY_BIKE / BOAT / TRAIN / SUB -> the same two tests, then u32 vehicle+1604 == 1 / 5..6 / 8..9 / 10 /
//   11 / 12; IS_PED_IN_FLYING_VEHICLE: 1 or 5..7. IS_THIS_MODEL_A_CAR / QUADBIKE / BICYCLE / HELI: model info +440 == 0 / 3 / 9 / 5 (the
//   same enum). Player ped: hud_game_state.h (u32 0x8394B8B0, +4; entity type byte +20 == 4).
// Not here (task-tree calls, not fields): swimming, parachuting.
// v704 (owner 2026-10-24: "the stick needs to disable when on any screens such as radio, character switch, weapon wheel etc."; SPEC 3.8):
// the screens that take the sticks, from the same handlers:
//   IS_HUD_COMPONENT_ACTIVE 6214631f -> sub_82D62F98 -> sub_82480768: bit 7 of u8 0x83899158 + 112 * id (id 18 alone is answered
//   elsewhere). Ids as the game's HUD component list: 19 the weapon wheel, 16 the radio stations (the radio wheel).
//   IS_PAUSE_MENU_ACTIVE d3600591 -> sub_82D63610 -> sub_8243FE18(1): first u8 0x83895CB5 or u8 0x83895CA4 != 0 (the frontend is up;
//   the rest of that function only narrows it, so this reads "a frontend menu is on screen").
//   IS_CUTSCENE_ACTIVE cce2fe9d -> sub_82D5AF20: u8 (u32 0x8390B9AC) + 3415 (sub_823EB510 returns that pointer).
// The character switch wheel has no HUD component: the app knows it (D-pad DOWN held, by touch or the pad).
// v705 (owner 2026-10-24: "touch controls should not appear in menus or cutscenes"; SPEC 3.10):
//   IS_CUTSCENE_PLAYING a3a78392 -> sub_82D5AF58 -> sub_82328038: u8 (the cutscene manager) + 3147 (joins bit 19 with ACTIVE's +3415).
//   IS_SCREEN_FADED_OUT 9caa05fa -> sub_82D590A8 -> sub_82475968: u32 0x8389ED84 (0x838A0000 - 6896 + 2164; v705-v709 had 0x83898D84) != 0 and u8 0x83793488 & 0x60 (bit 20: loading, fades).
//   IS_PLAYER_CONTROL_ON 618857f2 -> sub_82DB1CE0 -> sub_824A5350(player 0): with the network flag u8 0x83B13AEB clear, sub_824A53D0
//   returns the player ped (the slot path above) and control is on when u32 (u32 (ped + 2912) + 284) == 0 (bit 21: control off).
//   The phone: CAN_PHONE_BE_SEEN_ON_SCREEN is a stub (returns 1); no signal.
// v706: bit 18 was wrong (the owner opened the pause menu on the Fold at 21:10:19-23 with bit 18 = 0). sub_8243FE18(1) read in full
// (addresses = 0x8243FE18 + offset): A = u8 0x83895CB5 || u8 0x83895CA4 (+0x18..+0x3c); with A and the four frontend movies loaded
// (+0x54..+0xe8) it returns 1; with A and a movie missing, A becomes u8 0x83895CA4 alone (+0xec) and a non-zero A returns 1 (+0xf0..+0x154);
// with A == 0 (the owner's case) it returns u8 0x83895D0F (+0x14c, returned as is at +0x184) or else u8 0x83895DF3 (+0x16c). Bit 18 is now
// A || u8 0x83895D0F || u8 0x83895DF3 (the movie test left out: with A set and a movie missing only 0x83895CB5 alone differs). Bits
// 22-25 carry the four bytes (!= 0) as device evidence: 22 = 0x83895CB5, 23 = 0x83895CA4, 24 = 0x83895D0F, 25 = 0x83895DF3.
#include <cstdint>

namespace recomp::touchctx {
enum Context:int {None=0,Foot=1,Car=2,Bike=3,Bicycle=4,Boat=5,Sub=6,Heli=7,Plane=8,Train=9};
constexpr std::uint32_t PlayerSlotGlobal=0x8394B8B0u,PlayerSlotPed=4,EntityType=20,PedFlags=3444,PedVehicle=3620,VehicleType=1604;
constexpr std::uint8_t TypePed=4;
constexpr std::uint32_t InVehicleBit=1u<<30;
inline bool guestPointer(std::uint32_t a) noexcept {return a>=0x10000u && a<=0xFFFF0000u && !(a&3u);}
/** The context for a vehicle type (vehicle+1604). */
constexpr Context fromVehicleType(std::uint32_t t) noexcept {
    switch(t) {
        case 1:return Plane;
        case 5:case 6:case 7:return Heli;
        case 8:return Bike;
        case 9:return Bicycle;
        case 10:return Boat;
        case 11:return Train;
        case 12:return Sub;
        default:return Car; // 0 car, 2 trailer, 3 quad, 4 (not seen)
    }
}
struct Result {Context context=None;int vehicleType=-1;}; // vehicleType: -1 none (on foot / unknown)
constexpr std::uint32_t HudComponents=0x83899158u,HudStride=112,HudWeaponWheel=19,HudRadioStations=16,HudActiveBit=0x80u;
constexpr std::uint32_t FrontendA=0x83895CB5u,FrontendB=0x83895CA4u,FrontendC=0x83895D0Fu,FrontendD=0x83895DF3u,CutsceneManager=0x8390B9ACu,CutsceneActive=3415,CutscenePlaying=3147;
constexpr std::uint32_t FadeActive=0x8389ED84u,FadeFlags=0x83793488u,FadeOutMask=0x60u,NetworkGame=0x83B13AEBu,PedPlayerInfo=2912,ControlFlags=284;
enum Screen:int {WeaponWheel=1<<16,RadioWheel=1<<17,Frontend=1<<18,Cutscene=1<<19,FadedOut=1<<20,ControlOff=1<<21,FrontendBytes=0xF<<22};
/** The screens that take the sticks (Screen bits; 0 = none). */
template<class M> int screens(const M& m) noexcept {
    int s=0;
    if(m.u8(HudComponents+HudStride*HudWeaponWheel)&HudActiveBit)s|=WeaponWheel;
    if(m.u8(HudComponents+HudStride*HudRadioStations)&HudActiveBit)s|=RadioWheel;
    const int raw=(m.u8(FrontendA)?1:0)|(m.u8(FrontendB)?2:0)|(m.u8(FrontendC)?4:0)|(m.u8(FrontendD)?8:0);
    if(raw)s|=Frontend|raw<<22; // (IS_PAUSE_MENU_ACTIVE(1); the bytes themselves in bits 22-25)
    const std::uint32_t cut=m.u32(CutsceneManager);
    if(guestPointer(cut) && (m.u8(cut+CutsceneActive) || m.u8(cut+CutscenePlaying)))s|=Cutscene;
    if(m.u32(FadeActive) && (m.u8(FadeFlags)&FadeOutMask))s|=FadedOut;
    if(!m.u8(NetworkGame)) {
        const std::uint32_t slot=m.u32(PlayerSlotGlobal),ped=guestPointer(slot)?m.u32(slot+PlayerSlotPed):0;
        const std::uint32_t info=guestPointer(ped)?m.u32(ped+PedPlayerInfo):0;
        if(guestPointer(info) && m.u32(info+ControlFlags))s|=ControlOff;
    }
    return s;
}
// M: anything with u32 / u8 (hud_game_state.h GuestMemory on the device, a fake in the host test).
template<class M> Result read(const M& m) noexcept {
    Result r;
    const std::uint32_t slot=m.u32(PlayerSlotGlobal);
    if(!guestPointer(slot))return r;
    const std::uint32_t ped=m.u32(slot+PlayerSlotPed);
    if(!guestPointer(ped) || m.u8(ped+EntityType)!=TypePed)return r;
    const std::uint32_t vehicle=m.u32(ped+PedVehicle);
    if(!(m.u32(ped+PedFlags)&InVehicleBit) || !guestPointer(vehicle)){r.context=Foot;return r;}
    const std::uint32_t t=m.u32(vehicle+VehicleType);
    r.vehicleType=t<=255u?int(t):-1;
    r.context=fromVehicleType(t);
    return r;
}
// v707 (owner 2026-10-24: "in gta theres a weapon in the top corner of the screen, can you put that in the touch controls"; SPEC 3.12): the
// player's selected weapon and its group, for the app's WEAPON button:
//   GET_SELECTED_PED_WEAPON d240123e -> sub_82DFCB68 -> sub_82DFAF30: u32 (u32 (ped + 2920) + 12) (the ped's weapon manager, its selection).
//   GET_WEAPONTYPE_GROUP 5f2de833 -> sub_82DFC330 -> sub_8257F180: a binary search of the weapon infos at 0x83B0E9D0 (lis -31823, addi -5680;
//   v710: v707-v709 had 0x83AFE9D0, a subtraction slip - the Fold read ffffffff there, TOUCH_WEAPON_RAW 21:53) (u32 +32 = the array of
//   info pointers, u16 +36 = their count, sorted by the hash at info + 8; lo = mid + 1 when the hash is greater, unsigned); group = u32 info + 60.
// Group hashes = joaat of the group names (group_unarmed a00fc1e4 ... ; weapon_unarmed hashes to a2719263, the game's own value).
constexpr std::uint32_t PedWeaponManager=2920,SelectedWeapon=12,WeaponInfos=0x83B0E9D0u,InfoArray=32,InfoCount=36,InfoHash=8,InfoGroup=60;
enum WeaponGroup:int {GroupNone=0,Unarmed=1,Melee=2,Pistol=3,Smg=4,Rifle=5,Mg=6,Shotgun=7,Sniper=8,Heavy=9,Thrown=10,GroupOther=11,
    // v708 (the Fold, 21:41: v707 read group 0 for Michael unarmed): where the read stopped, in the values the groups leave free
    NotFound=12,NoTable=13,NoManager=14,NoPed=15};
constexpr int groupOf(std::uint32_t h) noexcept {
    switch(h) {
        case 0xa00fc1e4u:return Unarmed;case 0xd49321d4u:return Melee;case 0x18d5fa97u:return Pistol;case 0xc6e9a5c5u:return Smg;
        case 0x39d5c192u:return Rifle;case 0x451b04bcu:return Mg;case 0x33431399u:return Shotgun;case 0xb7bbd827u:return Sniper;
        case 0xa27a4f9fu:return Heavy;case 0x5c4c5883u:return Thrown;default:return GroupOther;
    }
}
template<class M> std::uint32_t u16(const M& m,std::uint32_t a) noexcept {return std::uint32_t(m.u8(a))<<8|m.u8(a+1);}
/** The player's selected weapon hash (0: none / unknown); why = NoPed / NoManager when it stops before the hash. */
template<class M> std::uint32_t selectedWeapon(const M& m,int* why=nullptr) noexcept {
    const std::uint32_t slot=m.u32(PlayerSlotGlobal),ped=guestPointer(slot)?m.u32(slot+PlayerSlotPed):0;
    if(!guestPointer(ped) || m.u8(ped+EntityType)!=TypePed){if(why)*why=NoPed;return 0;}
    const std::uint32_t mgr=m.u32(ped+PedWeaponManager);
    if(!guestPointer(mgr)){if(why)*why=NoManager;return 0;}
    return m.u32(mgr+SelectedWeapon);
}
/** The group of a weapon hash (WeaponGroup; GroupNone when the hash is 0 or not in the infos). */
template<class M> int weaponGroup(const M& m,std::uint32_t hash) noexcept {
    if(!hash)return GroupNone;
    const std::uint32_t arr=m.u32(WeaponInfos+InfoArray);const int count=int(u16(m,WeaponInfos+InfoCount));
    if(!guestPointer(arr) || count<=0)return NoTable;
    int lo=0,hi=count-1;
    for(int guard=0;lo<=hi && guard<32;++guard) {
        const int mid=(lo+hi)>>1;const std::uint32_t info=m.u32(arr+std::uint32_t(mid)*4u);
        if(!guestPointer(info))return NoTable;
        const std::uint32_t h=m.u32(info+InfoHash);
        if(h==hash)return groupOf(m.u32(info+InfoGroup));
        if(hash>h)lo=mid+1;else hi=mid-1;
    }
    return NotFound;
}
/** Bits 26-29: the group, or where the read stopped (12-15; 0 = the hash itself is 0). */
template<class M> int weaponNibble(const M& m) noexcept {int why=GroupNone;const std::uint32_t h=selectedWeapon(m,&why);return h?weaponGroup(m,h):why;}
/** The exported word: -1 no guest memory yet; else context | (vehicleType + 1) << 8 | screens (bits 16-19). Bit 15 (the pause full
 *  map) is the export's own (trace_runner.cpp). */
template<class M> int word(const M& m) noexcept {const auto r=read(m);return int(r.context)|((r.vehicleType+1)&0x7F)<<8|screens(m)|(weaponNibble(m)&0xF)<<26;} // (bits 16-25 screens, 26-29 the weapon group)
// Big-endian guest words through relaxed loads (the whole 4 GB range is mapped: guest_address_space.cpp).
struct GuestMemory {
    const std::uint8_t* base=nullptr;
    std::uint32_t u32(std::uint32_t a) const noexcept {
        if(!guestPointer(a))return 0;
        return __builtin_bswap32(__atomic_load_n(reinterpret_cast<const std::uint32_t*>(base+a),__ATOMIC_RELAXED));
    }
    std::uint8_t u8(std::uint32_t a) const noexcept {return a>=0x10000u && a<=0xFFFF0000u?__atomic_load_n(base+a,__ATOMIC_RELAXED):0;}
};
}
