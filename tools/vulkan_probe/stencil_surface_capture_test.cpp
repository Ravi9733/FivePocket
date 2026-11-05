// Read-only exact stopped-state connection check. No authored vertices,
// shader execution, depth initialization, or live admission is implied.
#include "gta5_draw_state.h"
#include "gta5_logical_tile_clear.h"
#include "native_stencil_only_plan.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
using namespace recomp::gpu;
int main(int argc,char** argv) {
    assert(argc==2);const std::string root=argv[1];
    const auto read=[&](const char* name,unsigned size) {
        std::ifstream stream(root+"/"+name,std::ios::binary|std::ios::ate);
        assert(stream && stream.tellg()==size);stream.seekg(0);
        std::vector<std::uint8_t> data(size);assert(stream.read(reinterpret_cast<char*>(data.data()),size));return data;
    };
    Gta5InlineDraw draw;draw.deviceState=read("device-state.bin",13860);
    const auto word=[](const auto& b,unsigned at) {
        return (unsigned(b[at])<<24)|(unsigned(b[at+1])<<16)|(unsigned(b[at+2])<<8)|b[at+3];
    };
    const auto depth=read("surface-4-resource.bin",40);
    assert(gta5DrawRegister(draw,0x2002)==0x00010000u);
    assert(gta5DrawRegister(draw,0x2004)==0x00000600u);
    assert(word(depth,28)==*gta5DrawRegister(draw,0x2002));
    assert(word(depth,24)==*gta5DrawRegister(draw,0x2000));
    assert(gta5DrawRegister(draw,0x210D)==0x00FFFF20u);
    assert(gta5DrawRegister(draw,0x210C)==0x00FFFF00u);
    assert(gta5DrawRegister(draw,0x2201)==0x00010001u); // Blend, not stencil.
    const auto stencil=decodeXenosDepthStencilState(*gta5DrawRegister(draw,0x2200),
        *gta5DrawRegister(draw,0x210D),*gta5DrawRegister(draw,0x210C));
    assert(stencil && stencil->front.reference==32 && stencil->front.readMask==255 && stencil->front.writeMask==255);
    assert(gta5DrawRegister(draw,0x2205)==0x18000u); // No face culling.
    assert(gta5DrawRegister(draw,0x2113)==0xBF800000u && gta5DrawRegister(draw,0x2114)==0x3F800000u);
    assert((draw.deviceState[11068]&0x30)==0 && word(draw.deviceState,13100)==0);
    assert(!describeGta5DriverTiling(draw.deviceState));
    for(unsigned slot:{0u,4u}) {
        assert(word(draw.deviceState,12816+4*slot)!=word(draw.deviceState,13104+4*slot));
    }
    Gta5ClearCapture capture;capture.state=draw.deviceState;
    for(unsigned i=0;i<5;++i) {
        capture.surfaces[i].object=word(draw.deviceState,12816+4*i);
        capture.surfaces[i].tileCache=word(draw.deviceState,13104+4*i);
    }
    capture.surfaces[0].header=read("surface-0-resource.bin",40);capture.surfaces[4].header=depth;
    assert(!describeGta5LogicalTileSurfaces(capture));
    const auto tiledDepth=read("scissor-surface-4.bin",40);
    const auto fixedDepth=read("fixed16-4-depth-header.bin",40);
    // Same physical base is insufficient: tiled depth is 384 rows, the later
    // direct surface 720, and a completed later fixed16 pass reuses base zero
    // with pitch 320 rather than 1280. These are not interchangeable images.
    assert(word(tiledDepth,28)==word(depth,28));
    assert(((word(tiledDepth,36)>>3)&0x7FFF)+1==384);
    assert(((word(depth,36)>>3)&0x7FFF)+1==720);
    assert(word(fixedDepth,28)==word(depth,28) && (word(fixedDepth,24)&0x3FFF)==320);
    std::puts("STENCIL_CAPTURE_RASTER_PASS stencil_reference=32 read_mask=255 write_mask=255 cull=0 depth_range=1_to_0 original_vertices_available=0");
    std::puts("STENCIL_CAPTURE_BOUNDARY_PASS depth_register=2002 depth_info=00010000 color2_not_depth=1 current_non_tiled=1 retained_binding_mismatch=1 tiled_description_refused=1 gpu=0 gameplay=0");
}
