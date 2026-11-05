#include "native_visibility_queries.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
using namespace recomp::gpu;
using Measurement=NativeVisibilityQueries::Measurement;
using Stage=NativeVisibilityQueries::Stage;
// Authored state-machine inputs, NOT measurements of Xenos hardware.
static std::optional<Measurement> potential(bool visible) {
    return Measurement{visible,Stage::PotentialVisibility};
}
int main() {
    unsigned cases=0;
    for(unsigned id=0;id<64;++id) {
        NativeVisibilityQueries q;NativeVisibilityQueries::Journal j{{0x2104,{0x1234,~0u}}};
        assert(q.idle() && q.condition(0)==true && !q.condition(0x100|id));
        const auto bank=0xC44+(id>>5),bit=1u<<(id&31);
        assert(q.command(id,j) && !q.idle() && j.at(0x21F9).first==7);
        assert(q.condition(0x100|id)==true && !j.count(bank));
        assert(q.draw(id*2+1,[]{return potential(false);}));
        assert(q.draw(id*2+1,[]{return potential(true);}));
        assert(q.draw(id*2+1,[]{return potential(false);}));
        assert(!j.count(bank)); // No early readable result publication.
        assert(q.command(0x100|id,j) && q.idle() && q.condition(0x100|id)==true);
        assert(j.at(bank).first==bit && j.at(bank).second==bit && j.at(0x21F9).first==8);
        assert(j.at(0x2104).first==0x1234);
        const auto completed=j.at(bank);
        assert(q.command(id,j) && j.at(bank)==completed && q.condition(0x100|id)==true);
        assert(q.draw(id*2+1,[]{return potential(false);}));
        assert(q.command(0x100|id,j) && q.condition(0x100|id)==false);
        assert(j.at(bank).first==0 && j.at(bank).second==bit);
        // New begin clears CP DISCARD although SC still exposes hidden.
        assert(q.command(id,j) && j.at(bank).first==0 && q.condition(0x100|id)==true);
        assert(q.command(0x100|id,j) && q.condition(0x100|id)==false);++cases;
    }
    { NativeVisibilityQueries q;NativeVisibilityQueries::Journal j;
      assert(q.command(8,j) && q.command(9,j));
      assert(q.draw(17,[]{return potential(true);}));
      assert(q.draw(19,[]{return potential(false);}));
      assert(q.command(0x109,j) && q.command(0x108,j));
      assert(q.condition(0x108)==true && q.condition(0x109)==false && q.idle());++cases;
    }
    for(unsigned mode=0;mode<9;++mode) {
        NativeVisibilityQueries q;NativeVisibilityQueries::Journal j;
        assert(q.command(8,j));const auto before=j;unsigned called=0;
        const auto good=[&](){++called;return potential(true);};
        switch(mode) {
        case 0:assert(!q.command(8,j));break;
        case 1:assert(!q.command(0x109,j));break;
        case 2:assert(!q.command(0x208,j));break;
        case 3:assert(!q.draw(19,good));break;
        case 4:assert(!q.draw(0x91,good));break;
        case 5:assert(!q.draw(0x111,good));break;
        case 6:assert(!q.draw(17,{}));break;
        case 7:assert(!q.draw(17,[]{return std::optional<Measurement>{};}));break;
        case 8:assert(!q.draw(17,[]()->std::optional<Measurement>{throw std::runtime_error("draw failed");}));break;
        }
        assert(!called && q.failed() && !q.idle() && j==before);
        assert(!q.command(0x108,j) && !q.condition(0x108));++cases;
    }
    for(unsigned mode=0;mode<2;++mode) {
        NativeVisibilityQueries q;NativeVisibilityQueries::Journal j;assert(q.command(8,j));const auto before=j;
        assert(!q.draw(17,[&]()->std::optional<Measurement>{
            assert(!q.condition(0x108)); // Reentrant observation isn't completion.
            if(mode)assert(!q.draw(17,[]{return potential(true);}));
            else assert(!q.command(0x108,j));
            return potential(true);
        }));
        assert(q.failed() && j==before);++cases;
    }
    { NativeVisibilityQueries q;NativeVisibilityQueries::Journal j{{0xC44,{~0u,~0u}}};
      assert(!q.condition(0x108)); // SC register alone is not CP ownership.
      assert(q.command(8,j) && q.command(0x108,j));
      assert(j.at(0xC44).first==(~0u^(1u<<8)) && j.at(0xC44).second==~0u);
      assert(!q.condition(8) && !q.condition(0x208));
      assert(q.draw(16,[]{return potential(false);}) && q.idle());++cases;
    }
    for(bool visible:{false,true}) {
        NativeVisibilityQueries q;NativeVisibilityQueries::Journal j;assert(q.command(8,j));const auto before=j;
        assert(!q.draw(17,[&](){return std::optional<Measurement>{{visible,Stage::FinalSamples}};}));
        assert(q.failed() && j==before && !q.command(0x108,j) && !q.condition(0x108));++cases;
    }
    { NativeVisibilityQueries q;
      assert(q.draw(16,[]{return std::optional<Measurement>{{false,Stage::FinalSamples}};}) && q.idle());++cases;
    }
    assert(cases==80);
    std::printf("NATIVE_VISIBILITY_LIFECYCLE_PASS cases=%u ids=64 unknown_bits_preserved=1 result_at_end=1 cp_begin_clears_discard=1 sc_status_retained=1 stages_not_conflated=1 hidden_clears_old_bit=1 callback_failure_latched=1 kill_modes_rejected=1 gpu_execution=0\n",cases);
}
