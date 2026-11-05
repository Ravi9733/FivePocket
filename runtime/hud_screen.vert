#version 450
// hud3 (v585): the second screen's HUD quads (hud_screen_layout.h drawList). One push-constant block per draw: the quad
// `rect` in second-screen pixels, grown by `param.w` pixels on each side (antialiasing apron); `screen.xy` = the HUD image.
// Four vertices, triangle strip. Compiled with: glslc --target-env=vulkan1.1 -O hud_screen.vert (NDK r27 shader-tools).
layout(push_constant) uniform Push { vec4 rect; vec4 color; vec4 color2; vec4 param; vec4 shape; vec4 screen; } p;
void main() {
    const vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    const vec2 q = mix(p.rect.xy - p.param.w, p.rect.zw + p.param.w, c);
    gl_Position = vec4(q / p.screen.xy * 2.0 - 1.0, 0.0, 1.0);
}
