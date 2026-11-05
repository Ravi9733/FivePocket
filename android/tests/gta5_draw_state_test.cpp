#include "gta5_draw_state.h"
#include <cassert>

int main() {
    using namespace recomp::gpu;
    constexpr std::uint32_t device=0x1004CD00;
    assert(gta5DrawStateUploadBytes(device,0xA000000000000000ULL,0x2200,device+10548)==12);
    assert(gta5DrawStateUploadBytes(device,0xFFF0000000000000ULL,0x2200,device+10548)==48);
    // BeginVertices uploads eight boolean and 32 integer constant DWORDs.
    assert(gta5DrawStateUploadBytes(device,0xFFFFFFFFFF000000ULL,0x4900,device+10112)==160);
    assert(!gta5DrawStateUploadBytes(device,0,0x2200,device+10548));
    assert(!gta5DrawStateUploadBytes(device,0xFFF8000000000000ULL,0x2200,device+10548));
    assert(!gta5DrawStateUploadBytes(device,0x8000000000000000ULL,0x2080,device+10368));
    assert(!gta5DrawStateUploadBytes(device,0x8000000000000000ULL,0x2200,device+10552));
    assert(!gta5DrawStateUploadBytes(0xFFFFF000,0x8000000000000000ULL,0x2200,0x1934));
    Gta5InlineDraw draw;draw.deviceState.resize(13860);
    draw.deviceState[10548]=0x12;draw.deviceState[10549]=0x34;
    draw.deviceState[10550]=0x56;draw.deviceState[10551]=0x78;
    assert(gta5DrawRegister(draw,0x2200)==0x12345678);
    assert(!gta5DrawRegister(draw,0x2080));
    draw.scissorRegisters=std::array<std::uint32_t,3>{1,2,3};
    assert(gta5DrawRegister(draw,0x2081)==2);
}
