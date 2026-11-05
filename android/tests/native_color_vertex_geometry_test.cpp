#include "native_color_vertex_geometry_fixture.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main(){
    unsigned rejects=0;
    for(unsigned role=0;role<2;++role){
        test::ColorVertexGeometryFixture f(role);NativeColorVertexGeometry result;
        assert(planNativeColorVertexGeometry(f.shader,f.pair,role,result));
        assert((result.uniformTintBits==std::array<std::uint32_t,4>{0,0,0,0x3F800000}));
        for(unsigned i=0;i<4;++i)assert(result.executedInstructions[i]==6 && result.fetches[i]==3);
        const NativeColorVertexGeometry sentinel{{{1,2,3,4}},{{5,6,7,8}},{{9,10,11,12}}};
        const auto reject=[&](const auto& shader,const auto& pair,unsigned index){
            result=sentinel;assert(!planNativeColorVertexGeometry(shader,pair,index,result));
            assert(result.uniformTintBits==sentinel.uniformTintBits && result.executedInstructions==sentinel.executedInstructions && result.fetches==sentinel.fetches);++rejects;
        };
        reject(f.shader,f.pair,2);auto shader=f.shader;shader.stage=1;reject(shader,f.pair,role);
        shader=f.shader;shader.bigEndianCode.clear();reject(shader,f.pair,role);
        for(unsigned mode=0;mode<9;++mode){auto pair=f.pair;
            if(mode==0)pair.vertices.slot=0;if(mode==1)pair.vertices.source.bytes=nullptr;
            if(mode==2)--pair.vertices.source.size;if(mode==3)pair.vertices.descriptor[0].knownMask=0;
            if(mode==4)pair.vertices.descriptor[1].knownMask=0;if(mode==5)++pair.vertices.source.physicalAddress;
            if(mode==6)pair.vertexConstants.pop_back();if(mode==7)pair.vertexConstants[0].index=12;
            if(mode==8)pair.vertexConstants[1].index=8;reject(f.shader,pair,role);
        }
        for(unsigned c=0;c<f.pair.vertexConstants.size();++c)for(unsigned lane=0;lane<4;++lane)for(unsigned bit=0;bit<32;++bit){
            auto pair=f.pair;pair.vertexConstants[c].words[lane].knownMask^=1u<<bit;reject(f.shader,pair,role);
        }
        // Every bit of all position/UV/tint source words participates; padding
        // alone is intentionally irrelevant to the declared vertex fetches.
        for(unsigned v=0;v<4;++v)for(unsigned byte=0;byte<28;++byte)for(unsigned bit=0;bit<8;++bit){
            f.vertices[v*36+byte]^=1u<<bit;reject(f.shader,f.pair,role);f.vertices[v*36+byte]^=1u<<bit;
        }
        for(unsigned v=0;v<4;++v)for(unsigned byte=28;byte<36;++byte)f.vertices[v*36+byte]=0xFF;
        assert(planNativeColorVertexGeometry(f.shader,f.pair,role,result));
    }
    std::printf("NATIVE_COLOR_VERTEX_GEOMETRY: %u rejection controls; whole W1 rectangles, UV corners, uniform tint, output preserved PASS\n",rejects);
}
