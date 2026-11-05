#include "native_gpu_word_readback.h"
#include <cassert>
#include <limits>

using namespace recomp::gpu;
int main() {
    const std::array<NativeWordRequest,3> requests{{{4,0x12345678,NativeWordPhase::BeforeGraphics},
        {12,0x90ABCDEF,NativeWordPhase::BeforeGraphics},{4,0x01234567,NativeWordPhase::AfterGraphics}}};
    NativeWordUpdatePlan plan;
    assert(planNativeWordUpdates(requests.data(),requests.size(),16,plan));
    const std::array<std::uint8_t,16> expected{{0xAA,0xAA,0xAA,0xAA, 1,0x23,0x45,0x67,
        0xAA,0xAA,0xAA,0xAA, 0x90,0xAB,0xCD,0xEF}};
    AuditedNativeWords result; NativeWordReadbackFailure error;
    assert(auditNativeWordReadback(plan,expected.data(),expected.size(),result,error));
    assert(error == NativeWordReadbackFailure::None && result.count == 2 && result.words[0].byteOffset == 4 &&
        result.words[0].bigEndianBytes[0] == 1 && result.words[1].byteOffset == 12);
    const auto unchanged = [&] { assert(result.count == 2 && result.words[0].bigEndianBytes[0] == 1 && result.words[1].byteOffset == 12); };
    for (unsigned offset : {4,5,6,7,12,13,14,15}) for (unsigned bit = 0; bit < 8; ++bit) {
        auto corrupt = expected; corrupt[offset] ^= 1 << bit;
        assert(!auditNativeWordReadback(plan,corrupt.data(),corrupt.size(),result,error));
        assert(error == NativeWordReadbackFailure::Mismatch); unchanged();
    }
    auto intermediate = expected; intermediate[4] = 0x12; intermediate[5] = 0x34; intermediate[6] = 0x56; intermediate[7] = 0x78;
    assert(!auditNativeWordReadback(plan,intermediate.data(),intermediate.size(),result,error)); unchanged();
    for (std::size_t size : {std::size_t{0},std::size_t{15},std::size_t{17},std::numeric_limits<std::size_t>::max()}) {
        assert(!auditNativeWordReadback(plan,expected.data(),size,result,error));
        assert(error == NativeWordReadbackFailure::InvalidReadback); unchanged();
    }
    assert(!auditNativeWordReadback(plan,nullptr,16,result,error)); unchanged();
    for (unsigned field = 0; field < 8; ++field) {
        auto bad = plan;
        switch (field) {
        case 0: bad.count = 65; break;
        case 1: bad.bufferBytes = 0; break;
        case 2: bad.words[0].byteOffset = 1; break;
        case 3: bad.words[1].byteOffset = 16; break;
        case 4: bad.words[0].phase = NativeWordPhase::AfterGraphics; break;
        case 5: bad.words[1].byteOffset = 4; break;
        case 6: bad.words[0].phase = static_cast<NativeWordPhase>(2); break;
        case 7: bad.words[1].byteOffset = 0xFFFFFFFC; break;
        }
        assert(!auditNativeWordReadback(bad,expected.data(),expected.size(),result,error));
        assert(error == NativeWordReadbackFailure::InvalidPlan); unchanged();
    }
    // Unaddressed bytes aren't an audit success claim: caller retains a
    // separate full-buffer canary check, as the real Vulkan probe does.
    auto unrelated = expected; unrelated[0] ^= 1;
    assert(auditNativeWordReadback(plan,unrelated.data(),unrelated.size(),result,error));
    std::array<NativeWordRequest,64> boundary{}; std::array<std::uint8_t,256> bytes{};
    for (unsigned i = 0; i < 64; ++i) { boundary[i] = {i*4,i,NativeWordPhase::AfterGraphics}; bytes[i*4+3] = i; }
    assert(planNativeWordUpdates(boundary.data(),boundary.size(),256,plan));
    assert(auditNativeWordReadback(plan,bytes.data(),bytes.size(),result,error) && result.count == 64);
    assert(planNativeWordUpdates(nullptr,0,16,plan));
    assert(auditNativeWordReadback(plan,expected.data(),expected.size(),result,error) && result.count == 0);
}
