#version 450
// Independently authored fixture, NOT a translated game vertex shader.
layout(location=0) out vec2 coordinate;
void main() {
    vec2 corners[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    vec2 p=corners[gl_VertexIndex];
    gl_Position=vec4(p,0,1);
    coordinate=(p+vec2(1))*0.5;
}
