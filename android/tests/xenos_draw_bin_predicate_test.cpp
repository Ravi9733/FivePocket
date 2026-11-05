#include "xenos_draw_bin_predicate.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    const std::array<std::uint32_t,5> values{0,1,2,0x80000000,0xFFFFFFFF};
    unsigned cases=0;
    // Unknown or five known values for each of four words: 6^4 states. Enumerate
    // all zero/all-one completions of unknowns. AND/OR monotonicity makes those
    // endpoints sufficient to determine whether the truth value is fixed.
    for(unsigned encoded=0;encoded<1296;++encoded){
        unsigned remaining=encoded;DrawBinState state;
        for(unsigned i=0;i<4;++i){const unsigned value=remaining%6;remaining/=6;
            if(value){state.knownWords|=1u<<i;state.words[i]=values[value-1];}}
        bool canPass=false,canSkip=false;
        for(unsigned fill=0;fill<16;++fill){auto words=state.words;
            for(unsigned i=0;i<4;++i)if(!(state.knownWords&(1u<<i)))words[i]=fill&(1u<<i)?0xFFFFFFFF:0;
            const bool pass=(words[0]&words[2]) || (words[1]&words[3]);canPass|=pass;canSkip|=!pass;
        }
        const auto expected=canPass?(canSkip?DrawBinDecision::Unknown:DrawBinDecision::Pass):DrawBinDecision::Skip;
        assert(evaluateDrawBinPredicate(state,true)==expected);
        assert(evaluateDrawBinPredicate(state,false)==DrawBinDecision::Pass);cases+=2;
        for(unsigned i=0;i<4;++i)if(!(state.knownWords&(1u<<i))){auto bad=state;bad.words[i]=1;
            assert(evaluateDrawBinPredicate(bad,true)==DrawBinDecision::InvalidState);
            assert(evaluateDrawBinPredicate(bad,false)==DrawBinDecision::InvalidState);}
    }
    for(unsigned mask:{16u,31u,0xFFFFFFFFu}){
        DrawBinState bad{{},mask};assert(evaluateDrawBinPredicate(bad,true)==DrawBinDecision::InvalidState);
    }
    const DrawBinState retained{{0xFFFFFFFF,0,0xFFFFFFFF,0},15};
    assert(evaluateDrawBinPredicate(retained,true)==DrawBinDecision::Pass);
    assert(evaluateDrawBinPredicate({},true)==DrawBinDecision::Unknown);
    std::printf("DRAW_BIN_PREDICATE_V1 reference_cases=%u unknown_is_not_reset_zero=1 guest_draws=0\n",cases);
}
