#include "xenos_stopped_shader_capture.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
using namespace recomp::gpu;
int main() {
    const std::vector<std::uint8_t> code(12, 0xA5);
    StoppedGpuWork work;
    work.shaders = {{1,{137,0,3,true,0,code}}, {1,{2292,1,3,false,0x1000,{}}},
                    {1,{2644,0,3,false,0x2000,{}}}};
    work.draws.push_back({1,{234,0x36,3,8,2,0,false,false,false,true}});
    std::vector<std::vector<std::uint8_t>> output{{0x17}};
    const auto sentinel = output;
    unsigned calls = 0;
    const Pm4PhysicalReader reader = [&](auto address, auto bytes) {
        ++calls; assert(bytes == 12); assert(address == 0x1000 || address == 0x2000);
        return std::optional<std::vector<std::uint8_t>>(code);
    };
    assert(captureStoppedShaderBytes(work, reader, output));
    assert(calls == 2 && output == std::vector<std::vector<std::uint8_t>>(3, code));
    output[0][0] = 0; assert(work.shaders[0].load.inlineBigEndianBytes[0] == 0xA5);
    assert(work.draws[0].draw.predicated); // Inspection never clears a predicate.
    unsigned failures = 0;
    for (unsigned position = 1; position <= 2; ++position) for (unsigned mode = 0; mode < 5; ++mode) {
        output = sentinel; calls = 0;
        const Pm4PhysicalReader failing = [&](auto, auto) -> std::optional<std::vector<std::uint8_t>> {
            if (++calls != position) return code;
            if (mode == 0) return std::nullopt;
            if (mode == 1) throw std::runtime_error("reader failed");
            if (mode == 2) throw std::bad_alloc();
            return std::vector<std::uint8_t>(mode == 3 ? 8 : 16, 0);
        };
        assert(!captureStoppedShaderBytes(work, failing, output));
        assert(output == sentinel && calls == position); ++failures;
    }
    unsigned guards = 0;
    for (unsigned mutation = 0; mutation < 13; ++mutation) {
        auto bad = work; auto& s = bad.shaders[1].load;
        switch (mutation) {
        case 0: s.stage = 2; break;
        case 1: s.instructionWords = 0; break;
        case 2: s.instructionWords = 4; break;
        case 3: s.instructionWords = UINT32_MAX; break;
        case 4: s.physicalAddress = 0x1001; break;
        case 5: s.physicalAddress = 0x20000000; break;
        case 6: s.physicalAddress = 0x1FFFFFF8; break;
        case 7: s.inlineBigEndianBytes = code; break;
        case 8: bad.shaders[0].load.physicalAddress = 0x1000; break;
        case 9: bad.shaders[0].load.inlineBigEndianBytes.pop_back(); break;
        case 10: bad.shaders.resize(9, bad.shaders[0]); break;
        case 11: s.instructionWords = 1023; break; // 4092 + previous12 exceeds cap.
        case 12: s.physicalAddress = 0xFFFFFFFC; break;
        }
        output = sentinel; calls = 0;
        assert(!captureStoppedShaderBytes(bad, reader, output));
        assert(output == sentinel && !calls); ++guards;
    }
    output = sentinel; assert(!captureStoppedShaderBytes(work, {}, output)); assert(output == sentinel);
    work.shaders.resize(1);
    assert(captureStoppedShaderBytes(work, {}, output) && output[0] == code);
    work.shaders[0].load.instructionWords = 1023;
    work.shaders[0].load.inlineBigEndianBytes.resize(4092, 0x31);
    assert(captureStoppedShaderBytes(work, {}, output) && output[0].size() == 4092);
    work.shaders.clear(); assert(captureStoppedShaderBytes(work, {}, output) && output.empty());
    std::cout << "STOPPED_SHADER_CAPTURE_V1 reader_failures=" << failures << " guards=" << guards
              << " external_snapshots=2 executed=0\n";
}
