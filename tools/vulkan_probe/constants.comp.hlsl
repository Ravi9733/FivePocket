// Synthetic ABI fixture. No game shader code or data.
#include "shader_common.h"
RWStructuredBuffer<uint> TestOutput : register(u1, space0);

[numthreads(1, 1, 1)]
void shaderMain() {
    for (uint i = 0; i < 256; ++i) {
        uint4 value = asuint(XENOS_LOAD_FLOAT4(Vertex, i * 16));
        for (uint j = 0; j < 4; ++j) TestOutput[i * 4 + j] = value[j];
    }
    for (uint i = 0; i < 224; ++i) {
        uint4 value = asuint(XENOS_LOAD_FLOAT4(Pixel, i * 16));
        for (uint j = 0; j < 4; ++j) TestOutput[1024 + i * 4 + j] = value[j];
    }
    for (uint i = 0; i < 80; ++i) TestOutput[1920 + i] = XENOS_LOAD_SHARED_UINT(i * 4);
    TestOutput[2000] = g_Booleans;
    TestOutput[2001] = g_ClipPlaneEnabled ? 1 : 0;
    TestOutput[2002] = asuint(g_AlphaThreshold);
    TestOutput[2003] = g_SwappedTexcoords;
    TestOutput[2004] = g_SwappedNormals;
    TestOutput[2005] = g_SwappedBinormals;
    TestOutput[2006] = g_SwappedTangents;
    TestOutput[2007] = g_SwappedBlendWeights;
    uint2 halfPixel = asuint(g_HalfPixelOffset);
    TestOutput[2008] = halfPixel.x;
    TestOutput[2009] = halfPixel.y;
    uint4 plane = asuint(g_ClipPlane);
    for (uint i = 0; i < 4; ++i) TestOutput[2010 + i] = plane[i];
    TestOutput[2014] = g_conditionalSurveyIndex;
    TestOutput[2015] = g_conditionalRenderingIndex;
    for (uint pattern = 0; pattern < 3; ++pattern) {
        uint mask = pattern == 0 ? 0 : (pattern == 1 ? 0xffffffffu : 0x80000001u);
        for (uint index = 0; index < 64; ++index) {
            uint4 swapped = asuint(swapFloats(mask, float4(1, 2, 3, 4), index));
            for (uint j = 0; j < 4; ++j)
                TestOutput[2016 + (pattern * 64 + index) * 4 + j] = swapped[j];
        }
    }
}
