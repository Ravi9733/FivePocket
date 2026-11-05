#pragma once
#include "gta5_tile_scene_continuation.h"
#include "native_graphics_state.h"
#include <algorithm>
#include <array>
#include <map>

namespace recomp::gpu {
// Native consumer of the original scene IRQ packets. The owner supplies a real
// queue-ordering fence, current leased scratch reads/writes, and an exclusive
// handoff that validates the original source1 handler's return. No callback may
// invent an acknowledgement. This completes ONLY these packets, not the queued
// scene, deferred resource fences, or command-ring retirement.
class NativeTileCommandInterrupt final {
public:
    using Plan=Gta5TileCommandInterrupt;
    using Journal=std::map<std::uint32_t,std::pair<std::uint32_t,std::uint32_t>>;
    using Memory=std::function<bool(const Plan::Operation&)>;
    using Fence=std::function<bool()>;
    using Guest=std::function<bool(const Plan&)>;
    enum class Phase { Ready,Ordering,Prefix,GuestRunning,Acknowledging,Complete,Failed };
    explicit NativeTileCommandInterrupt(const Plan& request):request_(request) {}
    explicit NativeTileCommandInterrupt(const Gta5TileSceneContinuation& tail)
        :request_(tail.request()),continuation_(tail) {}
    bool execute(std::uint32_t base,std::uint32_t mask,NativeGraphicsState& state,
            Journal& journal,const Fence& fence,const Memory& memory,const Guest& guest) {
        if(phase_!=Phase::Ready){phase_=Phase::Failed;return false;}
        phase_=Phase::Failed; // Every attempt, including an exception, is single-use.
        if(!fence || !memory || !request_.scratchStateMatches(base,mask) ||
           request_.predicated()!=continuation_.has_value())return false;
        std::array<unsigned,10> selected{};unsigned count=10;
        bool interrupt=true;
        if(continuation_) {
            const DrawBinState bins{{static_cast<unsigned>(state.binMask),static_cast<unsigned>(state.binMask>>32),
                static_cast<unsigned>(state.binSelect),static_cast<unsigned>(state.binSelect>>32)},state.knownBinWords};
            const auto selection=continuation_->select(bins);if(!selection)return false;
            selected=selection->operationIndices;count=selection->operationCount;interrupt=selection->interruptRequired;
        } else for(unsigned i=0;i<count;++i)selected[i]=i;
        if(interrupt && !guest)return false;
        // Reserve all journal entries before the first external effect. Existing
        // entries are retained; actions never insert after guest execution starts.
        // Attempt v179: the copy-and-swap (all or nothing if an insertion throws) runs only when a key is
        // missing; after the first interrupt all five exist and the journal is left as it was (v178 copied the
        // whole register journal, a std::map, on every interrupt: 26% of the exclusive span's CPU).
        constexpr std::array<unsigned,5> reserved{0x578u,0x57Cu,0x57Du,0x45Eu,0x5C8u};
        if(std::any_of(reserved.begin(),reserved.end(),[&](unsigned r){return journal.find(r)==journal.end();})) {
            auto prepared=journal;
            for(unsigned r:reserved)prepared.try_emplace(r,std::make_pair(0u,0u));
            journal.swap(prepared);
        }
        try {
            phase_=Phase::Ordering;
            if(continuation_) {
                state.binMask=(state.binMask&0xFFFFFFFF00000000ULL)|0x80000000u;
                state.knownBinWords|=1;
            }
            // For the first command this also orders the preceding maintenance
            // submission. The continuation contains its own WAIT_UNTIL(0x20000).
            if(!fence() || phase_!=Phase::Ordering)return fail();
            ++nativeFences_;
            if(continuation_)journal.find(0x5C8)->second={0x20000,UINT32_MAX};
            phase_=Phase::Prefix;
            for(unsigned n=0;n<count;++n) {
                const auto& op=request_.operations()[selected[n]];
                if(op.kind==Plan::Kind::Interrupt) {
                    phase_=Phase::GuestRunning;
                    if(!guest(request_) || phase_!=Phase::GuestRunning)return fail();
                    ++guestReturns_;phase_=Phase::Acknowledging;
                } else if(op.kind==Plan::Kind::CallbackAcknowledge) {
                    // Type0 ACK is unconditional even when the type3 IRQ is
                    // predicated away. This is register state, NOT a guest zero.
                    journal.find(op.target)->second={op.value,UINT32_MAX};
                } else {
                    const auto before=phase_;
                    if(!memory(op) || phase_!=before)return fail();
                    if(op.kind==Plan::Kind::ScratchWrite)
                        journal.find(0x578+op.target)->second={op.value,UINT32_MAX};
                }
                ++operations_;
            }
            if(continuation_)state.binMask=(state.binMask&0xFFFFFFFF00000000ULL)|UINT32_MAX;
            phase_=Phase::Complete;return true;
        } catch(...) {fail();throw;}
    }
    Phase phase() const noexcept{return phase_;}
    unsigned nativeFences() const noexcept{return nativeFences_;}
    unsigned guestReturns() const noexcept{return guestReturns_;}
    unsigned completedOperations() const noexcept{return operations_;}
private:
    bool fail() noexcept{phase_=Phase::Failed;return false;}
    Plan request_;std::optional<Gta5TileSceneContinuation> continuation_;
    Phase phase_=Phase::Ready;unsigned nativeFences_=0,guestReturns_=0,operations_=0;
};
}
