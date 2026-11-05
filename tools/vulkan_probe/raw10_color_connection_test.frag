#version 450
// Exact binary16 values and varying pixels expose lane/row/word-order errors.
// Unused bound textures are authored driver fixtures, not captured game data.
layout(location=0) out vec4 color0;
#ifdef MRT
layout(location=1) out vec4 color1;
#endif
void main() {
    uvec2 p=uvec2(gl_FragCoord.xy);
    color0=vec4(float(p.x%8u)/8.0,float(p.y%8u)/8.0,0.5,1.0);
#ifdef MRT
    color1=vec4(float(p.x+p.y*640u),0,0,1);
#endif
}
