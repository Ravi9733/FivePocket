// v42x (agent presenter-gpu): mode 2's cubic (direct_present_reference.h referenceFiltered, Filter::Cubic: Catmull-Rom over
// the 4x4 around u, clamped per channel to the 2x2 at floor(u)) from the map pass's picture (the tables already applied),
// with gathers instead of v410's 16 point fetches and their per-tap round / uint / float conversions.
// Shared verbatim by direct_present_gather.frag (DIRECT_FAST_GATHER: mode 4, 12 gathers of the RGBA8 picture),
// direct_present_packed.frag (DIRECT_FAST_PACKED: modes 6/7, 4 gathers of its R32_UINT view, the bytes unpacked here) and the
// host model tools/presenter_gpu/fast_model.h: only syntax that is both GLSL 4.50 and C++ (f-suffixed literals,
// single-component swizzles, no parameter qualifiers), so the model runs this text.
// u = (q + 0.5) * scale - 0.5 with scale = the reference's own fp32 1280 / image width, 720 / image height (push constants).
// Footprint: textureGatherOffset(s, c, o, k) at c = f / (1280, 720), f = floor(u), reads the texels floor(c * size - 0.5) + o
// and + o + 1 per axis = f - 1 + o, f + o (c * size is within 1e-4 of the integer f: 0.5 texel from a selection boundary),
// clamped to the edge as mappedTexel clamps, and returns component k of (i0,j1), (i1,j1), (i1,j0), (i0,j0) as x, y, z, w.
// Offsets (0,0), (2,0), (0,2), (2,2) give the blocks a, b, d, e covering the taps f-1 .. f+2 in x and y.
vec4 fastCubicWeights(float t) { // = cubicWeights: the texels at -1, 0, +1, +2 from floor(u)
    const float t2 = t * t, t3 = t2 * t;
    return vec4(-0.5f * t3 + t2 - 0.5f * t, 1.5f * t3 - 2.5f * t2 + 1.0f, -1.5f * t3 + 2.0f * t2 + 0.5f * t, 0.5f * t3 - 0.5f * t2);
}
// One channel from the blocks a (x f-1..f, y f-1..f), b (x f+1..f+2, y f-1..f), d (x f-1..f, y f+1..f+2), e (x f+1..f+2,
// y f+1..f+2) in gather order: the rows in the reference's order (row = w0 s0 + w1 s1 + w2 s2 + w3 s3, v = wy0 row0 + ... +
// wy3 row3), then the clamp to the 2x2 (f, f) .. (f+1, f+1) = a.y, b.x, d.z, e.w.
float fastChannel(vec4 a, vec4 b, vec4 d, vec4 e, vec4 wx, vec4 wy) {
    const float r0 = wx.x * a.w + wx.y * a.z + wx.z * b.w + wx.w * b.z; // y = f - 1
    const float r1 = wx.x * a.x + wx.y * a.y + wx.z * b.x + wx.w * b.y; // y = f
    const float r2 = wx.x * d.w + wx.y * d.z + wx.z * e.w + wx.w * e.z; // y = f + 1
    const float r3 = wx.x * d.x + wx.y * d.y + wx.z * e.x + wx.w * e.y; // y = f + 2
    const float v = wy.x * r0 + wy.y * r1 + wy.z * r2 + wy.w * r3;
    return clamp(v, min(min(a.y, b.x), min(d.z, e.w)), max(max(a.y, b.x), max(d.z, e.w)));
}
#ifdef DIRECT_FAST_GATHER
// Mode 4: q = the output pixel relative to the image rect; the result in [0, 1] (the picture's texels arrive as k / 255).
vec3 fastCubicGather(vec2 q) {
    const vec2 u = (q + 0.5f) * p.scale - 0.5f;
    const vec2 f = floor(u);
    const vec2 t = u - f;
    const vec4 wx = fastCubicWeights(t.x);
    const vec4 wy = fastCubicWeights(t.y);
    const vec2 c = f * vec2(1.0f / 1280.0f, 1.0f / 720.0f);
    vec3 v;
    v.x = fastChannel(textureGatherOffset(source, c, ivec2(0, 0), 0), textureGatherOffset(source, c, ivec2(2, 0), 0),
                      textureGatherOffset(source, c, ivec2(0, 2), 0), textureGatherOffset(source, c, ivec2(2, 2), 0), wx, wy);
    v.y = fastChannel(textureGatherOffset(source, c, ivec2(0, 0), 1), textureGatherOffset(source, c, ivec2(2, 0), 1),
                      textureGatherOffset(source, c, ivec2(0, 2), 1), textureGatherOffset(source, c, ivec2(2, 2), 1), wx, wy);
    v.z = fastChannel(textureGatherOffset(source, c, ivec2(0, 0), 2), textureGatherOffset(source, c, ivec2(2, 0), 2),
                      textureGatherOffset(source, c, ivec2(0, 2), 2), textureGatherOffset(source, c, ivec2(2, 2), 2), wx, wy);
    return v;
}
#endif
#ifdef DIRECT_FAST_PACKED
// Modes 6/7: the picture's R32_UINT view (bytes 0, 1, 2 = the map image's first three components in memory order; byte 3 =
// alpha, which the fast map pass writes as 0, so byte 2 needs no mask). Values in [0, 255], the result scaled to [0, 1].
vec4 fastByte0(uvec4 w) { return vec4(float(w.x & 255u), float(w.y & 255u), float(w.z & 255u), float(w.w & 255u)); }
vec4 fastByte1(uvec4 w) { return vec4(float((w.x >> 8u) & 255u), float((w.y >> 8u) & 255u), float((w.z >> 8u) & 255u), float((w.w >> 8u) & 255u)); }
vec4 fastByte2(uvec4 w) { return vec4(float(w.x >> 16u), float(w.y >> 16u), float(w.z >> 16u), float(w.w >> 16u)); }
vec3 fastCubicPacked(vec2 q) { // components in the map image's byte order (r, g, b for RGBA8; b, g, r for BGRA8)
    const vec2 u = (q + 0.5f) * p.scale - 0.5f;
    const vec2 f = floor(u);
    const vec2 t = u - f;
    const vec4 wx = fastCubicWeights(t.x);
    const vec4 wy = fastCubicWeights(t.y);
    const vec2 c = f * vec2(1.0f / 1280.0f, 1.0f / 720.0f);
    const uvec4 a = textureGatherOffset(packedSource, c, ivec2(0, 0), 0);
    const uvec4 b = textureGatherOffset(packedSource, c, ivec2(2, 0), 0);
    const uvec4 d = textureGatherOffset(packedSource, c, ivec2(0, 2), 0);
    const uvec4 e = textureGatherOffset(packedSource, c, ivec2(2, 2), 0);
    vec3 v;
    v.x = fastChannel(fastByte0(a), fastByte0(b), fastByte0(d), fastByte0(e), wx, wy);
    v.y = fastChannel(fastByte1(a), fastByte1(b), fastByte1(d), fastByte1(e), wx, wy);
    v.z = fastChannel(fastByte2(a), fastByte2(b), fastByte2(d), fastByte2(e), wx, wy);
    return v * (1.0f / 255.0f);
}
vec3 fastPackedRgb(vec3 v, bool bgra) { return bgra ? vec3(v.z, v.y, v.x) : v; } // memory order -> r, g, b
#endif
