#version 450
#extension GL_GOOGLE_include_directive : require
// v42x (agent presenter-gpu) modes 6/7 (files/private-trace/diag-v42x-scale-packed with diag-v409-scale-cubic): the swapchain
// pass of mode 2 (referenceFiltered, Cubic) from 4 gathers of the R32_UINT view of the fast map pass's picture (each gathered
// word = one texel's three mapped components; direct_present_cubic.glsl unpacks them); mode 6 = an RGBA8 picture, 7 = BGRA8
// (the components' memory order). The border black, alpha 1. direct_present.frag's push block; binding 0 = the R32_UINT view.
layout(set = 0, binding = 0) uniform usampler2D packedSource;
layout(push_constant) uniform Push { ivec4 rect; uint lutBase; uint mode; vec2 scale; } p;
layout(location = 0) out vec4 color;
#define DIRECT_FAST_PACKED 1
#include "direct_present_cubic.glsl"
void main() {
    const ivec2 q = ivec2(gl_FragCoord.xy) - p.rect.xy;
    if (q.x < 0 || q.y < 0 || q.x >= p.rect.z || q.y >= p.rect.w) { color = vec4(0.0, 0.0, 0.0, 1.0); return; }
    color = vec4(fastPackedRgb(fastCubicPacked(vec2(q)), p.mode == 7u), 1.0);
}
