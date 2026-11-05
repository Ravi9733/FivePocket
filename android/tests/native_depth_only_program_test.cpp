// ABI21 depth-only program contract. The defining property is an ABSENT pixel
// stage: the guest bound no pixel shader and masked every colour channel. These
// cases pin that the contract REQUIRES that absence rather than tolerating it,
// that it carries no interpolator, texture or colour output, and that no other
// band accepts it and it accepts no other band.
#include "native_translated_draw.h"
#include <cassert>
#include <cstdio>
using namespace recomp::gpu;

namespace {
NativeTranslatedDrawProgram depthOnly() {
    NativeTranslatedDrawProgram p;
    p.abiVersion=21;p.depthOnlyRectangle=true;p.indexed=false;p.stride=24;p.interpolators=0;
    p.textureSlot=UINT32_MAX;
    p.vertexIdentity={1,2,3,4};
    return p;
}
unsigned passed=0;
void check(bool condition,const char* what) {
    if(!condition){std::fprintf(stderr,"DEPTH_ONLY_CONTRACT_FAIL %s\n",what);std::abort();}
    ++passed;
}
}

int main() {
    const auto base=depthOnly();
    check(validateNativeDepthOnlyRectangleProgram(base),"canonical depth-only program admitted");

    // The absent pixel stage is required, not merely permitted.
    {auto p=base;p.pixelCode={0x07230203,9};check(!validateNativeDepthOnlyRectangleProgram(p),"pixel code rejected");}
    {auto p=base;p.pixelIdentity={7};check(!validateNativeDepthOnlyRectangleProgram(p),"pixel identity rejected");}

    // A depth-only pass carries no interpolator, texture or colour output.
    {auto p=base;p.interpolators=1;check(!validateNativeDepthOnlyRectangleProgram(p),"interpolator rejected");}
    {auto p=base;p.textureSlot=0;check(!validateNativeDepthOnlyRectangleProgram(p),"legacy texture slot rejected");}
    {auto p=base;p.textureSlots={0};check(!validateNativeDepthOnlyRectangleProgram(p),"texture binding rejected");}

    // Geometry, and the shape of the draw itself, are pinned.
    {auto p=base;p.indexed=true;check(!validateNativeDepthOnlyRectangleProgram(p),"indexed rejected");}
    {auto p=base;p.stride=0;check(!validateNativeDepthOnlyRectangleProgram(p),"zero stride rejected");}
    {auto p=base;p.stride=25;check(!validateNativeDepthOnlyRectangleProgram(p),"unaligned stride rejected");}
    {auto p=base;p.vertexCode={0x07230203,1};check(!validateNativeDepthOnlyRectangleProgram(p),"compiled vertex module rejected");}
    {auto p=base;p.vertexIdentity.clear();check(!validateNativeDepthOnlyRectangleProgram(p),"absent vertex identity rejected");}

    // The version and the marker must agree; neither alone admits the program.
    {auto p=base;p.abiVersion=20;check(!validateNativeDepthOnlyRectangleProgram(p),"wrong version rejected");}
    {auto p=base;p.depthOnlyRectangle=false;check(!validateNativeDepthOnlyRectangleProgram(p),"missing marker rejected");}

    // No neighbouring band claims it, and it claims none of theirs.
    {auto p=base;p.compositeRectangle=true;check(!validateNativeDepthOnlyRectangleProgram(p),"composite marker rejected");}
    {auto p=base;p.fixed16RectangleDepth=true;check(!validateNativeDepthOnlyRectangleProgram(p),"fixed16 rectangle marker rejected");}
    {auto p=base;p.geometryFixed16GradientDepth=true;check(!validateNativeDepthOnlyRectangleProgram(p),"gradient depth marker rejected");}
    {auto p=base;p.geometryDepth20e4=true;check(!validateNativeDepthOnlyRectangleProgram(p),"depth20e4 rejected");}
    {auto p=base;p.geometryPixelEffects=true;check(!validateNativeDepthOnlyRectangleProgram(p),"pixel effects rejected");}
    {auto p=base;p.geometryColor8Format=0;check(!validateNativeDepthOnlyRectangleProgram(p),"color8 format rejected");}
    {auto p=base;p.geometryMrtOutputs=1;check(!validateNativeDepthOnlyRectangleProgram(p),"MRT outputs rejected");}
    {auto p=base;p.geometryDepthColorWitness=true;check(!validateNativeDepthOnlyRectangleProgram(p),"depth/colour witness rejected");}
    {auto p=base;p.directTriangles=true;check(!validateNativeDepthOnlyRectangleProgram(p),"direct triangles rejected");}
    {auto p=base;p.cubeRectanglePwl=true;check(!validateNativeDepthOnlyRectangleProgram(p),"cube PWL rejected");}
    {auto p=base;p.explicitLodRectangle=true;check(!validateNativeDepthOnlyRectangleProgram(p),"explicit LOD rejected");}
    {auto p=base;p.color8SourceClamped=true;check(!validateNativeDepthOnlyRectangleProgram(p),"source clamp rejected");}
    {auto p=base;p.fixed16VertexRegisterCount=6;check(!validateNativeDepthOnlyRectangleProgram(p),"register count rejected");}
    {auto p=base;p.geometryParameterRegister=0;check(!validateNativeDepthOnlyRectangleProgram(p),"parameter register rejected");}
    {auto p=base;p.geometryCentroidMask=1;check(!validateNativeDepthOnlyRectangleProgram(p),"centroid mask rejected");}

    // The fixed16 bands must not admit an ABI21 program.
    check(!validateNativeFixed16Program(base),"fixed16 geometry band refuses depth-only");
    check(!validateNativeFixed16RectangleProgram(base),"fixed16 rectangle band refuses depth-only");

    // The legacy ABI4 draw validator refuses it on the program alone.
    {Gta5InlineDraw draw;check(!validateNativeTranslatedDraw(draw,base),"legacy ABI4 validator refuses depth-only");}

    std::printf("DEPTH_ONLY_PROGRAM_CONTRACT_PASS abi21_explicit=1 absent_pixel_stage_required=1 "
        "neighbouring_bands_excluded=%u gpu=0 gameplay=0\n",passed);
    return 0;
}
