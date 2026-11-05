#version 450
#extension GL_GOOGLE_include_directive : require
//============================================================================================================
//
//
//                  Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
// v460 (agent gsr1): Snapdragon Game Super Resolution 1 pass (files/private-trace/diag-v460-scale-gsr1 with diag-v460-gsr1-edge-direction).
// The scaler text is direct_present_gsr.glsl, derived from Qualcomm's sgsr1_shader_mobile_edge_direction.frag with UseEdgeDirection
// (BSD-3-Clause, notice above; what was changed is listed in that file). Reads the map pass's 1280x720 picture through a
// LINEAR + CLAMP_TO_EDGE sampler and writes the swapchain image: inside the image rect the scaled picture, outside black,
// alpha 1 (as every presenter mode). One pass, no intermediate picture.
// Compiled with: glslc --target-env=vulkan1.1 -O direct_present_gsr_edge.frag
precision mediump float;
precision highp int;
layout(set = 0, binding = 0) uniform mediump sampler2D source;
// rect = left, top, width, height of the image; viewport = 1/1280, 1/720, 1280, 720 (Qualcomm's ViewportInfo[0]);
// params = 1/width, 1/height, edge threshold, edge sharpness
layout(push_constant) uniform Push { highp ivec4 rect; highp vec4 viewport; highp vec4 params; } pc;
layout(location = 0) out vec4 target;
#define DIRECT_GSR_EDGE_DIRECTION 1
#include "direct_present_gsr.glsl"
void main() {
    const highp ivec2 q = ivec2(gl_FragCoord.xy) - pc.rect.xy;
    if (q.x < 0 || q.y < 0 || q.x >= pc.rect.z || q.y >= pc.rect.w) { target = vec4(0.0, 0.0, 0.0, 1.0); return; }
    target = directGsr(q);
}
