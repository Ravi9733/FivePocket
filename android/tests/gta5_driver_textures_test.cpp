#include "gta5_driver_textures.h"
#include <cassert>
#include <algorithm>

int main() {
    using namespace recomp::gpu;
    constexpr std::uint32_t device=0x10000000,vertex=0xA7000000;
    constexpr unsigned count=3544,stride=20;
    std::vector<std::uint8_t> shadow(13860);
    const auto put=[&](unsigned at,std::uint32_t v) {
        for(unsigned i=0;i<4;++i)shadow[at+i]=static_cast<std::uint8_t>(v>>(24-8*i));
    };
    const Gta5ResourceReader heap=[&](std::uint32_t at,std::uint32_t n)
        ->std::optional<std::vector<std::uint8_t>> {
        if(at<device || std::uint64_t{at-device}+n>shadow.size())return std::nullopt;
        return std::vector<std::uint8_t>(shadow.begin()+at-device,shadow.begin()+at-device+n);
    };
    const Gta5ResourceReader data=[&](std::uint32_t at,std::uint32_t n)
        ->std::optional<std::vector<std::uint8_t>> {
        if(at!=vertex || n!=count*stride)return std::nullopt;
        return std::vector<std::uint8_t>(n,0x3F);
    };
    constexpr std::array<std::uint32_t,6> literal{0x02024802,2,0,0x00281249,3,0x210};
    for(unsigned i=0;i<6;++i)put(1152+2*24+4*i,literal[i]);
    put(13848,vertex);put(13856,count*stride/4);put(13072,0x10004000);put(13068,0x10005000);
    Gta5DriverTextures bindings;
    constexpr std::uint64_t slot2=std::uint64_t{1}<<61;
    assert(!bindings.fetch(2));
    assert(bindings.upload(device,slot2,heap));
    assert(bindings.fetch(2)==literal && !bindings.fetch(0));
    const Gta5TextureFetchReader fetch=[&](unsigned slot){return bindings.fetch(slot);};
    put(1152+2*24,literal[0]&~3u);
    assert(!captureGta5InlineDraw(device,13,count,stride,{2},heap,data));
    const auto draw=captureGta5InlineDraw(device,13,count,stride,{2},heap,data,fetch);
    assert(draw && draw->vertices.size()==70880 && draw->textures[0].literalZero);
    assert(draw->textures[0].fetch==literal); // CPU edits are not submissions.
    assert(!bindings.upload(device,slot2|1,heap));
    assert(bindings.fetch(2)==literal);
    assert(!bindings.upload(device,slot2,[](auto,auto){return std::optional<std::vector<std::uint8_t>>{};}));
    assert(bindings.fetch(2)==literal); // Failed uploads never partially commit.
    assert(bindings.upload(device,slot2,heap));
    assert(((*bindings.fetch(2))[0]&3)==0); // Real unbinds are retained, not repaired.
    assert(!captureGta5InlineDraw(device,13,count,stride,{2},heap,data,fetch));
    put(1152+31*24,2);
    assert(bindings.upload(device,std::uint64_t{1}<<32,heap));
    assert(bindings.fetch(31) && (*bindings.fetch(31))[0]==2 && !bindings.fetch(32));
    assert(!bindings.upload(0xFFFFFB80,slot2,heap));

    // Diagnostic capture is independent of renderer layout acceptance. Even an
    // invalid/unbound submitted descriptor is evidence, not a missing slot.
    std::array<unsigned,32> calls{};
    const auto snapshot=captureGta5SubmittedTextures([&](unsigned slot) {
        assert(slot<32);++calls[slot];return bindings.fetch(slot);
    });
    for(auto n:calls)assert(n==1);
    assert(snapshot[2] && ((*snapshot[2])[0]&3)==0);
    assert(snapshot[31] && (*snapshot[31])[0]==2);
    assert(!snapshot[7]);
    for(const auto& absent:captureGta5SubmittedTextures({}))assert(!absent);
    for(unsigned i=0;i<6;++i)put(1152+2*24+4*i,literal[i]);
    assert(bindings.upload(device,slot2,heap));
    assert(bindings.fetch(2)==literal && ((*snapshot[2])[0]&3)==0);
    // A mip/unsupported descriptor still survives byte-exactly, without a
    // source read or any promise that the pixels are available/implementable.
    auto mip=literal;mip[4]|=2u<<6;mip[5]|=0x800;
    for(unsigned i=0;i<6;++i)put(1152+7*24+4*i,mip[i]);
    assert(bindings.upload(device,std::uint64_t{1}<<56,heap));
    assert(captureGta5SubmittedTextures(fetch)[7]==mip);
    assert(!captureGta5InlineDraw(device,13,count,stride,{7},heap,data,fetch));
}
