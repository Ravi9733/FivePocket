// display-rate vote (v522): what the presenter asks of Android for its window, as a pure rule (host-tested:
// local-analysis/agents/fr-vote/tests). The presenter looks at two switch files every 120 presents and tells the window
// (ANativeWindow_setFrameRate) when this says so.
#pragma once
#include <cstdint>

namespace recomp::frv {

// The rate asked for: 120 with diag-fr-120, else 60 with diag-fr-60, else 0 (no preference: the state of every build before).
constexpr unsigned want(bool has120,bool has60) noexcept {return has120?120u:has60?60u:0u;}

// What the window was last told, and which window that was (the app's window generation; a replaced window knows nothing).
struct Vote {
    unsigned rate=0;std::uint64_t gen=~std::uint64_t(0);
    // Must the window be told now? When the rate asked for differs from the one in force, or the window is another one and
    // there is a rate to tell it. A new window is never told "no preference": that is what it starts with.
    constexpr bool due(unsigned wanted,std::uint64_t windowGen) const noexcept {return wanted!=rate || (wanted && windowGen!=gen);}
    // The call's result (0 = accepted). A refused call leaves the state, so the next poll asks again.
    constexpr void told(unsigned wanted,std::uint64_t windowGen,int result) noexcept {if(result==0){rate=wanted;gen=windowGen;}}
};

} // namespace recomp::frv
