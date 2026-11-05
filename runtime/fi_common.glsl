// Shared declarations of the frame-interpolation passes (fi_*.comp). Bindings: 0 prev frame, 1 current frame (rgba8,
// full res), 2/3 quarter-res luma, 4 coarse best (packed cost<<11|candidate), 5 mid frame, 6 fine best (cost<<6|candidate
// of +-3), 7 final motion (ivec2 per block, full-res pixels; cur(x) ~ prev(x + v)).
layout(set = 0, binding = 0, rgba8) uniform readonly image2D prevFrame;
layout(set = 0, binding = 1, rgba8) uniform readonly image2D curFrame;
layout(set = 0, binding = 2, r32f) uniform image2D prevLuma;
layout(set = 0, binding = 3, r32f) uniform image2D curLuma;
layout(set = 0, binding = 4, std430) buffer Coarse { uint coarse[]; };
layout(set = 0, binding = 5, rgba8) uniform writeonly image2D midFrame;
layout(set = 0, binding = 6, std430) buffer Fine { uint fine[]; };
layout(set = 0, binding = 7, std430) buffer Motion { ivec2 motion[]; };
layout(push_constant) uniform P { ivec2 size; ivec2 quarter; ivec2 blocks; int radius; float penalty; } pc;
float luma(vec4 c) { return dot(c.rgb, vec3(0.299, 0.587, 0.114)); }
ivec2 coarseVector(uint block) { // quarter-res candidate -> full-res pixels
    int side = 2 * pc.radius + 1; uint cand = coarse[block] & 2047u;
    return (ivec2(int(cand) % side, int(cand) / side) - pc.radius) * 4;
}
