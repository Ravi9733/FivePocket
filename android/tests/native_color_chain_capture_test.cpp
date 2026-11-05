#include "native_color_chain_capture.h"
#include "native_color_viewport_fixture.h"
#include "native_color_render_state_fixture.h"
#include "native_color_vertex_geometry_fixture.h"
#include "native_depth_clear_fixture.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <stdexcept>
using namespace recomp::gpu;
using Bytes=std::vector<std::uint8_t>;
namespace {
struct Fixture {
    DrawCommandPlan plan;
    std::array<Bytes,7> raw;
    std::array<std::uint32_t,7> addresses{{0x1000,0x2000,0x3000,0x1000000,0x1100000,0x1200000,0x2000000}};
    std::optional<NativeColorChainProgram> program;
    unsigned reads=0;
    static void assign(DrawRegisterCandidates& regs,unsigned at,unsigned value) {regs[at].bits.assign(value);}
    explicit Fixture(bool mapped=false) {
        const auto module=[](unsigned model) {return std::vector<std::uint32_t>{0x07230203,0x10300,0,16,0,
            (2u<<16)|17,1,(5u<<16)|15,model,1,0x6E69616D,0};}; // Framing fixture, NOT executable SPIR-V.
        NativeColorChainProgram::Translations translations;
        for (unsigned i=0;i<5;++i) {
            const unsigned stage=i==2 || i==4?1:0;
            const Bytes code=mapped && !stage?(i?test::colorVertexShader(i==1):test::depthClearVertexShader()):Bytes(12,static_cast<std::uint8_t>(i+1));
            translations[i]={{stage,code},module(stage?4:0)};
            plan.shaders.push_back({i*10,stage,0,true,code});
        }
        NativeColorChainProgram::Support support;for (auto& code:support) code=module(5);
        program=NativeColorChainProgram::create(translations,support,1,mapped?
            NativeColorCoordinateProfile::Padded1280W1:NativeColorCoordinateProfile::DiagnosticPositive);assert(program);
        for (unsigned i=0;i<3;++i) {
            DrawCommandCandidate draw;draw.offsetWords=100+i*100;draw.opcode=i?0x22:0x36;
            draw.indexCount=i?4:3;draw.primitive=i?6:8;
            draw.shaders=i?std::array<std::uint32_t,2>{{i*2-1,i*2}}:std::array<std::uint32_t,2>{{0,NoDrawCommandDetail}};
            draw.predicate={{{{1,0,1,0}},15},true};
            assign(draw.registers,0x2208,i?4:5);
            const unsigned slot=i?95:0,bytes=i?144:84;
            assign(draw.registers,0x4800+slot*2,addresses[i]|3);assign(draw.registers,0x4801+slot*2,0x10000002|bytes);
            raw[i].resize(bytes);for (unsigned b=0;b<bytes;++b) raw[i][b]=static_cast<std::uint8_t>(b*19+i*31);
            plan.draws.push_back(draw);
        }
        for (unsigned i=0;i<3;++i) {
            const unsigned size=i?276480:921600;
            raw[i+3].resize(size);
            for (unsigned n=0;n<size;++n) raw[i+3][n]=static_cast<std::uint8_t>(n*7+i*41);
            const std::array<unsigned,6> words{{i?0x06024802u:0x0A024802u,addresses[i+3]|2,
                i?0x002CE27Fu:0x0059E4FFu,0x00281400,3,0x210}};
            for (unsigned w=0;w<6;++w) assign(plan.draws[1].registers,0x4800+i*6+w,words[w]);
        }
        raw[6].resize(4096);for (unsigned i=0;i<4096;++i) raw[6][i]=static_cast<std::uint8_t>(i*31);
        const std::array<unsigned,6> tile{{0x80400002,addresses[6]|0x86,0x3E01F,0xA80C14,3,0x210}};
        for (unsigned w=0;w<6;++w) assign(plan.draws[2].registers,0x480C+w,tile[w]);
        const auto constants=[&](unsigned draw,unsigned base,std::initializer_list<unsigned> indices) {
            for (auto c:indices) for (unsigned w=0;w<4;++w) assign(plan.draws[draw].registers,base+c*4+w,0xFEDC0000u^(c<<5)^w);
        };
        constants(1,0x4000,{8,9,10,11,36});constants(1,0x4400,{32,33,34,35,255});
        constants(2,0x4000,{8,9,10,11});constants(2,0x4400,{16,62});
        if(mapped)for(unsigned i=1;i<3;++i)for(const auto& pair:test::colorRenderStateDraw(i-1).registers)
            plan.draws[i].registers[pair.first]=pair.second;
        if(mapped)for(unsigned i=1;i<3;++i)raw[i]=test::colorVertexSource();
        if(mapped){
            auto depth=test::depthClearStatePlan();
            for(const auto& pair:depth.draws[0].registers)plan.draws[0].registers[pair.first]=pair.second;
            plan.draws[0].shaders[1]=static_cast<unsigned>(plan.shaders.size());plan.shaders.push_back(depth.shaders[1]);
            raw[0]=test::depthClearVertexSource();
        }
    }
    Pm4PhysicalReader reader() {return [&](auto address,auto size)->std::optional<Bytes> {
        ++reads;for (unsigned i=0;i<7;++i) if (address==addresses[i] && size==raw[i].size()) return raw[i];
        return std::nullopt;
    };}
};
}
int main() {
    for(bool mappedProfile:{false,true}){
        Fixture literal(mappedProfile);
        const std::array<std::uint32_t,6> words{0x02024802,2,0,0x00281249,3,0x210};
        for(unsigned w=0;w<6;++w)Fixture::assign(literal.plan.draws[2].registers,0x480C+w,words[w]);
        const auto reader=literal.reader();
        auto capturedZero=NativeColorChainCapture::create(literal.plan,{0,1,2},*literal.program,
            [&](auto address,auto size){assert(address!=0 && address!=literal.addresses[6]);return reader(address,size);});
        assert(capturedZero && literal.reads==6 && capturedZero->sourceRanges().count==6 &&
            capturedZero->sourceRanges().constantZeroTextures==1);
        NativeColorChainBindings zeroBindings;assert(capturedZero->bindings(zeroBindings));
        assert(color_chain_inputs::constantZeroTexture(zeroBindings.pairs[1].textures[0]));
        PackedNativeColorChainInputs zeroPacked;assert(capturedZero->pack(zeroPacked));
        assert(zeroPacked.depth==literal.raw[0] &&
            std::equal(literal.raw[2].begin(),literal.raw[2].end(),zeroPacked.pairs[1].begin()+4144));
        assert(std::all_of(zeroPacked.pairs[1].begin(),zeroPacked.pairs[1].begin()+4096,[](auto b){return b==0;}));
        const auto zeroSaved=zeroPacked;auto zeroMoved=std::move(*capturedZero);
        assert(zeroMoved.pack(zeroPacked) && zeroPacked.depth==zeroSaved.depth && zeroPacked.pairs==zeroSaved.pairs);
        assert(!capturedZero->pack(zeroPacked));
        assert(zeroMoved.bindings(zeroBindings) && color_chain_inputs::constantZeroTexture(zeroBindings.pairs[1].textures[0]));
        unsigned rejections=0;
        for(unsigned word=0;word<6;++word)for(unsigned bit=0;bit<32;++bit)for(unsigned mode=0;mode<2;++mode){
            auto invalid=literal.plan;auto& field=invalid.draws[2].registers[0x480C+word].bits;
            if(mode)field.value^=1u<<bit;else field.known^=1u<<bit;
            assert(!NativeColorChainCapture::create(invalid,{0,1,2},*literal.program,literal.reader()));
            assert(literal.reads==6);++rejections;
        }
        std::printf("NATIVE_COLOR_CONSTANT_ZERO_CAPTURE: mapped=%u reads=6 null_reads=0 pre_read_rejections=%u move_lifetime=PASS\n",mappedProfile,rejections);
    }
    Fixture f;auto captured=NativeColorChainCapture::create(f.plan,{0,1,2},*f.program,f.reader());
    assert(captured && f.reads==7 && f.program->matches(captured->active()));
    assert(captured->sourceRanges().count==7 && !captured->sourceRanges().shaderRanges && !captured->sourceRanges().constantRanges);
    NativeColorChainBindings bindings;assert(captured->bindings(bindings));
    PackedNativeColorChainInputs packed;assert(captured->pack(packed));
    assert(packed.depth==f.raw[0] && packed.pairs[0].size()==1382720 && packed.pairs[1].size()==4352);
    assert(std::equal(f.raw[1].begin(),f.raw[1].end(),packed.pairs[0].begin()+1382496));
    assert(std::equal(f.raw[2].begin(),f.raw[2].end(),packed.pairs[1].begin()+4144));
    assert(bindings.depth.source.bytes!=f.raw[0].data());
    const auto saved=packed;unsigned failures=0;
    const auto reject=[&](const DrawCommandPlan& plan,std::array<unsigned,3> indices=std::array<unsigned,3>{0,1,2}) {
        const auto prior=f.reads;
        assert(!NativeColorChainCapture::create(plan,indices,*f.program,f.reader()));
        assert(f.reads==prior);++failures;
    };
    for (auto indices:{std::array<unsigned,3>{1,0,2},std::array<unsigned,3>{0,1,1},
                     std::array<unsigned,3>{0,1,3},std::array<unsigned,3>{0,1,UINT32_MAX}}) reject(f.plan,indices);
    for (unsigned draw=0;draw<3;++draw) {
        for (const auto& [index,r]:f.plan.draws[draw].registers) {
            (void)r;
            auto bad=f.plan;bad.draws[draw].registers.erase(index);reject(bad);
            for (unsigned bit=0;bit<32;++bit) {
                bad=f.plan;bad.draws[draw].registers[index].bits.known^=1u<<bit;reject(bad);
            }
        }
        for (unsigned mutation=0;mutation<10;++mutation) {
            auto bad=f.plan;auto& d=bad.draws[draw];
            if (mutation==0) d.primitive^=1;
            if (mutation==1) ++d.indexCount;
            if (mutation==2) d.opcode=0x10;
            if (mutation==3) d.offsetWords=0;
            if (mutation==4) d.registers[0x2208].bits.value^=1;
            if (mutation==5) d.shaders[0]=8;
            if (mutation==6) d.predicate.state={};
            if (mutation==7) d.predicate.state={{{0,0,0,0}},15};
            if (mutation==8) d.predicate.state.knownWords=16;
            if (mutation==9) d.shaders[0]=NoDrawCommandDetail;
            reject(bad);
        }
        auto bad=f.plan;const unsigned slot=draw?95:0;
        bad.draws[draw].registers[0x4800+slot*2].bits.value=0x1FFFFFFF;reject(bad);
        bad=f.plan;bad.draws[draw].registers[0x4800+slot*2].bits.value^=1;reject(bad);
        bad=f.plan;bad.draws[draw].registers[0x4801+slot*2].bits.value^=4;reject(bad);
    }
    for (unsigned role=0;role<5;++role) {
        for (unsigned byte=0;byte<12;++byte) for (unsigned bit=0;bit<8;++bit) {
            auto bad=f.plan;bad.shaders[role].bigEndianCode[byte]^=1u<<bit;reject(bad);
        }
        auto bad=f.plan;bad.shaders[role].stage^=1;reject(bad);
        bad=f.plan;bad.shaders[role].bigEndianCode.clear();reject(bad);
        bad=f.plan;bad.shaders[role].bigEndianCode.resize(4104);reject(bad);
        bad=f.plan;bad.shaders[role].bigEndianCode.push_back(0);reject(bad);
        bad=f.plan;bad.shaders[role].offsetWords=UINT32_MAX;reject(bad);
    }
    for (unsigned draw=1;draw<3;++draw) for (unsigned slot=draw==1?0:2;slot<3;++slot) {
        auto bad=f.plan;bad.draws[draw].registers[0x4800+slot*6].bits.value^=1;reject(bad);
    }
    auto bad=f.plan;bad.shaders.resize(9);reject(bad);
    bad=f.plan;bad.shaders.clear();reject(bad);
    assert(!NativeColorChainCapture::create(f.plan,{0,1,2},*f.program,{}));++failures;
    // Each source can disappear, shorten, grow or throw after earlier successful
    // reads. Captured earlier outputs still own their original unchanged bytes.
    for (unsigned failAt=0;failAt<7;++failAt) for (unsigned mode=0;mode<5;++mode) {
        unsigned reads=0;
        const Pm4PhysicalReader reader=[&](auto address,auto size)->std::optional<Bytes> {
            const auto i=reads++;assert(address==f.addresses[i] && size==f.raw[i].size());
            if (i!=failAt) return f.raw[i];
            if (mode==0) return std::nullopt;
            if (mode==1) return Bytes(size-1);
            if (mode==2) return Bytes(size+1);
            if (mode==3) throw std::runtime_error("synthetic source failure");
            throw std::bad_alloc();
        };
        assert(!NativeColorChainCapture::create(f.plan,{0,1,2},*f.program,reader));
        assert(reads==failAt+1);++failures;
    }
    f.plan={};for (auto& b:f.raw) std::fill(b.begin(),b.end(),0);
    assert(captured->pack(packed) && packed.depth==saved.depth && packed.pairs==saved.pairs);
    auto moved=std::move(*captured);
    assert(moved.pack(packed) && packed.depth==saved.depth && packed.pairs==saved.pairs);
    assert(!captured->pack(packed) && packed.depth==saved.depth && packed.pairs==saved.pairs);
    assert(moved.bindings(bindings) && bindings.depth.source.size==84);
    std::printf("NATIVE_COLOR_CHAIN_CAPTURE: %u rejection controls; seven owned sources, metadata preflight, move lifetime PASS\n",failures);
    Fixture mappedFixture(true);
    auto mapped=NativeColorChainCapture::create(mappedFixture.plan,{0,1,2},*mappedFixture.program,mappedFixture.reader());
    assert(mapped && mappedFixture.reads==7 && mapped->coordinates()==NativeColorCoordinateProfile::Padded1280W1);
    for(unsigned i=0;i<2;++i) {
        assert(mapped->viewports()[i].drawOffset==mappedFixture.plan.draws[i+1].offsetWords);
        assert((mapped->viewports()[i].nativeViewport==std::array<float,6>{0,0,2048,1024,1,0}));
        assert((mapped->geometry()[i].uniformTintBits==std::array<std::uint32_t,4>{0,0,0,0x3F800000}));
    }
    unsigned mappedRejections=0;
    for(unsigned d=1;d<3;++d)for(const auto& pair:test::colorViewportDraw().registers)for(unsigned mutation=0;mutation<65;++mutation) {
        auto invalid=mappedFixture.plan;
        if(mutation==0)invalid.draws[d].registers.erase(pair.first);
        else if(mutation<=32)invalid.draws[d].registers[pair.first].bits.known^=1u<<(mutation-1);
        else invalid.draws[d].registers[pair.first].bits.value^=1u<<(mutation-33);
        assert(!NativeColorChainCapture::create(invalid,{0,1,2},*mappedFixture.program,mappedFixture.reader()));
        assert(mappedFixture.reads==7);++mappedRejections;
    }
    std::printf("NATIVE_COLOR_CHAIN_CAPTURE_COORDINATES: %u pre-read rejections; two owned viewport requests PASS\n",mappedRejections);
    unsigned stateRejections=0;
    for(unsigned d=1;d<3;++d)for(const auto& pair:test::colorRenderStateDraw(d-1).registers){
        if(test::colorViewportDraw().registers.count(pair.first))continue;
        for(unsigned mutation=0;mutation<65;++mutation){
            auto invalid=mappedFixture.plan;
            if(!mutation)invalid.draws[d].registers.erase(pair.first);
            else if(mutation<=32)invalid.draws[d].registers[pair.first].bits.known^=1u<<(mutation-1);
            else invalid.draws[d].registers[pair.first].bits.value^=1u<<(mutation-33);
            assert(!NativeColorChainCapture::create(invalid,{0,1,2},*mappedFixture.program,mappedFixture.reader()));
            assert(mappedFixture.reads==7);++stateRejections;
        }
        assert(mapped->renderStates()[d-1].role==d-1 && mapped->renderStates()[d-1].colorMask==15);
    }
    std::printf("NATIVE_COLOR_CHAIN_CAPTURE_RENDER_STATE: %u pre-read rejections; two owned state requests PASS\n",stateRejections);
    unsigned depthRejections=0;
    assert(mapped->depthState().drawOffset==100 && mapped->depthState().samples==4 && mapped->depthGeometry().depthBits==0x3F400001);
    const auto depthFixturePlan=test::depthClearStatePlan();
    for(const auto& pair:depthFixturePlan.draws[0].registers)for(unsigned mutation=0;mutation<65;++mutation){
        auto invalid=mappedFixture.plan;
        if(!mutation)invalid.draws[0].registers.erase(pair.first);
        else if(mutation<=32)invalid.draws[0].registers[pair.first].bits.known^=1u<<(mutation-1);
        else invalid.draws[0].registers[pair.first].bits.value^=1u<<(mutation-33);
        assert(!NativeColorChainCapture::create(invalid,{0,1,2},*mappedFixture.program,mappedFixture.reader()));
        assert(mappedFixture.reads==7);++depthRejections;
    }
    auto badPixel=mappedFixture.plan;badPixel.shaders.back().bigEndianCode.clear();
    assert(!NativeColorChainCapture::create(badPixel,{0,1,2},*mappedFixture.program,mappedFixture.reader()) && mappedFixture.reads==7);
    ++depthRejections;
    std::printf("NATIVE_COLOR_CHAIN_CAPTURE_DEPTH_STATE: %u pre-read rejections; known depth request and passthrough pixel PASS\n",depthRejections);
    for(unsigned role=1;role<3;++role)for(unsigned vertex=0;vertex<4;++vertex){
        const auto prior=mappedFixture.reads;mappedFixture.raw[role][vertex*36]^=1;
        assert(!NativeColorChainCapture::create(mappedFixture.plan,{0,1,2},*mappedFixture.program,mappedFixture.reader()));
        assert(mappedFixture.reads==prior+7);mappedFixture.raw[role][vertex*36]^=1;
    }
    assert(mapped->pack(packed));
    std::puts("NATIVE_COLOR_CHAIN_CAPTURE_GEOMETRY: 8 whole-primitive post-read rejections; prior owned capture unchanged PASS");
    for(unsigned vertex=0;vertex<3;++vertex){
        const auto prior=mappedFixture.reads;mappedFixture.raw[0][vertex*28+8]^=1;
        assert(!NativeColorChainCapture::create(mappedFixture.plan,{0,1,2},*mappedFixture.program,mappedFixture.reader()));
        assert(mappedFixture.reads==prior+7);mappedFixture.raw[0][vertex*28+8]^=1;
    }
    assert(mapped->pack(packed));
    std::puts("NATIVE_COLOR_CHAIN_CAPTURE_DEPTH_GEOMETRY: 3 whole-depth post-read rejections; prior capture unchanged PASS");
}
