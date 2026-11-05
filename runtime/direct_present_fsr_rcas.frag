#version 450
#extension GL_GOOGLE_include_directive : require
// agent fsr1 (v450): FSR 1 RCAS pass (files/private-trace/diag-v450-scale-fsr1 without diag-v450-fsr1-no-rcas). Reads EASU's
// picture (the image rect's size, RGBA8), writes the swapchain image: inside the rect RCAS of that picture, outside black,
// alpha 1 (as every presenter mode).
// Compiled with: glslc --target-env=vulkan1.1 -O -I third_party/fsr1 direct_present_fsr_rcas.frag (AMD's headers unmodified).
// ffx_a.h / ffx_fsr1.h: Copyright (c) 2021 Advanced Micro Devices, Inc., MIT licence (third_party/fsr1/license.txt).
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Push { ivec4 rect; uvec4 con0; uvec4 con1; uvec4 con2; uvec4 con3; } pc;
layout(location = 0) out vec4 color;
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F 1
#include "ffx_fsr1.h"
#define DIRECT_FSR_RCAS 1
#include "direct_present_fsr.glsl"
void main() {
    const ivec2 q = ivec2(gl_FragCoord.xy) - pc.rect.xy;
    if (q.x < 0 || q.y < 0 || q.x >= pc.rect.z || q.y >= pc.rect.w) { color = vec4(0.0, 0.0, 0.0, 1.0); return; }
    color = vec4(directFsrRcas(q), 1.0);
}
