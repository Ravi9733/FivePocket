#version 450
#extension GL_GOOGLE_include_directive : require
// v42x (agent presenter-gpu) mode 4 (files/private-trace/diag-v42x-scale-gather with diag-v409-scale-cubic): the swapchain
// pass of mode 2 (referenceFiltered, Cubic) from the fast map pass's picture (direct_present_map.frag) with 12 gathers and no
// per-tap conversions (direct_present_cubic.glsl); the border black, alpha 1. direct_present.frag's set layout and push
// block (+ scale: the reference's fp32 1280 / image width, 720 / image height, from the CPU). A pipeline of its own, so
// direct_present.frag (modes 0-3) keeps v416's exact SPIR-V and register footprint.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Push { ivec4 rect; uint lutBase; uint mode; vec2 scale; } p;
layout(location = 0) out vec4 color;
#define DIRECT_FAST_GATHER 1
#include "direct_present_cubic.glsl"
void main() {
    const ivec2 q = ivec2(gl_FragCoord.xy) - p.rect.xy;
    if (q.x < 0 || q.y < 0 || q.x >= p.rect.z || q.y >= p.rect.w) { color = vec4(0.0, 0.0, 0.0, 1.0); return; }
    color = vec4(fastCubicGather(vec2(q)), 1.0);
}
