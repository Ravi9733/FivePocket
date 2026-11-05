#version 450
// v255 direct present: packNativeRgbaSurface (letterbox, integer nearest neighbour, black borders, alpha 255) of
// gammaInPlace (per-channel 256-entry tables; alpha unchanged, then 255) of a 1280x720 RGBA8 UNORM source, per pixel.
// The table is identity (lutBase 768) for sources that are already mapped (CPU frames through mapNativeScanout).
// v409: mode 1 (linear) / 2 (cubic) = direct_present_reference.h's referenceFiltered, the same fp32 operations in its order.
// v410: lutBase 1536 = a source already mapped (direct_present.h's map pass): no table loads.
// v411: mode 3 = copy direct_scale.comp's picture 1:1.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1, std430) readonly buffer Lut { uint v[1536]; } lut;
layout(push_constant) uniform Push { ivec4 rect; uint lutBase; uint mode; } p; // rect = left, top, width, height of the image
layout(location = 0) out vec4 color;
vec3 mapped(ivec2 c) {
    c = clamp(c, ivec2(0), ivec2(1279, 719));
    const uvec3 b = uvec3(round(texelFetch(source, c, 0).rgb * 255.0));
    if (p.lutBase == 1536u) return vec3(b); // v410: the source is the map pass's output (the tables already applied)
    return vec3(float(lut.v[p.lutBase + b.r]), float(lut.v[p.lutBase + 256u + b.g]), float(lut.v[p.lutBase + 512u + b.b]));
}
vec4 cubicWeights(float t) { // the texels at -1, 0, +1, +2 from floor(u)
    const float t2 = t * t, t3 = t2 * t;
    return vec4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}
void main() {
    const ivec2 q = ivec2(gl_FragCoord.xy) - p.rect.xy;
    if (q.x < 0 || q.y < 0 || q.x >= p.rect.z || q.y >= p.rect.w) { color = vec4(0.0, 0.0, 0.0, 1.0); return; }
    if (p.mode == 3u) { // v411: the compute pass's picture (rect-sized, already mapped and filtered), 1:1
        const vec3 b = round(texelFetch(source, q, 0).rgb * 255.0);
        color = vec4((b + 0.25) / 255.0, 1.0);
        return;
    }
    if (p.mode == 0u) {
        const uint sx = uint(q.x) * 1280u / uint(p.rect.z), sy = uint(q.y) * 720u / uint(p.rect.w);
        const uvec3 b = uvec3(round(texelFetch(source, ivec2(sx, sy), 0).rgb * 255.0));
        const uvec3 m = uvec3(lut.v[p.lutBase + b.r], lut.v[p.lutBase + 256u + b.g], lut.v[p.lutBase + 512u + b.b]);
        // +0.25: every conversion that returns one of the two integers closest to m+0.25 (round or truncate) gives m.
        color = vec4((vec3(m) + 0.25) / 255.0, 1.0);
        return;
    }
    const vec2 u = (vec2(q) + 0.5) * vec2(1280.0 / float(p.rect.z), 720.0 / float(p.rect.w)) - 0.5;
    const vec2 f = floor(u), t = u - f;
    const ivec2 i = ivec2(f);
    vec3 v;
    if (p.mode == 1u) {
        const vec3 a = mapped(i), b = mapped(i + ivec2(1, 0)), d = mapped(i + ivec2(0, 1)), e = mapped(i + ivec2(1, 1));
        const vec3 top = a + (b - a) * t.x, bottom = d + (e - d) * t.x;
        v = top + (bottom - top) * t.y;
    } else {
        const vec4 wx = cubicWeights(t.x), wy = cubicWeights(t.y);
        vec3 lo = vec3(255.0), hi = vec3(0.0);
        v = vec3(0.0);
        for (int y = 0; y < 4; ++y) {
            vec3 row = vec3(0.0);
            for (int x = 0; x < 4; ++x) {
                const vec3 s = mapped(i + ivec2(x - 1, y - 1));
                row += wx[x] * s;
                if ((x == 1 || x == 2) && (y == 1 || y == 2)) { lo = min(lo, s); hi = max(hi, s); }
            }
            v += wy[y] * row;
        }
        v = clamp(v, lo, hi);
    }
    color = vec4(v / 255.0, 1.0);
}
