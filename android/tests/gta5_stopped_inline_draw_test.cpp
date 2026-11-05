#include "gta5_stopped_inline_draw.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    constexpr std::uint32_t Device=0x10000000,Vertices=0xBF067000,VertexShader=0x10004000;
    constexpr unsigned Count=36,Stride=36;
    std::vector<std::uint8_t> state(13860,0),vertices(Count*Stride);
    const auto put=[&](unsigned at,std::uint32_t word) {
        for(unsigned i=0;i<4;++i)state[at+i]=static_cast<std::uint8_t>(word>>(24-8*i));
    };
    put(13072,VertexShader);put(13848,Vertices);put(13856,Count*Stride/4);
    for(unsigned i=0;i<vertices.size();++i)vertices[i]=static_cast<std::uint8_t>(i*13+7);
    for(unsigned i=0;i<1024;++i){put(1920+4*i,0x3F800000+i);put(6016+4*i,0x40000000+i);}
    const Gta5ResourceReader heap=[&](std::uint32_t a,std::uint32_t n)
            ->std::optional<std::vector<std::uint8_t>> {
        if(a==Device && n==state.size())return state;return std::nullopt;
    };
    bool readable=true;
    const Gta5ResourceReader data=[&](std::uint32_t a,std::uint32_t n)
            ->std::optional<std::vector<std::uint8_t>> {
        if(readable && a==Vertices && n==vertices.size())return vertices;return std::nullopt;
    };
    assert(!captureGta5InlineDraw(Device,4,Count,Stride,{},heap,data));
    const auto stopped=captureGta5StoppedInlineDraw(Device,4,Count,Stride,{},heap,data);
    assert(stopped && !stopped->pixelShader && stopped->vertexShader==VertexShader);
    assert(stopped->vertices==vertices && stopped->deviceState==state);
    assert(stopped->vertexConstants[0]==0x3F800000 && stopped->vertexConstants[1023]==0x3F8003FF);
    assert(stopped->pixelConstants[0]==0x40000000 && stopped->pixelConstants[1023]==0x400003FF);
    // A returned record owns its bytes, independently of later guest mutation.
    const auto original=vertices;vertices[0]^=0xFF;put(1920,0);
    assert(stopped->vertices==original && stopped->vertexConstants[0]==0x3F800000);
    put(13068,0x10005000);
    assert(captureGta5InlineDraw(Device,4,Count,Stride,{},heap,data));
    assert(captureGta5StoppedInlineDraw(Device,4,Count,Stride,{},heap,data)->pixelShader==0x10005000);
    put(13068,0);put(13072,0);
    assert(!captureGta5StoppedInlineDraw(Device,4,Count,Stride,{},heap,data));
    put(13072,VertexShader);readable=false;
    assert(!captureGta5StoppedInlineDraw(Device,4,Count,Stride,{},heap,data));
    readable=true;put(13856,Count*Stride/4-1);
    assert(!captureGta5StoppedInlineDraw(Device,4,Count,Stride,{},heap,data));
    put(13856,Count*Stride/4);
    assert(!captureGta5StoppedInlineDraw(Device+4,4,Count,Stride,{},heap,data));
    assert(!captureGta5StoppedInlineDraw(Device,4,0,Stride,{},heap,data));
    assert(!captureGta5StoppedInlineDraw(Device,4,Count,Stride,{},heap,{}));
    assert(!captureGta5StoppedInlineDraw(Device,4,Count,Stride,{}, {},data));
    std::puts("STOPPED_VERTEX_ONLY_CAPTURE_PASS original_bytes_owned=1 constants_owned=1 execution_default_unchanged=1 missing_inputs_rejected=1 gameplay=0");
}
