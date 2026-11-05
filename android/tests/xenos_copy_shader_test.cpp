#include "xenos_copy_shader.h"
#include "xenos_copy_shader_fixture.h"
#include <cassert>
#include <limits>
#include <vector>

using namespace recomp::gpu;
using namespace recomp::gpu::test;
int main() {
    const auto vertex = program({alloc(2),exec(2,1,1),alloc(1),exec(3)},
        {alu(0,0,0,true,true),alu(62,8,8,true,true)});
    const auto pixel = program({alloc(),exec()},{alu()});
    const auto copy = pixel;
    CopyShaderProof vs, ps;
    CopyShaderError error;
    assert(analyzeCopyShader(vertex.data(),vertex.size(),CopyShaderStage::Vertex,vs,error));
    assert(analyzeCopyShader(pixel.data(),pixel.size(),CopyShaderStage::Pixel,ps,error));
    assert(error.failure == CopyShaderFailure::None && vs.controlCount == 4 && vs.executedCount == 2);
    assert(vs.initialComponentsRead.none() && vs.exported.count() == 2 && ps.initialComponentsRead.count() == 4);
    ConstantShaderPair pair;
    assert(linkConstantShaderPair(vs,ps,0x1000000E,0,pair));
    assert((pair.position == std::array<std::uint8_t,4>{{0,0,0,1}}));
    assert((pair.parameter == std::array<std::uint8_t,4>{{0,0,0,0}}));
    assert(pixel == copy);
    for (auto control : {0u,0x1004000Eu,0x1010000Eu}) assert(!linkConstantShaderPair(vs,ps,control,0,pair));
    assert(!linkConstantShaderPair(vs,ps,0x1000000E,1,pair));
    assert(!linkConstantShaderPair(ps,vs,0x1000000E,0,pair));
    assert(pair.position[3] == 1);
    const auto reject = [&](const Bytes& data, CopyShaderStage stage = CopyShaderStage::Pixel) {
        auto previous = ps;
        assert(!analyzeCopyShader(data.data(),data.size(),stage,previous,error));
        assert(error.failure != CopyShaderFailure::None && previous.byteCount == ps.byteCount &&
            previous.exported == ps.exported && previous.exports == ps.exports && previous.executedCount == ps.executedCount);
    };
    reject({}); reject({1}); reject(Bytes(4104)); reject(pixel,static_cast<CopyShaderStage>(2));
    assert(!analyzeCopyShader(nullptr,24,CopyShaderStage::Pixel,ps,error));
    assert(!analyzeCopyShader(pixel.data(),std::numeric_limits<std::size_t>::max(),CopyShaderStage::Pixel,ps,error));
    for (const auto& cf : std::vector<std::vector<std::uint64_t>>{{alloc(3),exec()}, {alloc(1),exec()},
        {alloc(2,5),exec()}, {alloc(),exec(0)}, {alloc(),exec(2)}, {alloc(),exec(1,0)},
        {alloc(),exec(1,7)}, {alloc(),exec() | (1u << 16)}, {alloc(),exec() | (4u << 16)},
        {alloc(),exec(1,1,3)}, {alloc(),exec(1,1,1)}, {alloc() | (1u << 20),exec()},
        {alloc(),exec() | (std::uint64_t{1} << 35)}, {1,exec()}}) reject(program(cf,{alu()}));
    reject(program({alloc(),alloc(),exec(2)},{alu()}));
    reject(program({alloc(1,2),exec()},{alu(62)}),CopyShaderStage::Vertex);
    for (const auto& mutation : std::array<std::array<unsigned,2>,8>{{{0,6},{0,7},{0,24},{0,25},{1,24},{1,28},{1,30},{2,30}}}) {
        auto words = alu(); words[mutation[0]] ^= 1u << mutation[1]; reject(program({alloc(),exec()},{words}));
    }
    for (const auto& words : {alu(32),alu(0,15,0,true,false,64),alu(0,0,1),alu(0,15,0,false,true)})
        reject(program({alloc(),exec()},{words}));
    auto mismatched = alu(); mismatched[2] ^= 1 << 8; reject(program({alloc(),exec()},{mismatched}));
    mismatched = alu(); mismatched[2] ^= 1 << 24; reject(program({alloc(),exec()},{mismatched}));
    reject(program({exec()},{alu(0,15,0,false)}));
    // Every export lane mask, including a full constant output with no reads.
    for (unsigned mask = 0; mask < 16; ++mask) {
        const auto data = program({alloc(),exec()},{alu(0,mask,mask,true,true)});
        CopyShaderProof proof;
        assert(analyzeCopyShader(data.data(),data.size(),CopyShaderStage::Pixel,proof,error));
        assert(proof.initialComponentsRead.none());
        for (unsigned i = 0; i < 4; ++i) assert(proof.exports[0][i] == (mask & (1 << i) ? CopyShaderProof::One : CopyShaderProof::Zero));
    }
    // Every swizzle, across all source registers, uses a simultaneous snapshot.
    for (unsigned r = 0; r < 64; ++r) for (unsigned swizzle = 0; swizzle < 256; ++swizzle) {
        const auto data = program({alloc(),exec(1,2)}, {alu(r,15,0,false,false,r,swizzle),alu(0,15,0,true,false,r)});
        CopyShaderProof proof;
        assert(analyzeCopyShader(data.data(),data.size(),CopyShaderStage::Pixel,proof,error));
        for (unsigned i = 0; i < 4; ++i)
            assert(proof.exports[0][i] == CopyShaderProof::Initial + r*4 + (((swizzle >> (2*i)) + i) & 3));
    }
    // Boundary counts: exactly 128 controls / 256 ALUs succeed, one more rejects.
    std::vector<std::uint64_t> controls(128,0); controls[0] = alloc(); controls[127] = exec(64);
    auto data = program(controls,{alu()}); CopyShaderProof proof;
    assert(analyzeCopyShader(data.data(),data.size(),CopyShaderStage::Pixel,proof,error) && proof.controlCount == 128);
    controls.resize(130); controls[127] = 0; controls[129] = exec(65);
    reject(program(controls,{alu()}));
    controls = {alloc()};
    for (unsigned i = 0; i < 42; ++i) controls.push_back(exec(23,6,1));
    controls.push_back(exec(23,4));
    std::vector<Words> body(7,alu()); // Pair padding occupies index 22; body starts at 23.
    data = program(controls,body);
    assert(analyzeCopyShader(data.data(),data.size(),CopyShaderStage::Pixel,proof,error) && proof.executedCount == 256);
    controls.back() = exec(23,5); reject(program(controls,body));
    // Deterministic hostile streams exercise byte/range validation under UBSan.
    std::uint32_t seed = 0x41C6CE57;
    for (unsigned n = 0; n < 10000; ++n) {
        Bytes fuzz((n % 340 + 1) * 12);
        for (auto& byte : fuzz) { seed = seed*1664525u + 1013904223u; byte = seed >> 24; }
        (void)analyzeCopyShader(fuzz.data(),fuzz.size(),n & 1 ? CopyShaderStage::Vertex : CopyShaderStage::Pixel,proof,error);
    }
}
