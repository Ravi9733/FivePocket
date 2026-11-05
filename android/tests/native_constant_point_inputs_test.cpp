#include "native_constant_point_inputs.h"
#include <cassert>
#include <cstring>

using namespace recomp::gpu;
int main() {
    NativeConstantPointInputs output;
    for (unsigned position = 0; position < 16; ++position) for (unsigned parameter = 0; parameter < 16; ++parameter) {
        ConstantShaderPair pair;
        for (unsigned i = 0; i < 4; ++i) { pair.position[i] = (position >> i) & 1; pair.parameter[i] = (parameter >> i) & 1; }
        assert(packNativeConstantPointInputs(pair,output));
        std::array<std::uint32_t,8> words{}; std::memcpy(words.data(),&output,sizeof(output));
        for (unsigned i = 0; i < 4; ++i) {
            assert(words[i] == (position & (1 << i) ? 0x3F800000u : 0u));
            assert(words[i+4] == (parameter & (1 << i) ? 0x3F800000u : 0u));
        }
    }
    ConstantShaderPair pair{{0,0,0,1},{0,0,0,0}};
    assert(packNativeConstantPointInputs(pair,output));
    const auto previous = output;
    for (unsigned component = 0; component < 8; ++component) for (unsigned invalid : {2,127,255}) {
        auto bad = pair;
        (component < 4 ? bad.position[component] : bad.parameter[component-4]) = invalid;
        assert(!packNativeConstantPointInputs(bad,output));
        assert(std::memcmp(&output,&previous,sizeof(output)) == 0);
    }
}
