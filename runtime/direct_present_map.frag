#version 450
// v42x (agent presenter-gpu): the fast modes' map pass (mode 5): each texel of the 1280x720 source through the tables at 1:1
// into the picture (v410's map pass = mode 0 at 1:1, the same bytes, without mode 0's two integer divisions per texel), alpha
// 0 (the packed modes read byte 3 of each texel as zero). direct_present.frag's set layout and push block.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1, std430) readonly buffer Lut { uint v[1536]; } lut;
layout(push_constant) uniform Push { ivec4 rect; uint lutBase; uint mode; vec2 scale; } p;
layout(location = 0) out vec4 color;
void main() {
    const ivec2 q = ivec2(gl_FragCoord.xy); // (the framebuffer is the 1280x720 picture)
    const uvec3 b = uvec3(round(texelFetch(source, q, 0).rgb * 255.0));
    const uvec3 m = uvec3(lut.v[p.lutBase + b.r], lut.v[p.lutBase + 256u + b.g], lut.v[p.lutBase + 512u + b.b]);
    // +0.25: every conversion that returns one of the two integers closest to m+0.25 (round or truncate) gives m (as mode 0).
    color = vec4((vec3(m) + 0.25) / 255.0, 0.0);
}
