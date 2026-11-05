#include "gta5_vertex_shader_binding.h"
#include <cassert>
#include <stdexcept>
using Binding=recomp::gpu::Gta5VertexShaderBinding;
int main() {
    Binding binding,otherDevice;
    std::vector<std::uint8_t> bytes(24,0xA5);
    const auto owned=[&](auto address,auto count)->std::optional<std::vector<std::uint8_t>> {
        if(address!=0xA0010000 || count!=bytes.size())return std::nullopt;
        return bytes;
    };
    const auto bind=[&](auto object,auto variant,auto address,auto count) {
        return binding.select(object,variant,address,count,Binding::Source::CachedLoad,owned);
    };
    assert(bind(0x1000,0,0xA0010000,24));
    assert(!otherDevice.forObject(0x1000) && !binding.forObject(0x1004));
    bytes[0]=0xB6;assert(binding.forObject(0x1000)->code[0]==0xA5);
    assert(bind(0x1000,1,0xA0010000,24));
    assert(binding.forObject(0x1000)->variant==1 && binding.forObject(0x1000)->code[0]==0xB6);
    // Every failed admission invalidates the previous selection, including a
    // reader exception. There is no last-patched fallback.
    for(auto bad: {0u,3u,65540u,0xFFFFFFFFu}) {
        assert(!bind(0x1000,0,0xA0010000,bad));assert(!binding.forObject(0x1000));
    }
    assert(!bind(0,0,0xA0010000,24));assert(!bind(0x1001,0,0xA0010000,24));
    assert(!bind(0x1000,32,0xA0010000,24));assert(!bind(0x1000,0,0,24));
    assert(!bind(0x1000,0,0xA0010001,24));assert(!bind(0x1000,0,0xFFFFFFF0,24));
    assert(!bind(0x1000,0,0xA0011000,24));
    assert(!binding.select(0x1000,0,0x100,24,Binding::Source::CachedLoad,
        [](auto,auto){return std::optional(std::vector<std::uint8_t>(20));}));
    assert(bind(0x1000,0,0xA0010000,24));
    bool threw=false;
    try { binding.select(0x1000,0,0x100,24,Binding::Source::CommandCopy,
        [](auto,auto)->std::optional<std::vector<std::uint8_t>>{throw std::runtime_error("lost ownership");});
    } catch(const std::runtime_error&) {threw=true;}
    assert(threw && !binding.forObject(0x1000));
    assert(bind(0x1000,0,0xA0010000,24));binding.invalidate();assert(!binding.forObject(0x1000));
}
