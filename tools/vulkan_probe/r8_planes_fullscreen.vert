#version 450
// Authored partner, explicit UV duplication. Not title linkage proof.
layout(location=0) out vec4 coordinate;
void main() {
    vec2 p=gl_VertexIndex==0?vec2(-1,-1):gl_VertexIndex==1?vec2(3,-1):vec2(-1,3);
    gl_Position=vec4(p,0,1);
    vec2 uv=(p+1.0)*0.5;
    coordinate=vec4(uv,uv);
}
