#include "native_gpu_word_updates.h"
#include <cassert>
#include <limits>

using namespace recomp::gpu;
int main() {
    using Phase = NativeWordPhase;
    NativeWordUpdatePlan plan;
    const std::array<NativeWordRequest, 3> input{{{4,0x12345678,Phase::BeforeGraphics},
        {60,0x89ABCDEF,Phase::BeforeGraphics},{4,0x10203040,Phase::AfterGraphics}}};
    assert(planNativeWordUpdates(input.data(),input.size(),64,plan));
    const std::array<std::uint8_t,4> first{0x12,0x34,0x56,0x78}, last{0x10,0x20,0x30,0x40};
    assert(plan.count == 3 && plan.bufferBytes == 64 && plan.words[0].bigEndianBytes == first &&
        plan.words[2].bigEndianBytes == last && input[0].value == 0x12345678);
    const auto reject = [&](const NativeWordRequest* source, std::size_t count, std::uint32_t bytes) {
        assert(!planNativeWordUpdates(source,count,bytes,plan));
        assert(plan.count == 3 && plan.words[0].bigEndianBytes == first);
    };
    reject(nullptr,1,64); reject(input.data(),65,64);
    reject(input.data(),std::numeric_limits<std::size_t>::max(),64);
    for (auto bytes : {0u,3u,63u,4u*1024*1024+4u,0xFFFFFFFCu}) reject(input.data(),input.size(),bytes);
    for (auto offset : {1u,61u,64u,0xFFFFFFFCu,0xFFFFFFFFu}) {
        auto bad = input; bad[1].byteOffset = offset; reject(bad.data(),bad.size(),64);
    }
    auto bad = input; bad[1].byteOffset = 4; reject(bad.data(),bad.size(),64);
    bad = input; bad[0].phase = Phase::AfterGraphics; reject(bad.data(),bad.size(),64);
    bad = input; bad[1].phase = static_cast<Phase>(2); reject(bad.data(),bad.size(),64);
    std::array<NativeWordRequest,64> limit{};
    for (std::size_t i = 0; i < limit.size(); ++i) limit[i].byteOffset = i * 4;
    assert(planNativeWordUpdates(limit.data(),limit.size(),256,plan) && plan.count == 64);
    assert(planNativeWordUpdates(nullptr,0,4,plan) && plan.count == 0);
    const NativeWordRequest end{4*1024*1024-4,0xFFFFFFFF,Phase::AfterGraphics};
    assert(planNativeWordUpdates(&end,1,4*1024*1024,plan));
    for (auto byte : plan.words[0].bigEndianBytes) assert(byte == 255);
    // Reconstruct independent random values from the four output bytes.
    std::uint32_t seed = 0xC001D00D;
    for (int i = 0; i < 10000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const NativeWordRequest request{0,seed,Phase::BeforeGraphics};
        assert(planNativeWordUpdates(&request,1,4,plan));
        std::uint32_t decoded = 0;
        for (auto byte : plan.words[0].bigEndianBytes) decoded = decoded * 256u + byte;
        assert(decoded == seed);
    }
}
