#pragma once
#include "native_color_vertex_geometry.h"
#include "xenos_copy_shader_fixture.h"
namespace recomp::gpu::test {
inline Bytes colorVertexShader(bool r8) {
    const Words position{{(1u<<12)|(1u<<19)|(31u<<20)|(2u<<25),0x688u|0x3000u|(38u<<16),9}};
    const Words uv{{(2u<<12)|(1u<<19),(r8?0x208u:0xB48u)|0x3000u|(37u<<16)|(1u<<30),4u<<8}};
    const Words tint{{(3u<<12)|(1u<<19),0x688u|(6u<<16)|(1u<<30),6u<<8}};
    return program({alloc(1),alloc(2),exec(2,6)|(0x15u<<16)},
        {position,uv,tint,alu(62,15,0,true,false,1),alu(0,15,0,true,false,2),alu(1,15,0,true,false,3)});
}
inline Bytes colorVertexSource() {
    Bytes bytes(144);
    const auto put=[&](unsigned offset,std::uint32_t word){for(unsigned b=0;b<4;++b)bytes[offset+b]=word>>(24-b*8);};
    for(unsigned i=0;i<4;++i){
        put(i*36,i/2?0x3F800000:0xBF800000);put(i*36+4,i%2?0x3F800000:0xBF800000);
        put(i*36+8,0);put(i*36+12,0x3F800000);
        put(i*36+16,i/2?0x3F800000:0);put(i*36+20,i%2?0:0x3F800000);put(i*36+24,0xFF000000);
    }
    return bytes;
}
struct ColorVertexGeometryFixture {
    Bytes vertices=colorVertexSource();NativeColorShaderIdentity shader;NativeColorPairBindings pair;
    explicit ColorVertexGeometryFixture(unsigned role){
        shader={0,colorVertexShader(!role)};
        pair.vertices.slot=95;pair.vertices.descriptor={NativeInputWord{0x1003,0xFFFFFFFF},NativeInputWord{0x10000092,0xFFFFFFFF}};
        pair.vertices.source={0x1000,vertices.data(),vertices.size()};
        for(unsigned c:{8u,9u,10u,11u,36u}){
            if(role && c==36)continue;NativeColorConstant constant;constant.index=c;
            for(auto& w:constant.words)w={0,0xFFFFFFFF};pair.vertexConstants.push_back(constant);
        }
    }
    ColorVertexGeometryFixture(const ColorVertexGeometryFixture&)=delete;
    ColorVertexGeometryFixture& operator=(const ColorVertexGeometryFixture&)=delete;
};
}
