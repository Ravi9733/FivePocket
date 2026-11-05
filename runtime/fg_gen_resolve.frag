#version 450
#extension GL_GOOGLE_include_directive : require
// framegen-1b (frame generation, stage 1b), pass 4 of 5, RESOLVE: open blocks take their neighbours' majority (run three times). The arithmetic is fg_gen_core.glsl (see there: bindings, push block).
// Drawn with direct_present.vert's one triangle; SPIR-V in fg_gen_spirv.h (tools/build_shaders.sh).
layout(set = 0, binding = 0) uniform sampler2D uA;
layout(set = 0, binding = 1) uniform sampler2D uB;
layout(set = 0, binding = 2) uniform sampler2D uD;
layout(set = 0, binding = 3) uniform sampler2D uE;
layout(push_constant) uniform Push { vec4 m0; vec4 m1; vec4 m2; vec4 m3; vec4 a; vec4 b; } pc;
layout(location = 0) out vec4 color;
#include "fg_gen_core.glsl"
void main() { color = fgResolve(gl_FragCoord.x, gl_FragCoord.y); }
