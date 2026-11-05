//============================================================================================================
//
//
//                  Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
// v460 (agent gsr1): Qualcomm Snapdragon Game Super Resolution 1 (SGSR 1, one pass: 12-tap Lanczos-like scale + adaptive
// sharpen of the green channel's difference) as the presenter's output scaler. This file is DERIVED from Qualcomm's
// sgsr/v1/include/glsl/sgsr1_shader_mobile.frag and, with DIRECT_GSR_EDGE_DIRECTION, from
// sgsr1_shader_mobile_edge_direction.frag with its UseEdgeDirection defined (BSD-3-Clause; the full licence text is in
// THIRD-PARTY-NOTICES.md of the gsr1 lane and must ship with the app's licence files: third_party/snapdragon-gsr/LICENSE and
// assets/licenses/Qualcomm-Snapdragon-GSR1-BSD-3-Clause.txt, added at the merge). The arithmetic is Qualcomm's,
// operation for operation and in the same order. What was changed, and only this:
//  - Vulkan interface: `source` (set 0, binding 0) and the push block `pc` are declared by the including shader; Qualcomm's
//    ViewportInfo[0] is pc.viewport = (1/1280, 1/720, 1280, 720); in_TEXCOORD0 (an interpolated 0..1 coordinate in
//    Qualcomm's sample) is computed from the output pixel: (q + 0.5) * pc.params.xy with pc.params.xy = 1 / the image rect.
//  - EdgeThreshold and EdgeSharpness are pc.params.z / pc.params.w instead of #defines (Qualcomm's defaults 8.0/255.0 and
//    2.0 unless the user sets the tuning files; see direct_present.h).
//  - OperationMode is fixed at 1 (RGBA: the green channel is the luma) as DIRECT_GSR_MODE; the mode 3 / 4 branches and the
//    unused xCenter / yCenter are left out.
//  - Syntax valid in both GLSL 4.50 and C++ (the host test runs this text through tests/glsl_shim.h): f-suffixed literals,
//    single-component swizzles (Qualcomm's `upDown.xy = gather.wz` is written per component), `std` renamed gsrStd,
//    `highp vec2(...)` constructors written `vec2(...)` (GLSL 4.50 has no qualified constructors; their operands, pc.viewport,
//    are highp, so the results stay highp).
// Shared verbatim by direct_present_gsr.frag, direct_present_gsr_edge.frag and the host model (tests/gsr1_shim_model.h).
// Sampler: LINEAR + CLAMP_TO_EDGE (textureLod gives the bilinear base colour; textureGather ignores the filter). Input: the
// map pass's 1280x720 picture (the game's scan-out tables applied: display-encoded values, which the 8/255 and 23/255
// constants assume). Precision: Qualcomm's `precision mediump float` (the including shader sets it); coordinates highp.
#ifndef DIRECT_GSR_MODE
#define DIRECT_GSR_MODE 1
#endif
float gsrFastLanczos2(float x)
{
    float wA = x - 4.0f;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}
#ifdef DIRECT_GSR_EDGE_DIRECTION
vec2 gsrWeightY(float dx, float dy, float c, vec3 data)
{
    float gsrStd = data.x;
    vec2 dir = vec2(data.y, data.z);
    float edgeDis = ((dx * dir.y) + (dy * dir.x));
    float x = (((dx * dx) + (dy * dy)) + ((edgeDis * edgeDis) * ((clamp(((c * c) * gsrStd), 0.0f, 1.0f) * 0.7f) + -1.0f)));
    float w = gsrFastLanczos2(x);
    return vec2(w, w * c);
}
vec2 gsrEdgeDirection(vec4 left, vec4 right)
{
    vec2 dir;
    float RxLz = (right.x + (-left.z));
    float RwLy = (right.w + (-left.y));
    vec2 delta;
    delta.x = (RxLz + RwLy);
    delta.y = (RxLz + (-RwLy));
    float lengthInv = inversesqrt((delta.x * delta.x + 3.075740e-05f) + (delta.y * delta.y));
    dir.x = (delta.x * lengthInv);
    dir.y = (delta.y * lengthInv);
    return dir;
}
#else
vec2 gsrWeightY(float dx, float dy, float c, float gsrStd)
{
    float x = ((dx * dx) + (dy * dy)) * 0.55f + clamp(abs(c) * gsrStd, 0.0f, 1.0f);
    float w = gsrFastLanczos2(x);
    return vec2(w, w * c);
}
#endif
// q = the output pixel relative to the image rect (0 .. size - 1); the result in [0, 1], alpha 1.
vec4 directGsr(ivec2 q)
{
    highp vec2 texCoord = (vec2(q) + 0.5f) * pc.params.xy;
    float edgeThreshold = pc.params.z;
    float edgeSharpness = pc.params.w;

    vec4 color = textureLod(source, texCoord, 0.0f);

    highp vec2 imgCoord = ((texCoord * pc.viewport.zw) + vec2(-0.5f, 0.5f));
    highp vec2 imgCoordPixel = floor(imgCoord);
    highp vec2 coord = (imgCoordPixel * pc.viewport.xy);
    vec2 pl = (imgCoord + (-imgCoordPixel));
    vec4 left = textureGather(source, coord, DIRECT_GSR_MODE);

    float edgeVote = abs(left.z - left.y) + abs(color[DIRECT_GSR_MODE] - left.y) + abs(color[DIRECT_GSR_MODE] - left.z);
    if (edgeVote > edgeThreshold)
    {
        coord.x += pc.viewport.x;

        vec4 right = textureGather(source, coord + vec2(pc.viewport.x, 0.0f), DIRECT_GSR_MODE);
        vec4 upGather = textureGather(source, coord + vec2(0.0f, -pc.viewport.y), DIRECT_GSR_MODE);
        vec4 downGather = textureGather(source, coord + vec2(0.0f, pc.viewport.y), DIRECT_GSR_MODE);
        vec4 upDown = vec4(upGather.w, upGather.z, downGather.y, downGather.x);

        float mean = (left.y + left.z + right.x + right.w) * 0.25f;
        left = left - vec4(mean);
        right = right - vec4(mean);
        upDown = upDown - vec4(mean);
        color.w = color[DIRECT_GSR_MODE] - mean;

        float sum = (((((abs(left.x) + abs(left.y)) + abs(left.z)) + abs(left.w)) + (((abs(right.x) + abs(right.y)) + abs(right.z)) + abs(right.w))) + (((abs(upDown.x) + abs(upDown.y)) + abs(upDown.z)) + abs(upDown.w)));
#ifdef DIRECT_GSR_EDGE_DIRECTION
        float sumMean = 1.014185e+01f / sum;
        float gsrStd = (sumMean * sumMean);
        vec3 data = vec3(gsrStd, gsrEdgeDirection(left, right));
#else
        float data = 2.181818f / sum;
#endif
        vec2 aWY = gsrWeightY(pl.x, pl.y + 1.0f, upDown.x, data);
        aWY += gsrWeightY(pl.x - 1.0f, pl.y + 1.0f, upDown.y, data);
        aWY += gsrWeightY(pl.x - 1.0f, pl.y - 2.0f, upDown.z, data);
        aWY += gsrWeightY(pl.x, pl.y - 2.0f, upDown.w, data);
        aWY += gsrWeightY(pl.x + 1.0f, pl.y - 1.0f, left.x, data);
        aWY += gsrWeightY(pl.x, pl.y - 1.0f, left.y, data);
        aWY += gsrWeightY(pl.x, pl.y, left.z, data);
        aWY += gsrWeightY(pl.x + 1.0f, pl.y, left.w, data);
        aWY += gsrWeightY(pl.x - 1.0f, pl.y - 1.0f, right.x, data);
        aWY += gsrWeightY(pl.x - 2.0f, pl.y - 1.0f, right.y, data);
        aWY += gsrWeightY(pl.x - 2.0f, pl.y, right.z, data);
        aWY += gsrWeightY(pl.x - 1.0f, pl.y, right.w, data);

        float finalY = aWY.y / aWY.x;

        float maxY = max(max(left.y, left.z), max(right.x, right.w));
        float minY = min(min(left.y, left.z), min(right.x, right.w));
        finalY = clamp(edgeSharpness * finalY, minY, maxY);

        float deltaY = finalY - color.w;

        //smooth high contrast input
        deltaY = clamp(deltaY, -23.0f / 255.0f, 23.0f / 255.0f);

        color.x = clamp((color.x + deltaY), 0.0f, 1.0f);
        color.y = clamp((color.y + deltaY), 0.0f, 1.0f);
        color.z = clamp((color.z + deltaY), 0.0f, 1.0f);
    }

    color.w = 1.0f; //assume alpha channel is not used
    return color;
}
