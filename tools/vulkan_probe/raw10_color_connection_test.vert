#version 450
// Authored coverage shader, NOT a translated guest shader.
layout(push_constant) uniform Viewport { vec4 scaleOffset; } viewport;
void main() {
    const vec2 corners[6]=vec2[6](vec2(-1,1),vec2(1,1),vec2(-1,-1),
                                vec2(-1,-1),vec2(1,1),vec2(1,-1));
    gl_Position=vec4(corners[gl_VertexIndex]*viewport.scaleOffset.xy+viewport.scaleOffset.zw,0,1);
}
