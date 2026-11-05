#include "gta5_shader_patch_capture.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <stdexcept>
using namespace recomp::gpu;

int main() {
    std::map<std::uint32_t,std::vector<std::uint8_t>> memory;
    memory[0x1000].resize(4096); memory[0x4000].resize(240);
    memory[0x5000].resize(240); memory[0x6000].resize(76); memory[0x7000].resize(96);
    const auto put=[&](unsigned base,unsigned offset,unsigned value) {
        auto& b=memory.at(base);
        for(unsigned i=0;i<4;++i)b.at(offset+i)=value>>(24-i*8);
    };
    put(0x1000,32,0x4000);put(0x1000,896,64); // relative variant header
    put(0x1000,872+64,0);put(0x1000,876+64,240);
    put(0x1000,872+64+24,2);put(0x1000,872+64+28,3);
    put(0x6000,24,2);
    for(unsigned i=0;i<240;++i)memory[0x4000][i]=static_cast<std::uint8_t>(i*13+1);
    memory[0x5000]=memory[0x4000];
    unsigned reads=0;
    ShaderPatchReader read=[&](unsigned a,unsigned n)->std::optional<std::vector<std::uint8_t>> {
        ++reads;
        for(const auto& [base,b]:memory) if(a>=base && std::uint64_t{a}+n<=std::uint64_t{base}+b.size())
            return std::vector<std::uint8_t>(b.begin()+(a-base),b.begin()+(a-base)+n);
        return std::nullopt;
    };
    const std::array<unsigned,5> args{0x1000,0x5000,0x6000,0x7000,0};
    auto c=captureGta5ShaderPatchBefore(args,read,read);assert(c);
    assert(c->source==c->before && !c->returned && !c->afterAvailable);
    assert(c->sourceHeader.size()==36 && c->patchTable.size()==12);
    assert(c->fetchTemplates.size()==416 && c->declaration.size()==76 && c->lookup.size()==96);
    const auto original=c->source;
    memory[0x5000][37]^=0x80;
    assert(captureGta5ShaderPatchAfter(*c,read));
    assert(c->returned && c->afterAvailable && c->source==original && c->before==original);
    assert(c->after[37]!=(c->before[37]) && c->after==memory[0x5000]);
    const auto after=c->after;assert(!captureGta5ShaderPatchAfter(*c,read));assert(c->after==after);
    // Owned snapshots survive source/destination changes and expose a bad copy.
    memory[0x4000][0]^=1;
    auto badCopy=captureGta5ShaderPatchBefore(args,read,read);assert(badCopy);
    assert(badCopy->source!=badCopy->before && c->source==original);
    ShaderPatchReader shortRead=[](unsigned,unsigned n){return std::vector<std::uint8_t>(n-1);};
    ShaderPatchReader throws=[](unsigned,unsigned)->std::optional<std::vector<std::uint8_t>> {throw std::runtime_error("fixture");};
    assert(!captureGta5ShaderPatchBefore(args,read,shortRead));
    assert(!captureGta5ShaderPatchBefore(args,shortRead,read));
    assert(!captureGta5ShaderPatchBefore(args,read,throws));
    assert(!captureGta5ShaderPatchBefore(args,{},read));
    assert(!captureGta5ShaderPatchAfter(*badCopy,shortRead));
    assert(badCopy->returned && !badCopy->afterAvailable && badCopy->after.empty());
    assert(!captureGta5ShaderPatchAfter(*badCopy,throws));
    for(const auto destination:{0u,0x5001u,0xFFFFFFF0u}) {
        auto a=args;a[1]=destination;assert(!captureGta5ShaderPatchBefore(a,read,read));
    }
    auto invalid=args;invalid[4]=32;assert(!captureGta5ShaderPatchBefore(invalid,read,read));
    put(0x1000,876+64,65540);assert(!captureGta5ShaderPatchBefore(args,read,read));
    put(0x1000,876+64,240);
    put(0x1000,872+64+28,0xFFFFFFFF);put(0x6000,24,0xFFFFFFFF);
    auto bounded=captureGta5ShaderPatchBefore(args,read,read);assert(bounded);
    assert(bounded->patchTable.empty() && bounded->declaration.empty());
    assert(reads<300);
    std::puts("GTA5_SHADER_PATCH_CAPTURE_PASS source_copy_patch_distinguished=1 guest_writes=0");
}
