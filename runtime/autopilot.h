#pragma once
// autopilot (2026-10-25, v842; owner: "if people can make aimbots and tasbots, the great opus 5.5 should be doing way way way better"):
// the play bot's hands inside the game. Instead of steering Michael with synthetic sticks from screenshots (adb + model latency of
// seconds), the bot writes one command (files/private-trace/diag-autopilot, written atomically: tmp + mv) and the library gives the
// player's ped the GAME'S OWN AI task for it (TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE, TASK_VEHICLE_CHASE, TASK_SHOOT_AT_ENTITY ...),
// the way GTA trainers' "auto-drive to waypoint" does. The game's AI then drives, paths, enters and shoots at frame rate.
//
// It runs where the trainer runs (trainer_glue.inc: the script thread, inside a script's PLAYER_PED_ID native call, once per game
// frame) through the same Natives interface (trainer.h), so the natives see a current script thread as for the game's own calls.
// Argument slots of every task native below were read from our recompiled handlers (v840 src; 4-byte slots, a Vector3 = 3 slots):
//   TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE sub_82E0E0F8: ped, veh, x y z (vector at +8), speed f, style i, stop range f      (8)
//   TASK_VEHICLE_CHASE sub_82E01850: ped, target ped                                                                       (2)
//   TASK_VEHICLE_DRIVE_WANDER sub_82DE5FB8: ped, veh, speed f, style i                                                     (4)
//   TASK_FOLLOW_NAV_MESH_TO_COORD sub_82DE60F8: ped, x y z (+4), speed f, timeout i, stop range f, persist i, unk i       (9)
//   TASK_ENTER_VEHICLE sub_82DE5EE8: ped, veh, timeout i, seat i, speed f, flags i, unk i                                  (7)
//   TASK_LEAVE_ANY_VEHICLE sub_82DE64C0: ped, unk i, flags i                                                               (3)
//   TASK_SHOOT_AT_COORD sub_82E017E8: ped, x y z (+4), duration ms, firing pattern hash                                     (6)
//   TASK_SHOOT_AT_ENTITY sub_82E01860: ped, entity, duration ms, firing pattern hash                                       (4)
//   TASK_AIM_GUN_AT_COORD sub_82E017B0: ped, x y z (+4), time ms, unk, unk                                                 (7)
//   TASK_COMBAT_HATED_TARGETS_AROUND_PED sub_82DB88C8: ped, radius f, unk                                                  (3)
//   TASK_SMART_FLEE_COORD sub_82DE6020: ped, x y z (+4), distance f, time ms, unk, unk                                      (8)
//   CLEAR_PED_TASKS sub_82DE6500: ped (1); GET_SCRIPT_TASK_STATUS sub_82DE6480: ped, task hash (2)
//   GET_CLOSEST_VEHICLE sub_82E0EDD0 -> sub_82E0E510: x y z (vector at +0), radius f (+12), model (+16), flags (+20)       (6)
// Safety: nothing happens without a command file. A command waits (never dropped) while the gate is closed (pause menu,
// cutscene, switch, fade, death). Physical input (the owner's pad or touch: noteHuman) clears the tasks and drops the command at
// once. Every command, result and a status line about once a second are logged (RENDER_AUTOPILOT). Pure: the tests mock Natives.
#include "trainer.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace recomp::autopilot {

inline constexpr char Marker[]="recomp-autopilot-v1";
using recomp::trainer::joaat;using recomp::trainer::bits;using recomp::trainer::asFloat;using recomp::trainer::Natives;

namespace n {
constexpr std::uint32_t DriveLong=joaat("TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE"),Chase=joaat("TASK_VEHICLE_CHASE"),
    Wander=joaat("TASK_VEHICLE_DRIVE_WANDER"),NavMesh=joaat("TASK_FOLLOW_NAV_MESH_TO_COORD"),Enter=joaat("TASK_ENTER_VEHICLE"),
    LeaveAny=joaat("TASK_LEAVE_ANY_VEHICLE"),ShootCoord=joaat("TASK_SHOOT_AT_COORD"),ShootEntity=joaat("TASK_SHOOT_AT_ENTITY"),
    AimCoord=joaat("TASK_AIM_GUN_AT_COORD"),CombatAround=joaat("TASK_COMBAT_HATED_TARGETS_AROUND_PED"),Flee=joaat("TASK_SMART_FLEE_COORD"),
    ClearTasks=joaat("CLEAR_PED_TASKS"),TaskStatus=joaat("GET_SCRIPT_TASK_STATUS"),ClosestVehicle=joaat("GET_CLOSEST_VEHICLE"),
    EntitySpeed=joaat("GET_ENTITY_SPEED"),FirstBlip=joaat("GET_FIRST_BLIP_INFO_ID"),NextBlip=joaat("GET_NEXT_BLIP_INFO_ID"),
    BlipExists=joaat("DOES_BLIP_EXIST"),BlipCoord=joaat("GET_BLIP_INFO_ID_COORD"),BlipEntity=joaat("GET_BLIP_INFO_ID_ENTITY_INDEX"),
    BlipColour=joaat("GET_BLIP_COLOUR"),PedInSeat=joaat("GET_PED_IN_VEHICLE_SEAT"),IsVehicle=joaat("IS_ENTITY_A_VEHICLE"),
    IsPed=joaat("IS_ENTITY_A_PED"),EntityExists=joaat("DOES_ENTITY_EXIST"),
    // v844: SET_CURRENT_PED_WEAPON sub_82E02EA0 (ped, weapon hash, equip now: 3), HAS_PED_GOT_WEAPON sub_82DFC520 (ped, hash, 0: 3),
    // TASK_THROW_PROJECTILE sub_82E01940 (ped, x y z at +4: 4)
    SetWeapon=joaat("SET_CURRENT_PED_WEAPON"),HasWeapon=joaat("HAS_PED_GOT_WEAPON"),Throw=joaat("TASK_THROW_PROJECTILE"),
    // v845 (owner: "no just SLAM into the van"): TASK_VEHICLE_MISSION sub_82DE65A8 -> sub_82DE1658: ped, vehicle, target vehicle,
    // mission type (2 = ram), speed f, style i, min distance f, straight-line distance f, flag (9)
    VehicleMission=joaat("TASK_VEHICLE_MISSION");
}
// The natives this module calls besides the trainer's own (the glue records both lists' handlers).
inline constexpr std::uint32_t UsedNatives[]{n::DriveLong,n::Chase,n::Wander,n::NavMesh,n::Enter,n::LeaveAny,n::ShootCoord,n::ShootEntity,
    n::AimCoord,n::CombatAround,n::Flee,n::ClearTasks,n::TaskStatus,n::ClosestVehicle,n::EntitySpeed,n::NextBlip,n::BlipEntity,
    n::BlipColour,n::PedInSeat,n::IsVehicle,n::IsPed,n::EntityExists,n::SetWeapon,n::HasWeapon,n::Throw,n::VehicleMission};
namespace tn=recomp::trainer::n; // PLAYER_ID, PLAYER_PED_ID, the gate's natives, GET_ENTITY_COORDS, the vehicle and blip readers

constexpr std::uint32_t FullAuto=joaat("FIRING_PATTERN_FULL_AUTO");
// Driving styles (flag words of the game's driving AI): normal obeys lights, rushed overtakes, avoid keeps clear of traffic.
constexpr int StyleNormal=786603,StyleRushed=1074528293,StyleAvoid=786468,StyleIgnoreLights=2883621;

enum Verb : int {None=0,Stop,Drive,DriveBlip,ChaseBlip,Goto,GotoBlip,Enter,Leave,Shoot,ShootBlip,Aim,Combat,Wander,Flee,Blips,Weapon,Throw,RamBlip,ShootRear,AmbushBlip,GotoRear,VerbCount};
inline const char* verbName(int v) noexcept {
    static const char* const names[]={"none","stop","drive","drive_blip","chase_blip","goto","goto_blip","enter","leave","shoot",
        "shoot_blip","aim","combat","wander","flee","blips","weapon","throw","ram_blip","shoot_rear","ambush_blip","goto_rear"};
    return v>=0 && v<VerbCount?names[v]:"?";
}
struct Command {
    int verb=None;float x=0,y=0,z=0;float speed=20;int style=StyleNormal;float stop=8;
    int sprite=-1,colour=-1;int ms=3000;float radius=60;int seq=0;
    std::uint32_t weapon=0; // v844: weapon's hash (0 = "best": the first one owned of BestWeapons)
};

// One command line: "VERB arg ..." (blank-separated; "#" starts a comment). Numbers as text; styles by name or number.
//   stop | drive X Y Z [SPEED [STYLE [STOP]]] | drive_blip SPRITE [SPEED [STYLE [STOP [COLOUR]]]] | chase_blip SPRITE [COLOUR]
//   goto X Y Z [SPEED [STOP]] | goto_blip SPRITE [SPEED [STOP [COLOUR]]] | enter [RADIUS] | leave
//   shoot X Y Z [MS] | shoot_blip SPRITE [MS [COLOUR]] | aim X Y Z [MS] | combat [RADIUS] | wander [SPEED [STYLE]]
//   flee X Y Z [DISTANCE] | blips [MAXSPRITE] | weapon WEAPON_NAME|best | throw X Y Z   (v844)
//   goto_rear SPRITE [BACK [SIDE [COLOUR]]] (v849: on foot to BACK m behind the vehicle and SIDE m to its right - clear of our rammed car)
//   ambush_blip SPRITE COLOUR X Y Z [SPEED] (v848: drive to X Y Z on the target's road ahead of it, wait, then ram it from behind)
//   ram_blip SPRITE [SPEED [COLOUR [STYLE]]] (style 0: no stopping or avoiding) | shoot_rear SPRITE [MS [COLOUR]]   (v845: ram the blip's vehicle; fire at the point 3 m behind it)
inline bool parseStyle(const char* s,int& out) noexcept {
    if(!std::strcmp(s,"normal"))out=StyleNormal;else if(!std::strcmp(s,"rushed"))out=StyleRushed;
    else if(!std::strcmp(s,"avoid"))out=StyleAvoid;else if(!std::strcmp(s,"ignorelights"))out=StyleIgnoreLights;
    else {char* e=nullptr;const long v=std::strtol(s,&e,0);if(!e || *e)return false;out=int(v);}
    return true;
}
inline bool parse(const char* line,Command& c) noexcept {
    c=Command{};
    char buf[256];std::snprintf(buf,sizeof buf,"%s",line);
    if(char* h=std::strchr(buf,'#'))*h=0;
    const char* w[12]{};int k=0;
    for(char* t=std::strtok(buf," \t\r\n");t && k<12;t=std::strtok(nullptr," \t\r\n"))w[k++]=t;
    if(!k)return false;
    int v=None;for(int i=1;i<VerbCount;++i)if(!std::strcmp(w[0],verbName(i)))v=i;
    if(v==None)return false;
    c.verb=v;
    const auto num=[&](int i,float& out)->bool{if(i>=k)return true;char* e=nullptr;const float f=std::strtof(w[i],&e);
        if(!e || *e || !std::isfinite(f))return false;out=f;return true;};
    const auto inum=[&](int i,int& out)->bool{if(i>=k)return true;char* e=nullptr;const long l=std::strtol(w[i],&e,0);if(!e || *e)return false;
        out=int(l);return true;};
    const auto xyz=[&](int i)->bool{if(k<i+3)return false;return num(i,c.x) && num(i+1,c.y) && num(i+2,c.z) &&
        std::fabs(c.x)<20000.f && std::fabs(c.y)<20000.f && std::fabs(c.z)<5000.f;};
    const auto sprite=[&](int i)->bool{if(i>=k)return false;return inum(i,c.sprite) && c.sprite>=0 && c.sprite<1024;};
    switch(v) {
    case Stop: case Leave: return k==1;
    case Drive: return xyz(1) && num(4,c.speed) && (k<=5 || parseStyle(w[5],c.style)) && num(6,c.stop);
    case DriveBlip: return sprite(1) && num(2,c.speed) && (k<=3 || parseStyle(w[3],c.style)) && num(4,c.stop) && inum(5,c.colour);
    case ChaseBlip: return sprite(1) && inum(2,c.colour);
    case Goto: c.speed=2.0f;c.stop=1.5f;return xyz(1) && num(4,c.speed) && num(5,c.stop);
    case GotoBlip: c.speed=2.0f;c.stop=1.5f;return sprite(1) && num(2,c.speed) && num(3,c.stop) && inum(4,c.colour);
    case Enter: c.radius=25;return num(1,c.radius);
    case Shoot: case Aim: return xyz(1) && inum(4,c.ms) && c.ms>0 && c.ms<=60000;
    case ShootBlip: return sprite(1) && inum(2,c.ms) && c.ms>0 && c.ms<=60000 && inum(3,c.colour);
    case Combat: return num(1,c.radius);
    case Wander: return num(1,c.speed) && (k<=2 || parseStyle(w[2],c.style));
    case Flee: c.radius=200;return xyz(1) && num(4,c.radius);
    case Blips: c.sprite=600;return k==1 || inum(1,c.sprite);
    case Weapon: if(k!=2)return false;if(std::strcmp(w[1],"best"))c.weapon=joaat(w[1]);return true;
    case Throw: return xyz(1) && k==4;
    // v846: style 0 - no "stop before vehicles" (bit 0) / "avoid vehicles" (bit 2): v845's 2883621 (0x2C0025) made the RAM mission hold
    // 18-22 m behind the van for 3 rounds (BZ run 2, 11:32-11:35); an optional 4th word overrides
    case GotoRear: c.x=8;c.y=3;c.speed=3.0f;c.stop=1.0f;return sprite(1) && num(2,c.x) && num(3,c.y) && inum(4,c.colour) && k<=5;
    case AmbushBlip: c.speed=40;c.style=0;c.stop=4;return k>=6 && sprite(1) && inum(2,c.colour) && xyz(3) && num(6,c.speed);
    case RamBlip: c.speed=40;c.style=0;return sprite(1) && num(2,c.speed) && inum(3,c.colour) && (k<=4 || parseStyle(w[4],c.style));
    case ShootRear: return sprite(1) && inum(2,c.ms) && c.ms>0 && c.ms<=60000 && inum(3,c.colour);
    default: return false;
    }
}

// v844: "weapon best" takes the first of these the player owns (rifles first: the BZ van's doors, the Jewel Store's cases).
inline constexpr const char* BestWeapons[]{"WEAPON_CARBINERIFLE","WEAPON_ASSAULTRIFLE","WEAPON_ADVANCEDRIFLE","WEAPON_SMG","WEAPON_MICROSMG",
    "WEAPON_PUMPSHOTGUN","WEAPON_COMBATPISTOL","WEAPON_PISTOL"};
constexpr int MissionRam=2;
constexpr float AmbushPassMetres=60.0f; // v848/v850: the target counts as passed inside this once we are behind it along its heading
constexpr float RamCloseMetres=25.0f,RamLooseMetres=60.0f,RamAlignDegrees=60.0f,RamApartDegrees=120.0f; // v847: chase, then ram
constexpr float RearMetres=3.0f,ShootRangeMax=30.0f; // shoot_rear: the aim point behind the vehicle; refused beyond this range
constexpr std::uint32_t StatusFrames=30;       // a status line about once a second at 30 fps
constexpr std::uint32_t RetargetFrames=15;     // a moving blip target is re-read twice a second
constexpr float RetargetMetres=12.0f;          // ... and the drive task re-issued when it moved this far
constexpr unsigned BlipScanMax=24;              // blips of one sprite examined for the nearest

class Autopilot {
public:
    using LogFn=void(*)(const char* line);
    explicit Autopilot(LogFn log=nullptr) noexcept:log_(log) {}

    // ---- any thread
    void submit(const Command& c) noexcept {
        std::lock_guard l(m_);pending_=c;pending_.seq=++seq_;havePending_=true;
        logf("RENDER_AUTOPILOT command seq=%d verb=%s",pending_.seq,verbName(pending_.verb));
        work_.store(true,std::memory_order_release);
    }
    // The owner's own input (pad or touch): the tasks are cleared at the next tick and nothing more is issued until a new command.
    void noteHuman() noexcept {if(work_.load(std::memory_order_relaxed))human_.store(true,std::memory_order_release);}
    bool wanted() const noexcept {return work_.load(std::memory_order_relaxed);}

    // ---- the script thread (at most once per game frame)
    void tick(Natives& nat,std::uint32_t frame) noexcept {
        {std::lock_guard l(m_);if(havePending_){next_=pending_;havePending_=false;haveNext_=true;}}
        const std::uint32_t player=call1(nat,tn::PlayerId,{}),ped=call1(nat,tn::PlayerPedId,{});
        if(human_.exchange(false,std::memory_order_acq_rel)) {
            if(ped && active_.verb!=None)call0(nat,n::ClearTasks,{ped});
            if(active_.verb!=None || haveNext_)logf("RENDER_AUTOPILOT result seq=%d human-input tasks-cleared",haveNext_?next_.seq:active_.seq);
            active_=Command{};haveNext_=false;settle();return;
        }
        const char* why=gate(nat,player,ped);
        if(why) {
            if(why!=lastBlock_)logf("RENDER_AUTOPILOT blocked=%s",why);
            lastBlock_=why;return;   // the command waits: the game's own state (a cutscene, the pause menu) ends on its own
        }
        lastBlock_=nullptr;
        if(haveNext_){haveNext_=false;begin(nat,ped,player,next_,frame);}
        if(active_.verb!=None)follow(nat,ped,player,frame);
        settle();
    }

private:
    template<class... A> void logf(const char* f,A... a) const noexcept {
        if(!log_)return;
        char line[240];std::snprintf(line,sizeof line,f,a...);log_(line);
    }
    static std::uint32_t call1(Natives& nat,std::uint32_t hash,std::initializer_list<std::uint32_t> a) noexcept {
        std::uint32_t out[3]{};return nat.call(hash,a.begin(),unsigned(a.size()),out)?out[0]:0u;
    }
    static void call0(Natives& nat,std::uint32_t hash,std::initializer_list<std::uint32_t> a) noexcept {
        std::uint32_t out[3]{};(void)nat.call(hash,a.begin(),unsigned(a.size()),out);
    }
    static bool coords(Natives& nat,std::uint32_t entity,float* p) noexcept {
        std::uint32_t out[3]{};const std::uint32_t a[2]{entity,1};
        if(!nat.call(tn::GetEntityCoords,a,2,out))return false;
        for(int i=0;i<3;++i)p[i]=asFloat(out[i]);return true;
    }
    static const char* gate(Natives& nat,std::uint32_t player,std::uint32_t ped) noexcept {
        if(!ped)return "no-ped";
        if(call1(nat,tn::NetworkGame,{}))return "network-game";
        if(!call1(nat,tn::IsPlayerPlaying,{player}))return "no-player";
        if(call1(nat,tn::IsPauseMenuActive,{}))return "pause-menu";
        if(call1(nat,tn::IsCutsceneActive,{}))return "cutscene";
        if(call1(nat,tn::IsSwitchInProgress,{}))return "character-switch";
        if(call1(nat,tn::IsScreenFadedOut,{}))return "faded-out";
        if(call1(nat,tn::IsEntityDead,{ped}))return "player-dead";
        return nullptr;
    }
    void settle() noexcept {
        std::lock_guard l(m_);
        work_.store(havePending_ || haveNext_ || active_.verb!=None,std::memory_order_release);
    }
    void done(const char* kind,const char* detail) noexcept {
        logf("RENDER_AUTOPILOT result seq=%d %s %s %s",active_.seq,kind,verbName(active_.verb),detail?detail:"");
        active_=Command{};
    }
    static float dist(const float* a,float x,float y,float z) noexcept {
        const float dx=a[0]-x,dy=a[1]-y,dz=a[2]-z;return std::sqrt(dx*dx+dy*dy+dz*dz);
    }

    // The nearest blip of a sprite (and colour, when given): its id, its coordinates, its entity (0: a place, not an entity).
    struct Blip {std::uint32_t id=0,entity=0;float p[3]{};};
    static bool findBlip(Natives& nat,int sprite,int colour,const float* from,Blip& best) noexcept {
        bool found=false;float bestD=1e30f;
        std::uint32_t id=call1(nat,tn::FirstBlipInfoId,{std::uint32_t(sprite)});
        for(unsigned i=0;i<BlipScanMax && id;++i,id=call1(nat,n::NextBlip,{std::uint32_t(sprite)})) {
            if(!call1(nat,tn::DoesBlipExist,{id}))continue;
            if(colour>=0 && int(call1(nat,n::BlipColour,{id}))!=colour)continue;
            std::uint32_t out[3]{};if(!nat.call(tn::BlipInfoIdCoord,&id,1,out))continue;
            Blip b;b.id=id;for(int k=0;k<3;++k)b.p[k]=asFloat(out[k]);
            if(!std::isfinite(b.p[0]) || !std::isfinite(b.p[1]))continue;
            const float d=dist(from,b.p[0],b.p[1],b.p[2]);
            if(d<bestD){bestD=d;best=b;found=true;}
        }
        if(found)best.entity=call1(nat,n::BlipEntity,{best.id});
        return found;
    }
    static std::uint32_t vehicleOf(Natives& nat,std::uint32_t ped) noexcept {
        return call1(nat,tn::IsPedInAnyVehicle,{ped,0})?call1(nat,tn::GetVehiclePedIsIn,{ped,0}):0u;
    }
    void issueDrive(Natives& nat,std::uint32_t ped,std::uint32_t veh,float x,float y,float z) noexcept {
        call0(nat,n::DriveLong,{ped,veh,bits(x),bits(y),bits(z),bits(active_.speed),std::uint32_t(active_.style),bits(active_.stop)});
        tx_=x;ty_=y;tz_=z;
    }
    void issueGoto(Natives& nat,std::uint32_t ped,float x,float y,float z) noexcept {
        call0(nat,n::NavMesh,{ped,bits(x),bits(y),bits(z),bits(active_.speed),std::uint32_t(-1),bits(active_.stop),0,0});
        tx_=x;ty_=y;tz_=z;
    }

    void begin(Natives& nat,std::uint32_t ped,std::uint32_t player,const Command& c,std::uint32_t frame) noexcept {
        (void)player;
        if(active_.verb!=None && c.verb!=Blips && c.verb!=Weapon)call0(nat,n::ClearTasks,{ped}); // a new command replaces the running one (v844: a scan or a weapon change does not)
        if(c.verb==Blips){scanBlips(nat,ped,c);return;}
        if(c.verb==Weapon){selectWeapon(nat,ped,c);return;} // (instant: the running task keeps going)
        active_=c;started_=frame;lastStatus_=frame;lastRetarget_=frame;ramming_=0;ramDriver_=0;
        float me[3]{};coords(nat,ped,me);
        const std::uint32_t veh=vehicleOf(nat,ped);
        char d[120];
        switch(c.verb) {
        case Stop: call0(nat,n::ClearTasks,{ped});done("ok","");return;
        case Drive:
            if(!veh){done("fail","not-in-a-vehicle");return;}
            issueDrive(nat,ped,veh,c.x,c.y,c.z);break;
        case DriveBlip: case GotoBlip: case ChaseBlip: case ShootBlip: case RamBlip: case ShootRear: case GotoRear: {
            Blip b;if(!findBlip(nat,c.sprite,c.colour,me,b)){std::snprintf(d,sizeof d,"no-blip sprite=%d colour=%d",c.sprite,c.colour);done("fail",d);return;}
            target_=b.entity;
            if(c.verb==DriveBlip){if(!veh){done("fail","not-in-a-vehicle");return;}issueDrive(nat,ped,veh,b.p[0],b.p[1],b.p[2]);}
            else if(c.verb==GotoBlip)issueGoto(nat,ped,b.p[0],b.p[1],b.p[2]);
            else if(c.verb==RamBlip) { // v845: the game's own ram mission against the blip's vehicle, full speed (v846: style 0 by default)
                if(!veh){done("fail","not-in-a-vehicle");return;}
                if(!b.entity || !call1(nat,n::IsVehicle,{b.entity})){done("fail","blip-is-not-a-vehicle");return;}
                target_=b.entity;tx_=b.p[0];ty_=b.p[1];tz_=b.p[2];
                ramDriver_=call1(nat,n::PedInSeat,{b.entity,std::uint32_t(-1)});
                ramPhase(nat,ped,veh,me,true); // v847: chase first unless already close behind it, going its way
            } else if(c.verb==ShootRear) { // v845: the rear doors - 3 m behind the vehicle along its heading, a little above its centre
                if(!b.entity){done("fail","blip-has-no-entity");return;}
                float v[3]{};if(!coords(nat,b.entity,v)){done("fail","no-coords");return;}
                const float h=asFloat(call1(nat,tn::GetEntityHeading,{b.entity}))*3.14159265f/180.0f;
                const float rx=v[0]+std::sin(h)*RearMetres,ry=v[1]-std::cos(h)*RearMetres,rz=v[2]+0.3f; // forward = (-sin h, cos h)
                if(dist(me,v[0],v[1],v[2])>ShootRangeMax){done("fail","too-far");return;}
                call0(nat,n::ShootCoord,{ped,bits(rx),bits(ry),bits(rz),std::uint32_t(c.ms),FullAuto});
                target_=b.entity;tx_=rx;ty_=ry;tz_=rz;
            } else if(c.verb==GotoRear) { // v849 (BZ run 5: our rammed car stood against the van's doors; Michael shot it and died beside it)
                if(veh){done("fail","in-a-vehicle");return;}
                if(!b.entity){done("fail","blip-has-no-entity");return;}
                float v[3]{};if(!coords(nat,b.entity,v)){done("fail","no-coords");return;}
                const float h=asFloat(call1(nat,tn::GetEntityHeading,{b.entity}))*3.14159265f/180.0f;
                const float fx=-std::sin(h),fy=std::cos(h),rx=fy,ry=-fx; // forward, and its right
                const float gx=v[0]-fx*c.x+rx*c.y,gy=v[1]-fy*c.x+ry*c.y;
                active_.speed=c.speed;active_.stop=c.stop;issueGoto(nat,ped,gx,gy,v[2]);target_=b.entity;
            } else if(c.verb==ChaseBlip) {
                if(!veh){done("fail","not-in-a-vehicle");return;}
                std::uint32_t driver=0;
                if(b.entity && call1(nat,n::IsVehicle,{b.entity}))driver=call1(nat,n::PedInSeat,{b.entity,std::uint32_t(-1)});
                else if(b.entity && call1(nat,n::IsPed,{b.entity}))driver=b.entity;
                if(!driver){done("fail","blip-has-no-driver");return;}
                call0(nat,n::Chase,{ped,driver});target_=b.entity;tx_=b.p[0];ty_=b.p[1];tz_=b.p[2];
            } else {
                if(!b.entity){done("fail","blip-has-no-entity");return;}
                call0(nat,n::ShootEntity,{ped,b.entity,std::uint32_t(c.ms),FullAuto});tx_=b.p[0];ty_=b.p[1];tz_=b.p[2];
            }
            std::snprintf(d,sizeof d,"target=%.1f,%.1f,%.1f entity=%08x dist=%.1f",double(b.p[0]),double(b.p[1]),double(b.p[2]),b.entity,
                          double(dist(me,b.p[0],b.p[1],b.p[2])));
            logf("RENDER_AUTOPILOT begin seq=%d %s %s",c.seq,verbName(c.verb),d);
            return;}
        case AmbushBlip: { // v848: phase 1 - to the ambush point (the target's own carriageway, ahead of it)
            if(!veh){done("fail","not-in-a-vehicle");return;}
            const int keepStyle=active_.style;const float keepSpeed=active_.speed;
            active_.style=StyleRushed;active_.speed=35;issueDrive(nat,ped,veh,c.x,c.y,c.z);active_.style=keepStyle;active_.speed=keepSpeed; // (v850: rushed - run 6 arrived late)
            ambush_=1;ambushLast_=1e30f;target_=0;
            logf("RENDER_AUTOPILOT ambush seq=%d phase=drive point=%.1f,%.1f,%.1f",c.seq,double(c.x),double(c.y),double(c.z));
            return;}
        case Goto: issueGoto(nat,ped,c.x,c.y,c.z);break;
        case Enter: {
            const std::uint32_t args[6]{bits(me[0]),bits(me[1]),bits(me[2]),bits(c.radius),0,70};
            std::uint32_t out[3]{};(void)nat.call(n::ClosestVehicle,args,6,out);
            const std::uint32_t car=out[0];
            if(!car){done("fail","no-vehicle-near");return;}
            float at[3]{};const bool known=coords(nat,car,at);
            const float metres=known?dist(me,at[0],at[1],at[2]):c.radius;
            enterMs_=8000u+std::uint32_t(std::min(40.0f,metres)*1500.0f); // v843: walking there counts (12 s was too short from the street)
            call0(nat,n::Enter,{ped,car,enterMs_,std::uint32_t(-1),bits(2.0f),1,0});target_=car;tx_=at[0];ty_=at[1];tz_=at[2];
            logf("RENDER_AUTOPILOT begin seq=%d enter vehicle=%08x at=%.1f,%.1f,%.1f dist=%.1f timeout=%u",c.seq,car,double(at[0]),double(at[1]),
                 double(at[2]),double(metres),enterMs_);
            return;}
        case Leave: if(!veh){done("ok","already-on-foot");return;}call0(nat,n::LeaveAny,{ped,0,0});break;
        case Shoot: call0(nat,n::ShootCoord,{ped,bits(c.x),bits(c.y),bits(c.z),std::uint32_t(c.ms),FullAuto});break;
        case Aim: call0(nat,n::AimCoord,{ped,bits(c.x),bits(c.y),bits(c.z),std::uint32_t(c.ms),0,0});break;
        case Combat: call0(nat,n::CombatAround,{ped,bits(c.radius),0});break;
        case Wander: if(!veh){done("fail","not-in-a-vehicle");return;}
            call0(nat,n::Wander,{ped,veh,bits(c.speed),std::uint32_t(c.style)});break;
        case Throw: call0(nat,n::Throw,{ped,bits(c.x),bits(c.y),bits(c.z)});tx_=c.x;ty_=c.y;tz_=c.z;break;
        case Flee: call0(nat,n::Flee,{ped,bits(c.x),bits(c.y),bits(c.z),bits(c.radius),std::uint32_t(-1),0,0});break;
        default: done("fail","unknown");return;
        }
        logf("RENDER_AUTOPILOT begin seq=%d %s from=%.1f,%.1f,%.1f veh=%d",c.seq,verbName(c.verb),double(me[0]),double(me[1]),double(me[2]),veh?1:0);
    }

    void follow(Natives& nat,std::uint32_t ped,std::uint32_t player,std::uint32_t frame) noexcept {
        float me[3]{};coords(nat,ped,me);
        const std::uint32_t veh=vehicleOf(nat,ped);
        const std::uint32_t elapsedMs=(frame-started_)*33u; // (game frames at ~30 fps: a timeout guide, not a clock)
        // moving targets: re-read the blip and re-issue the drive / walk when it moved
        if((active_.verb==DriveBlip || active_.verb==GotoBlip) && frame-lastRetarget_>=RetargetFrames) {
            lastRetarget_=frame;Blip b;
            if(!findBlip(nat,active_.sprite,active_.colour,me,b)){done("ok","blip-gone");return;}
            if(dist(b.p,tx_,ty_,tz_)>RetargetMetres) {
                if(active_.verb==DriveBlip && veh)issueDrive(nat,ped,veh,b.p[0],b.p[1],b.p[2]);
                else if(active_.verb==GotoBlip)issueGoto(nat,ped,b.p[0],b.p[1],b.p[2]);
            }
        }
        if(active_.verb==AmbushBlip) {if(!veh){done("fail","left-the-vehicle");return;}ambushStep(nat,ped,veh,me,frame);return;}
        if(active_.verb==ChaseBlip || active_.verb==ShootBlip || active_.verb==RamBlip) {
            if(target_ && !call1(nat,n::EntityExists,{target_})){done("ok","target-gone");return;}
            if(target_){float t[3]{};if(coords(nat,target_,t)){tx_=t[0];ty_=t[1];tz_=t[2];}}
        }
        const float d=dist(me,tx_,ty_,tz_);
        if(active_.verb==RamBlip && veh && frame-lastRetarget_>=RetargetFrames){lastRetarget_=frame;ramPhase(nat,ped,veh,me,false);}
        switch(active_.verb) {
        case Drive: case DriveBlip: if(!veh){done("fail","left-the-vehicle");return;}
            if(d<=active_.stop+4.0f){char s[48];std::snprintf(s,sizeof s,"arrived dist=%.1f",double(d));done("ok",s);return;}break;
        case Goto: case GotoBlip: case GotoRear: if(d<=active_.stop+1.0f){char s[48];std::snprintf(s,sizeof s,"arrived dist=%.1f",double(d));done("ok",s);return;}break;
        case Enter: if(veh){done("ok","in-vehicle");return;}if(elapsedMs>enterMs_+2000u){done("fail","enter-timeout");return;}break;
        case Leave: if(!veh){done("ok","on-foot");return;}if(elapsedMs>6000){done("fail","leave-timeout");return;}break;
        case Shoot: case ShootBlip: case Aim: case ShootRear: if(elapsedMs>=std::uint32_t(active_.ms)+500u){done("ok","duration");return;}break;
        case Throw: if(elapsedMs>=3000u){done("ok","thrown");return;}break;
        default: break; // chase, combat, wander, flee: until the next command (or stop)
        }
        if(frame-lastStatus_>=StatusFrames) {
            lastStatus_=frame;
            const float speed=asFloat(call1(nat,n::EntitySpeed,{ped}));
            const float tspeed=target_ && call1(nat,n::EntityExists,{target_})?asFloat(call1(nat,n::EntitySpeed,{target_})):-1.0f; // v844
            logf("RENDER_AUTOPILOT status seq=%d %s pos=%.1f,%.1f,%.1f speed=%.1f veh=%d wanted=%u target=%.1f,%.1f,%.1f dist=%.1f tspeed=%.1f t=%u",
                 active_.seq,verbName(active_.verb),double(me[0]),double(me[1]),double(me[2]),double(speed),veh?1:0,
                 call1(nat,tn::GetWantedLevel,{player}),double(tx_),double(ty_),double(tz_),double(d),double(tspeed),elapsedMs);
        }
    }

    // v847 (BZ runs 2-3: the RAM mission never turned round - the van met Michael on the other carriageway and was 500 m away in 20 s;
    // run 1's TASK_VEHICLE_CHASE did turn round and closed to 13 m): ram_blip CHASES the van's driver until Michael is within RamCloseMetres
    // and heading within RamAlignDegrees of the van, then RAMS it (style 0); it falls back to the chase when the van gets RamLooseMetres away
    // or they face apart again. Phase changes are logged.
    static float headingGap(float a,float b) noexcept {float d=std::fmod(std::fabs(a-b),360.0f);return d>180.0f?360.0f-d:d;}
    void ramPhase(Natives& nat,std::uint32_t ped,std::uint32_t veh,const float* me,bool first) noexcept {
        float v[3]{};if(!coords(nat,target_,v))return;
        const float d=dist(me,v[0],v[1],v[2]);
        const float gap=headingGap(asFloat(call1(nat,tn::GetEntityHeading,{veh})),asFloat(call1(nat,tn::GetEntityHeading,{target_})));
        const bool close=d<=RamCloseMetres && gap<=RamAlignDegrees,loose=d>RamLooseMetres || gap>RamApartDegrees;
        const int want=first?(close?2:1):(ramming_==1 && close)?2:(ramming_==2 && loose)?1:ramming_;
        if(want==ramming_)return;
        if(want==2)call0(nat,n::VehicleMission,{ped,veh,target_,std::uint32_t(MissionRam),bits(active_.speed),std::uint32_t(active_.style),bits(1.0f),bits(0.0f),0});
        else if(ramDriver_)call0(nat,n::Chase,{ped,ramDriver_});
        else call0(nat,n::VehicleMission,{ped,veh,target_,std::uint32_t(MissionRam),bits(active_.speed),std::uint32_t(active_.style),bits(1.0f),bits(0.0f),0});
        ramming_=want;
        logf("RENDER_AUTOPILOT ram seq=%d phase=%s dist=%.1f heading_gap=%.0f",active_.seq,want==2?"ram":"chase",double(d),double(gap));
    }
    // v848 (BZ runs 1-4: every approach met the van head-on on the other carriageway; no task got back inside the ~264 m "got away"
    // limit): the video's geometry. 1 drive to the point; 2 wait there (no task: the car stands) until the target vehicle's blip exists
    // and has PASSED - within AmbushPassMetres with us behind it along its heading (v850); 3 ram_blip's
    // chase-then-ram from behind, going its way. Phase changes are logged.
    void ambushStep(Natives& nat,std::uint32_t ped,std::uint32_t veh,const float* me,std::uint32_t frame) noexcept {
        if(ambush_==1 && dist(me,active_.x,active_.y,active_.z)<=active_.stop+6.0f) {
            call0(nat,n::ClearTasks,{ped});ambush_=2;logf("RENDER_AUTOPILOT ambush seq=%d phase=wait at=%.1f,%.1f,%.1f",active_.seq,double(me[0]),double(me[1]),double(me[2]));
        }
        if(frame-lastRetarget_<RetargetFrames)return;
        lastRetarget_=frame;
        if(ambush_==3){ramPhase(nat,ped,veh,me,false);return;}
        Blip b;if(!findBlip(nat,active_.sprite,active_.colour,me,b) || !b.entity || !call1(nat,n::IsVehicle,{b.entity}))return; // not spawned yet
        float v[3]{};if(!coords(nat,b.entity,v))return;
        const float d=dist(me,v[0],v[1],v[2]);
        const float gap=headingGap(asFloat(call1(nat,tn::GetEntityHeading,{veh})),asFloat(call1(nat,tn::GetEntityHeading,{b.entity})));
        // v850 (BZ run 6: the van met us at 43 m during the drive and was 75 m away at the next sample; "distance growing" needs two
        // samples, the first had nothing to grow from): passed = within AmbushPassMetres and we are BEHIND it along its heading
        const float th=asFloat(call1(nat,tn::GetEntityHeading,{b.entity}))*3.14159265f/180.0f;
        const bool behind=(me[0]-v[0])*(-std::sin(th))+(me[1]-v[1])*std::cos(th)<0.0f;
        const bool passed=d<=AmbushPassMetres && behind; // (going our way: ramPhase rams; the other carriageway: it chases)
        if(frame-lastStatus_>=StatusFrames){lastStatus_=frame;
            logf("RENDER_AUTOPILOT ambush seq=%d phase=%s target_dist=%.1f heading_gap=%.0f tspeed=%.1f",active_.seq,ambush_==1?"drive":"wait",double(d),double(gap),
                 double(asFloat(call1(nat,n::EntitySpeed,{b.entity}))));}
        ambushLast_=d;
        if(!passed)return;
        target_=b.entity;ramDriver_=call1(nat,n::PedInSeat,{b.entity,std::uint32_t(-1)});ambush_=3;ramming_=0;
        logf("RENDER_AUTOPILOT ambush seq=%d phase=go target_dist=%.1f heading_gap=%.0f",active_.seq,double(d),double(gap));
        ramPhase(nat,ped,veh,me,true);
    }
    // v844: the named weapon (or the best owned) in hand now; the running task is not touched.
    void selectWeapon(Natives& nat,std::uint32_t ped,const Command& c) noexcept {
        std::uint32_t pick=0;
        if(c.weapon){if(call1(nat,n::HasWeapon,{ped,c.weapon,0}))pick=c.weapon;}
        else for(const char* w:BestWeapons){const std::uint32_t h=joaat(w);if(call1(nat,n::HasWeapon,{ped,h,0})){pick=h;break;}}
        if(!pick){logf("RENDER_AUTOPILOT result seq=%d fail weapon not-owned hash=%08x",c.seq,c.weapon);return;}
        call0(nat,n::SetWeapon,{ped,pick,1});
        logf("RENDER_AUTOPILOT result seq=%d ok weapon hash=%08x",c.seq,pick);
    }
    // Every blip of sprites 0..max: sprite, colour, coordinates, entity (what the bot can target).
    void scanBlips(Natives& nat,std::uint32_t ped,const Command& c) noexcept {
        float me[3]{};coords(nat,ped,me);unsigned count=0;
        for(int s=0;s<=c.sprite && s<1024;++s) {
            std::uint32_t id=call1(nat,tn::FirstBlipInfoId,{std::uint32_t(s)});
            for(unsigned i=0;i<BlipScanMax && id;++i,id=call1(nat,n::NextBlip,{std::uint32_t(s)})) {
                if(!call1(nat,tn::DoesBlipExist,{id}))continue;
                std::uint32_t out[3]{};if(!nat.call(tn::BlipInfoIdCoord,&id,1,out))continue;
                const float x=asFloat(out[0]),y=asFloat(out[1]),z=asFloat(out[2]);
                logf("RENDER_AUTOPILOT blip seq=%d sprite=%d colour=%u pos=%.1f,%.1f,%.1f dist=%.1f entity=%08x",c.seq,s,
                     call1(nat,n::BlipColour,{id}),double(x),double(y),double(z),double(dist(me,x,y,z)),call1(nat,n::BlipEntity,{id}));
                ++count;
            }
        }
        logf("RENDER_AUTOPILOT result seq=%d ok blips count=%u",c.seq,count);
    }

    LogFn log_;
    mutable std::mutex m_;Command pending_;bool havePending_=false;int seq_=0;  // m_: the submitter's side
    std::atomic<bool> work_{false},human_{false};
    // script thread only
    Command next_,active_;bool haveNext_=false;std::uint32_t started_=0,lastStatus_=0,lastRetarget_=0,target_=0;
    float tx_=0,ty_=0,tz_=0;const char* lastBlock_=nullptr;std::uint32_t enterMs_=12000;int ramming_=0;std::uint32_t ramDriver_=0;int ambush_=0;float ambushLast_=1e30f; // v848: 1 drive, 2 wait, 3 go // v847: 1 chase, 2 ram
};

} // namespace recomp::autopilot
