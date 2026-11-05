#include "gta5_shader_resource.h"
#include <cassert>
#include <map>

int main() {
    std::map<std::uint32_t,std::uint32_t> memory{
        {0x1040,16},{0x1018,0xA0001000},{0x1038,128},{0x103C,24},
        {0x2380,0},{0x2020,0xA0100000},{0x2368,64},{0x236C,96}};
    const auto read=[&](std::uint32_t at)->std::optional<std::uint32_t> {
        const auto it=memory.find(at);
        return it==memory.end()?std::nullopt:std::optional<std::uint32_t>(it->second);
    };
    auto pixel=recomp::gpu::describeGta5ShaderCode(0x1000,1,0,read);
    assert(pixel && pixel->virtualAddress==0xA0001080 && pixel->byteCount==24 && !pixel->requiresVertexPatching);
    auto vertex=recomp::gpu::describeGta5ShaderCode(0x2000,0,0,read);
    assert(vertex && vertex->virtualAddress==0xA0100040 && vertex->byteCount==96 && vertex->requiresVertexPatching);
    assert(!recomp::gpu::describeGta5ShaderCode(0x2000,0,1,read)); // Missing variant, not silently variant zero.
    memory[0x103C]=0;assert(!recomp::gpu::describeGta5ShaderCode(0x1000,1,0,read));
    memory[0x103C]=24;memory[0x1018]=0xFFFFFFF0;
    assert(!recomp::gpu::describeGta5ShaderCode(0x1000,1,0,read)); // No virtual address wrap.
    assert(!recomp::gpu::describeGta5ShaderCode(0xFFFFFFF0,0,0,read));
}
