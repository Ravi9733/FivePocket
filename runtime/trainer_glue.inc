// trainer (2026-10-25): the library side of trainer.h, included at the end of indirect_call_resolver.cpp (it uses that file's
// PPCFuncMappings search and its native registration / probe paths). Shared by every edition: no Thor-only part.
//  - recomp_native_probe_register (every native the game registers) -> recomp_trainer_note_native: the handlers of the natives
//    the trainer calls (trainer.h UsedNatives), by hash.
//  - indirectCallSlow's probe path, after a probed native returned -> recomp_trainer_after_native: when it was PLAYER_PED_ID
//    called by the script VM (return address 0x8343A208, sub_83439990's native call), the trainer's tick runs once per game frame.
//    Both are plain calls (no new branch in the profiled resolver: its IR-PGO CFG hash, and so its profile, stay as they were).
//  - the exports the apps call (dlsym on the library they loaded): recomp_trainer_v1 / _state_v1 / _status_v1 / _list_v1.
#include "trainer.h"
#include <chrono>
#include <unistd.h>
#include "autopilot.h"   // v842: the play bot's in-game autopilot (the game's own AI tasks), ticked at the same safe point
#include "phone_call.h"  // v852: the phone's call state (IS_PED_RINGTONE_PLAYING / IS_MOBILE_PHONE_CALL_ONGOING), polled at the same safe point
#if defined(__aarch64__)
#include <arm_acle.h>
#endif

namespace {
constexpr std::uint32_t VmNativeReturn=0x8343A208u;   // the script VM's native call (sub_83439990, bctrl at 0x8343A204)
constexpr std::uint32_t FrameCounter=0x83C5D10Cu;     // GET_FRAME_COUNT's word (sub_82D666E8: lis -31802, addi -12024, lwz +4)
constexpr std::uint32_t PlayerPedIdHash=0xfa92e226u;
void trainerLog(const char* line) {__android_log_print(ANDROID_LOG_INFO,"RecompRenderer","%s",line);}
recomp::trainer::Trainer& trainer() {static recomp::trainer::Trainer t(trainerLog);return t;}
std::array<std::atomic<std::uint32_t>,std::size(recomp::trainer::UsedNatives)> trainerHandlers{};
std::array<std::atomic<std::uint32_t>,std::size(recomp::autopilot::UsedNatives)> autopilotHandlers{}; // v842
std::array<std::atomic<std::uint32_t>,std::size(recomp::phonecall::UsedNatives)> phoneCallHandlers{}; // v852
recomp::autopilot::Autopilot& autopilot() {static recomp::autopilot::Autopilot a(trainerLog);return a;}
// v842: a native's recorded handler, from either list (0: not one of ours, or not registered yet)
std::uint32_t nativeHandler(std::uint32_t hash) noexcept {
    if(const auto* at=std::find(std::begin(recomp::trainer::UsedNatives),std::end(recomp::trainer::UsedNatives),hash);at!=std::end(recomp::trainer::UsedNatives))
        return trainerHandlers[std::size_t(at-std::begin(recomp::trainer::UsedNatives))].load(std::memory_order_acquire);
    if(const auto* at=std::find(std::begin(recomp::autopilot::UsedNatives),std::end(recomp::autopilot::UsedNatives),hash);at!=std::end(recomp::autopilot::UsedNatives))
        return autopilotHandlers[std::size_t(at-std::begin(recomp::autopilot::UsedNatives))].load(std::memory_order_acquire);
    if(const auto* at=std::find(std::begin(recomp::phonecall::UsedNatives),std::end(recomp::phonecall::UsedNatives),hash);at!=std::end(recomp::phonecall::UsedNatives))
        return phoneCallHandlers[std::size_t(at-std::begin(recomp::phonecall::UsedNatives))].load(std::memory_order_acquire); // v852
    return 0;
}
std::atomic<std::uint32_t> trainerLastFrame{0xFFFFFFFFu};
thread_local bool trainerInside=false;

std::uint32_t trainerBe(const std::uint8_t* base,std::uint32_t a) noexcept {std::uint32_t v;std::memcpy(&v,base+a,4);return __builtin_bswap32(v);}
void trainerPut(std::uint8_t* base,std::uint32_t a,std::uint32_t v) noexcept {v=__builtin_bswap32(v);std::memcpy(base+a,&v,4);}
PPCFunc* trainerHost(std::uint32_t guest) noexcept {
    const auto count=recomp::bootstrap::validatedMappingCount.load(std::memory_order_acquire);
    std::size_t first=0,last=count;
    while(first<last){const auto m=first+(last-first)/2;if(PPCFuncMappings[m].guest<guest)first=m+1;else last=m;}
    return first<count && PPCFuncMappings[first].guest==guest?PPCFuncMappings[first].host:nullptr;
}

// The natives through guest memory: one call context below the VM's stack pointer, laid out as the VM's own (sub_83439990 at
// r1+128: +0 result pointer, +4 argument count, +8 argument pointer, +12 = 0), then the arguments, the result and small scratch.
class GuestNatives final : public recomp::trainer::Natives {
public:
    static constexpr std::uint32_t Frame=0x800,Scratch=0x100,Args=0x20,Result=0x70,Heap=0x80,HeapEnd=0x600;
    GuestNatives(PPCContext& c,std::uint8_t* base,std::uint32_t r1) noexcept:c_(c),base_(base),r1_(r1),s_(r1+Scratch) {}
    bool call(std::uint32_t hash,const std::uint32_t* args,unsigned n,std::uint32_t* out) override {
        out[0]=out[1]=out[2]=0;
        if(n>12)return false;
        const std::uint32_t handler=nativeHandler(hash); // (v842: the trainer's and the autopilot's natives)
        PPCFunc* host=handler?trainerHost(handler):nullptr;
        if(!host)return false;
        std::memset(base_+s_,0,Heap);
        trainerPut(base_,s_+0,s_+Result);trainerPut(base_,s_+4,n);trainerPut(base_,s_+8,s_+Args);
        for(unsigned k=0;k<n;++k)trainerPut(base_,s_+Args+4*k,args[k]);
        c_.r3.u64=s_;c_.r1.u64=r1_;c_.lr=VmNativeReturn;
        host(c_,base_);
        for(unsigned k=0;k<3;++k)out[k]=trainerBe(base_,s_+Result+4*k);
        return true;
    }
    std::uint32_t text(const char* s) override {
        const std::size_t n=std::strlen(s)+1;
        if(n>HeapEnd-Heap-used_)return 0;
        const std::uint32_t a=s_+Heap+used_;std::memcpy(base_+a,s,n);used_=std::uint32_t((used_+n+3)&~std::size_t(3));return a;
    }
    std::uint32_t word(std::uint32_t v) override {
        if(used_+4>HeapEnd-Heap)return 0;
        const std::uint32_t a=s_+Heap+used_;trainerPut(base_,a,v);used_+=4;return a;
    }
    std::uint32_t load(std::uint32_t a) override {return trainerBe(base_,a);}
private:
    PPCContext& c_;std::uint8_t* base_;std::uint32_t r1_,s_,used_=0;
};

void trainerReadSettings() noexcept {
    char text[512]{};std::size_t n=0;
    if(FILE* f=std::fopen(RECOMP_APP_PATH("/settings/trainer.txt"),"rb")){n=std::fread(text,1,sizeof text-1,f);std::fclose(f);}
    trainer().setEnabled(recomp::trainer::parseEnabled(text,n));
}
void trainerSettingsOnce() noexcept {static std::once_flag once;std::call_once(once,trainerReadSettings);}
} // namespace

extern "C" void recomp_trainer_note_native(std::uint32_t hash,std::uint32_t handler) noexcept {
    const auto* at=std::find(std::begin(recomp::trainer::UsedNatives),std::end(recomp::trainer::UsedNatives),hash);
    if(at!=std::end(recomp::trainer::UsedNatives))
        trainerHandlers[std::size_t(at-std::begin(recomp::trainer::UsedNatives))].store(handler,std::memory_order_release);
    const auto* ap=std::find(std::begin(recomp::autopilot::UsedNatives),std::end(recomp::autopilot::UsedNatives),hash);
    if(ap!=std::end(recomp::autopilot::UsedNatives))
        autopilotHandlers[std::size_t(ap-std::begin(recomp::autopilot::UsedNatives))].store(handler,std::memory_order_release);
    const auto* pc=std::find(std::begin(recomp::phonecall::UsedNatives),std::end(recomp::phonecall::UsedNatives),hash); // v852
    if(pc!=std::end(recomp::phonecall::UsedNatives))
        phoneCallHandlers[std::size_t(pc-std::begin(recomp::phonecall::UsedNatives))].store(handler,std::memory_order_release);
}
namespace {
// v852: the call poll's off switch (files/private-trace/diag-phone-call-off), looked for every SwitchFrames game frames; one
// RENDER_PHONE_CALL line per change of the call bits (and of the switch).
std::atomic<bool> phoneCallOff{false};
std::atomic<std::uint32_t> phoneCallLastFrame{0xFFFFFFFFu}; // (the poll's own once-per-frame word)
void phoneCallSwitch(std::uint32_t frame) noexcept {
    static std::uint32_t nextAt=0;static bool first=true;
    if(!first && frame-nextAt>0x80000000u)return;   // (before nextAt; wraps)
    nextAt=frame+recomp::phonecall::SwitchFrames;
    const bool off=::access(RECOMP_APP_PATH("/private-trace/diag-phone-call-off"),F_OK)==0;
    if(first || off!=phoneCallOff.load(std::memory_order_relaxed)) {
        phoneCallOff.store(off,std::memory_order_relaxed);
        if(off)recomp::phonecall::setCall(false,false);
        char msg[160];std::snprintf(msg,sizeof msg,"RENDER_PHONE_CALL poll=%u ring_native=%u call_native=%u",unsigned(!off),
            unsigned(nativeHandler(recomp::phonecall::IsPedRingtonePlaying)!=0),unsigned(nativeHandler(recomp::phonecall::IsMobilePhoneCallOngoing)!=0));
        trainerLog(msg);
    }
    first=false;
}
template<class N> void phoneCallTick(N& natives) {
    if(phoneCallOff.load(std::memory_order_relaxed))return;
    const unsigned before=recomp::phonecall::state()&(recomp::phonecall::Ringing|recomp::phonecall::InCall);
    const unsigned now=recomp::phonecall::poll(natives);
    if(now!=before){char msg[96];std::snprintf(msg,sizeof msg,"RENDER_PHONE_CALL ring=%u call=%u up=%u",unsigned((now&recomp::phonecall::Ringing)!=0),
        unsigned((now&recomp::phonecall::InCall)!=0),unsigned((recomp::phonecall::state()&recomp::phonecall::Up)!=0));trainerLog(msg);}
}
} // namespace
namespace {
// v842: the bot's command file, files/private-trace/diag-autopilot (one line, written atomically by the bot: tmp + mv), looked for at
// most every 100 ms on the script thread; read, removed and handed to the autopilot. Nothing else reads it.
void autopilotPoll() noexcept {
    static std::atomic<long long> next{0};
    const long long now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if(now<next.load(std::memory_order_relaxed))return;
    next.store(now+100,std::memory_order_relaxed);
    constexpr const char* Path=RECOMP_APP_PATH("/private-trace/diag-autopilot");
    FILE* f=std::fopen(Path,"r");
    if(!f)return;
    char line[256]{};const bool got=std::fgets(line,sizeof line,f)!=nullptr;
    std::fclose(f);
    if(!got || !line[0])return;   // (empty: being written; the next poll reads it)
    ::unlink(Path);
    recomp::autopilot::Command c;
    if(recomp::autopilot::parse(line,c))autopilot().submit(c);
    else {char msg[300];std::snprintf(msg,sizeof msg,"RENDER_AUTOPILOT rejected line=%.200s",line);trainerLog(msg);}
}
} // namespace
// v842: the owner's own input (the physical pad or the app's touch), from the gamepad callback: the autopilot lets go at once.
extern "C" void recomp_autopilot_note_human() noexcept {autopilot().noteHuman();}
extern "C" [[gnu::noinline]] void recomp_trainer_after_native(std::uint32_t hash,std::uint64_t callerLr,PPCContext& c,std::uint8_t* base) noexcept {
    if(hash!=PlayerPedIdHash || callerLr!=VmNativeReturn || trainerInside)return;
    auto& t=trainer();auto& ap=autopilot();
    autopilotPoll(); // v842
    const std::uint32_t r1=c.r1.u32;
    if(r1<0x10000u+GuestNatives::Frame || (r1&3u))return;
    const std::uint32_t frame=trainerBe(base,FrameCounter);
    // v852: the trainer / autopilot tick keeps its own once-per-frame rule exactly as before (claimed only while one of them wants a
    // tick); the call poll has its own (every game frame, whatever the trainer does).
    const bool tickFrame=(t.wanted() || ap.wanted()) && trainerLastFrame.exchange(frame,std::memory_order_relaxed)!=frame;
    const bool phoneFrame=phoneCallLastFrame.exchange(frame,std::memory_order_relaxed)!=frame;
    if(!tickFrame && !phoneFrame)return;
    if(phoneFrame)phoneCallSwitch(frame);
    trainerInside=true;
    const PPCContext saved=c;   // the VM continues with exactly the registers it had
#if defined(__aarch64__)
    std::uint64_t fpcr;asm volatile("mrs %0, fpcr":"=r"(fpcr));
#endif
    {
        GuestNatives natives(c,base,(r1-GuestNatives::Frame)&~0xFu);
        if(tickFrame && t.wanted())t.tick(natives,frame);
        if(tickFrame && ap.wanted()){GuestNatives own(c,base,(r1-GuestNatives::Frame)&~0xFu);ap.tick(own,frame);} // v842 (its own scratch)
        if(phoneFrame){GuestNatives own(c,base,(r1-GuestNatives::Frame)&~0xFu);phoneCallTick(own);} // v852
    }
#if defined(__aarch64__)
    asm volatile("msr fpcr, %0"::"r"(fpcr));
#endif
    c=saved;
    trainerInside=false;
}
// A saved game was committed (guest_save_store.h): logged when trainer toggles are on.
extern "C" void recomp_trainer_note_save(const char* name) noexcept {trainer().noteSave(name);}

// ---- the API (trainer.h: the action, toggle and list words; README: local-analysis/trainer-mods/README.md)
extern "C" __attribute__((visibility("default"),used)) const char recomp_trainer_marker[]="recomp-trainer-v1";
extern "C" __attribute__((visibility("default"))) int recomp_trainer_v1(int action,int a0,int a1,int a2) {
    trainerSettingsOnce();
    if(action==recomp::trainer::Sync){trainerReadSettings();return trainer().enabled()?1:0;}
    return trainer().request(action,a0,a1,a2);
}
extern "C" __attribute__((visibility("default"))) unsigned recomp_trainer_state_v1(void) {trainerSettingsOnce();return trainer().state();}
// v852: the phone's state for the apps' touch layouts (phone_call.h): bit 0 phone up, bit 1 ringing, bit 2 in a call.
extern "C" __attribute__((visibility("default"))) unsigned recomp_phone_state_v1(void) {return recomp::phonecall::state();}
extern "C" __attribute__((visibility("default"))) int recomp_trainer_status_v1(char* out,int cap) {return trainer().status(out,cap);}
extern "C" __attribute__((visibility("default"))) int recomp_trainer_list_v1(int list,int index,char* out,int cap) {
    return recomp::trainer::listEntry(list,index,out,cap);
}
