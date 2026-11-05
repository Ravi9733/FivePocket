#include "native_color_copy_request.h"
#include "xenos_copy_shader_fixture.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
using namespace recomp::gpu;
using namespace recomp::gpu::test;
static Bytes shader(unsigned stride=2,unsigned swizzle=0xB48) {
    const Words fetch{{1u<<19,0x3000u|(37u<<16)|swizzle,stride}};
    return program({alloc(1),exec(1,2)|(1u<<16)},{fetch,alu(62)});
}
static std::vector<std::uint32_t> encode(const NativeColorCopyRequest& request) {
    std::vector<std::uint32_t> out{request.copy.source.surface,request.copy.source.info,
        request.copy.source.width,request.copy.source.height,unsigned(request.copy.source.depth),
        request.copy.destinationBase,request.copy.capacity,request.copy.touchedSpan,request.copy.pixelBytes,
        unsigned(request.copy.swapRedBlue),request.drawOffset,request.vertexPhysicalAddress};
    for (auto b:request.vertexBytes) out.push_back(b);return out;
}
int main() {
    DrawCommandPlan plan;plan.shaders.push_back({0,0,0,true,shader()});
    DrawCommandCandidate draw;draw.offsetWords=100;draw.opcode=0x36;draw.indexCount=3;draw.primitive=8;draw.shaders[0]=0;
    for (const auto& pair:std::array<std::array<unsigned,2>,12>{{
        {{0x2208,6}},{{0x2081,0}},{{0x2082,0x2D00500}},{{0x2301,0}},{{0x2000,0x14000500}},
        {{0x2001,0xC02D0}},{{0x2318,0x100000}},{{0x2319,0x1000000}},{{0x231A,0x2D00500}},
        {{0x231B,0x1000300}},{{0x4800,0x1003}},{{0x4801,0x1000001A}}}}) draw.registers[pair[0]].bits.assign(pair[1]);
    plan.draws.push_back(draw);Bytes vertices;
    for (float f:{-.5f,-.5f,1279.5f,-.5f,1279.5f,719.5f}) {
        std::uint32_t w=0;std::memcpy(&w,&f,4);for (unsigned b=0;b<4;++b) vertices.push_back(w>>(24-b*8));
    }
    unsigned reads=0,rejects=0;
    const Pm4PhysicalReader reader=[&](auto address,auto size)->std::optional<Bytes> {
        ++reads;assert(address==0x1000 && size==24);return vertices;
    };
    NativeColorCopyRequest output;
    assert(planNativeColorCopyRequest(plan,0,reader,output) && reads==1);
    assert(output.copy.capacity==3768320 && output.copy.pixelBytes==3686400 && output.copy.swapRedBlue);
    assert(output.vertexPhysicalAddress==0x1000 && output.drawOffset==100);
    const auto saved=encode(output);
    const auto reject=[&](const DrawCommandPlan& bad,unsigned index=0) {
        const auto prior=reads;assert(!planNativeColorCopyRequest(bad,index,reader,output));
        assert(reads==prior && encode(output)==saved);++rejects;
    };
    reject(plan,1);reject(plan,UINT32_MAX);
    for (const auto& [index,r]:draw.registers) {
        (void)r;auto bad=plan;bad.draws[0].registers.erase(index);reject(bad);
        for (unsigned bit=0;bit<32;++bit) {
            bad=plan;bad.draws[0].registers[index].bits.known^=1u<<bit;reject(bad);
        }
        if (index!=0x2319 && index!=0x4800) {bad=plan;bad.draws[0].registers[index].bits.value^=1;reject(bad);}
    }
    for (unsigned mutation=0;mutation<11;++mutation) {
        auto bad=plan;auto& d=bad.draws[0];
        if (mutation==0) ++d.indexCount;
        if (mutation==1) d.primitive=6;
        if (mutation==2) d.opcode=0x10;
        if (mutation==3) d.shaders[0]=8;
        if (mutation==4) d.offsetWords=0;
        if (mutation==5) bad.shaders[0].stage=1;
        if (mutation==6) bad.shaders[0].bigEndianCode.clear();
        if (mutation==7) bad.shaders[0].bigEndianCode=shader(3);
        if (mutation==8) bad.shaders[0].bigEndianCode=shader(2,0xB40); // y becomes x.
        if (mutation==9) bad.shaders[0].bigEndianCode=shader(2,0x948); // z becomes0, not admitted1.
        if (mutation==10) bad.shaders.resize(9);
        reject(bad);
    }
    for (unsigned known:{0u,15u,16u}) {auto bad=plan;bad.draws[0].predicate={{{},known},true};reject(bad);}
    for (unsigned index:{0x4800u,0x2319u}) for (unsigned v:{0x20000003u,0x1FFFFFFFu}) {
        auto bad=plan;bad.draws[0].registers[index].bits.value=v;reject(bad);
    }
    assert(!planNativeColorCopyRequest(plan,0,{},output) && encode(output)==saved);++rejects;
    // All 192 bits of the source rectangle are significant, including raw float
    // encodings before any host/GPU arithmetic could round/flush a difference.
    const auto original=vertices;
    for (unsigned byte=0;byte<24;++byte) for (unsigned bit=0;bit<8;++bit) {
        vertices=original;vertices[byte]^=1u<<bit;
        assert(!planNativeColorCopyRequest(plan,0,reader,output) && encode(output)==saved);++rejects;
    }
    vertices=original;
    for (unsigned mode=0;mode<5;++mode) {
        const Pm4PhysicalReader bad=[mode](auto,auto)->std::optional<Bytes> {
            if (mode==0) return std::nullopt;
            if (mode==1) return Bytes(23);
            if (mode==2) return Bytes(25);
            if (mode==3) throw std::runtime_error("synthetic source failure");
            throw std::bad_alloc();
        };
        assert(!planNativeColorCopyRequest(plan,0,bad,output) && encode(output)==saved);++rejects;
    }
    auto predicated=plan;predicated.draws[0].predicate={{{{1,0,1,0}},15},true};
    assert(planNativeColorCopyRequest(predicated,0,reader,output));
    std::fill(vertices.begin(),vertices.end(),0);assert(encode(output)==saved);
    std::printf("NATIVE_COLOR_COPY_REQUEST: %u rejection controls; proven float2 copy dataflow and full-origin rectangle PASS\n",rejects);
}
