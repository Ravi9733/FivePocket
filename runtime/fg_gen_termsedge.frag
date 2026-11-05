#version 450
#extension GL_GOOGLE_include_directive : require
// fg-scaled (frame generation at a scaled picture size), pass 3a', for a picture that is not a whole number of 8 x 8 blocks: TERMS into a target of whole blocks (a texel outside the picture holds zero). The arithmetic is fg_gen_core.glsl (see there, behind pass 3: bindings, push block, the rule at the picture's edge).
// Drawn with direct_present.vert's one triangle; SPIR-V in fg_gen_spirv.h (local-analysis/agents/fg-scaled/tools/build_shaders.py).
layout(set = 0, binding = 0) uniform sampler2D uA;
layout(set = 0, binding = 1) uniform sampler2D uB;
layout(set = 0, binding = 2) uniform sampler2D uD;
layout(set = 0, binding = 3) uniform sampler2D uE;
layout(push_constant) uniform Push { vec4 m0; vec4 m1; vec4 m2; vec4 m3; vec4 a; vec4 b; } pc;
layout(location = 0) out vec4 color;
#include "fg_gen_core.glsl"
void main() { color = fgTermsEdge(gl_FragCoord.x, gl_FragCoord.y); }
