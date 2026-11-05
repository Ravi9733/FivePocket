#include "native_gamepad_snapshot.h"
#include <cassert>
int main() {
    recomp::input::GamepadSnapshot input;
    recomp::input::GamepadState got;got.packet=99;
    assert(input.read(0,got)==0 && got.packet==99);
    recomp::input::GamepadState state;state.buttons=0x1021;state.leftTrigger=128;state.rightTrigger=255;
    state.leftX=-32768;state.leftY=32767;state.rightX=-1;state.rightY=1;
    input.publish(true,state);assert(input.read(0,got)==1 && got.packet==1);
    const auto bytes=recomp::input::encodeGamepadState(got);
    const std::array<std::uint8_t,16> expected{0,0,0,1,0x10,0x21,128,255,128,0,127,255,255,255,0,1};
    assert(bytes==expected);
    input.publish(true,state);assert(input.read(0,got)==1 && got.packet==1);
    state.buttons=0;input.publish(true,state);assert(input.read(0,got)==1 && got.packet==2 && got.buttons==0);
    assert(input.read(1,got)==0 && input.read(255,got)==0);
    input.capabilities(state);assert(input.readCapabilities(0,got)==1 && got.leftX==-32768);
    assert(input.readCapabilities(1,got)==0);
    input.publish(false,state);assert(input.read(0,got)==0);
    assert(input.readCapabilities(0,got)==0);
    input.publish(true,{});assert(input.read(0,got)==1 && got.packet==4 && got.buttons==0 && got.leftX==0);
}
