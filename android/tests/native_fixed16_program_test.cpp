#include "native_fixed16_draw_state.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;
int main() {
    NativeTranslatedDrawProgram plain;
    plain.abiVersion=17;plain.indexed=true;plain.geometryDepth20e4=true;
    plain.geometryFixed16GradientDepth=true;plain.textureSlot=UINT32_MAX;
    plain.stride=28;plain.interpolators=1;
    assert(validateNativeFixed16Program(plain));
    auto addressed=plain;addressed.fixed16VertexRegisterCount=6;assert(validateNativeFixed16Program(addressed));
    for(unsigned count:{1u,2u,3u,5u,7u,64u}) {
        auto invalid=addressed;invalid.fixed16VertexRegisterCount=count;assert(!validateNativeFixed16Program(invalid));
    }
    auto effects=plain;effects.abiVersion=18;effects.geometryPixelEffects=true;
    effects.interpolators=2;effects.textureSlots={0};effects.textureSlot=0;
    assert(validateNativeFixed16Program(effects));
    auto addressedEffects=effects;addressedEffects.fixed16VertexRegisterCount=7;
    assert(validateNativeFixed16Program(addressedEffects));
    for(unsigned count:{1u,2u,3u,5u,6u,8u,64u}) {
        auto invalid=addressedEffects;invalid.fixed16VertexRegisterCount=count;
        assert(!validateNativeFixed16Program(invalid));
    }
    auto wrongCount=effects;wrongCount.fixed16VertexRegisterCount=6;assert(!validateNativeFixed16Program(wrongCount));
    auto wrong=effects;wrong.abiVersion=17;assert(!validateNativeFixed16Program(wrong));
    wrong=effects;wrong.geometryPixelEffects=false;assert(!validateNativeFixed16Program(wrong));
    wrong=plain;wrong.textureSlot=0;assert(!validateNativeFixed16Program(wrong));
    wrong=plain;wrong.geometryPixelEffects=true;assert(!validateNativeFixed16Program(wrong));
    for(auto slots:{std::vector<unsigned>{0,0},std::vector<unsigned>{32},
                    std::vector<unsigned>{0,1,2,3,4}}) {
        wrong=effects;wrong.textureSlots=slots;assert(!validateNativeFixed16Program(wrong));
    }
    wrong=effects;wrong.textureSlots={31,4,9,0};assert(validateNativeFixed16Program(wrong));
    for(unsigned abi:{4u,13u,14u,15u,16u,19u}) {
        wrong=effects;wrong.abiVersion=abi;assert(!validateNativeFixed16Program(wrong));
    }
    for(unsigned field=0;field<10;++field) {
        wrong=effects;
        switch(field) {
        case 0:wrong.indexed=false;break;
        case 1:wrong.geometryDepth20e4=false;break;
        case 2:wrong.geometryFixed16GradientDepth=false;break;
        case 3:wrong.geometrySampleDepth=true;break;
        case 4:wrong.geometryColor8Format=0;break;
        case 5:wrong.geometryCentroidMask=1;break;
        case 6:wrong.geometryMrtOutputs=1;break;
        case 7:wrong.geometryMrtFormats[0]=0;break;
        case 8:wrong.geometryDepthColorWitness=true;break;
        case 9:wrong.geometryParameterRegister=0;break;
        }
        assert(!validateNativeFixed16Program(wrong));
    }
    wrong=effects;wrong.geometryMrtBlendControls[0]=0x7010001;assert(!validateNativeFixed16Program(wrong));
    wrong=effects;wrong.fixed16RectangleDepth=true;assert(!validateNativeFixed16Program(wrong));
    NativeTranslatedDrawProgram rectangle;
    rectangle.abiVersion=19;rectangle.fixed16RectangleDepth=true;rectangle.geometryDepth20e4=true;
    rectangle.textureSlot=2;rectangle.stride=24;rectangle.interpolators=1;
    assert(validateNativeFixed16RectangleProgram(rectangle));
    wrongCount=rectangle;wrongCount.fixed16VertexRegisterCount=6;assert(!validateNativeFixed16RectangleProgram(wrongCount));
    assert(!validateNativeFixed16Program(rectangle));
    assert(!validateNativeFixed16RectangleProgram(plain));
    assert(!validateNativeFixed16RectangleProgram(effects));
    for(unsigned field=0;field<16;++field) {
        wrong=rectangle;
        switch(field) {
        case 0:wrong.abiVersion=18;break;
        case 1:wrong.indexed=true;break;
        case 2:wrong.fixed16RectangleDepth=false;break;
        case 3:wrong.geometryFixed16GradientDepth=true;break;
        case 4:wrong.geometryDepth20e4=false;break;
        case 5:wrong.geometrySampleDepth=true;break;
        case 6:wrong.geometryPixelEffects=true;break;
        case 7:wrong.geometryCentroidMask=1;break;
        case 8:wrong.textureSlots={2,3};break;
        case 9:wrong.textureSlot=32;break;
        case 10:wrong.geometryMrtOutputs=1;break;
        case 11:wrong.cubeRectanglePwl=true;break;
        case 12:wrong.explicitLodRectangle=true;break;
        case 13:wrong.directTriangles=true;break;
        case 14:wrong.stride=0;break;
        case 15:wrong.interpolators=2;break;
        }
        assert(!validateNativeFixed16RectangleProgram(wrong));
    }
    std::puts("FIXED16_RECTANGLE_PROGRAM_CONTRACT_PASS abi19_explicit=1 indexed_profiles_preserved=1 gpu=0 ownership=0");
    std::puts("FIXED16_PROGRAM_CONTRACT_PASS abi17_preserved=1 abi18_explicit=1 invalid_combinations_rejected=1 gpu=0 ownership=0");
    std::puts("FIXED16_ADDRESSED_EFFECTS_CONTRACT_PASS abi18_seven_GPR_explicit=1 legacy_defaults_preserved=1 wrong_counts_rejected=1 gpu=0 ownership=0");
}
