#version 450
// Diagnostic interface only: expose interpolated UV and window depth without
// textures or a title pixel shader. Compile once for each explicit VS interface.
#ifdef RECOMP_R8_UV
layout(location=0) in vec4 coordinate;
#else
layout(location=0) in vec2 coordinate;
#endif
layout(location=0) out vec4 result;
void main() { result=vec4(coordinate.xy,gl_FragCoord.z,1); }
