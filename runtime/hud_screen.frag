#version 450
// hud3 (v585): the second screen's HUD, drawn at its own resolution (hud_screen_layout.h drawList; the CPU twin used by the
// host tests and the preview is local-analysis/agents/hud3/tests/hud3_reference.h - keep the two in step).
//  kind 0 box:   a rounded rectangle `shape` (radius param.y), colour `color`; the frame's strips share one shape.
//  kind 1 radar: the game's own radar render target (the capture image, hud_screen.h): quad `rect` shows texels `shape`;
//                inside the map (texels color2) a Catmull-Rom filter whose taps stay inside the map (its edge is not
//                blended with the transparent margin); in the margin (blips reaching past the map's edge) a bilinear filter
//                of the margin's texels only, with their alpha. Then the game's display tables (`lut`, the main screen's
//                gamma) and the dim factor param.z.
//  kind 2 bar:   rectangle `shape`, the left param.z of it `color`, the rest `color2` (the track).
//  kind 3 star:  a five-pointed star in `shape` (inner ratio param.z), fill `color`, outline `color2` of param.y pixels.
//  kind 6 shadow (v681, Thor): a soft shadow outside rounded box `shape`, none over box color2 (the Resume pill's border).
//  kind 5 label (v667, Thor): binding 0 texel for texel (premultiplied in, straight out): the Resume control's label.
//  kind 4 flags (hud6 v618, param.y): 1 = premultiplied alpha out (the game's stars over the map), 2 = area filter when scaled down,
//                4 (hud8 v625) = with 1: coverage below ~0.25-0.55 dropped (smoothstep; the box layer's faint frame shading).
//                8 (v638, look=game) = opaque texels drawn with the constant alpha param.w (the map at the game's 0.5).
//  kind 4 replica (hud4 v597): the renderer's replay of the game's own minimap box (binding 2, the main frame's format): quad `rect`
//                shows texels `shape` of it (1/size in color2.xy): texel for texel (texelFetch) when the two sizes are equal (the
//                box fits the screen: the Thor), else bilinear; then
//                the display tables exactly as the main screen's presenter applies them (round to 8 bits, the game's tables) and
//                the dim factor param.z; opaque (the replay drew the box over black).
// Compiled with: glslc --target-env=vulkan1.1 -O hud_screen.frag (NDK r27 shader-tools).
layout(set = 0, binding = 0) uniform sampler2D radar; // linear, clamp to edge; RGBA8 UNORM, RadarCapacity texels square
layout(set = 0, binding = 1, std430) readonly buffer Lut { uint v[1536]; } lut;
layout(set = 0, binding = 2) uniform sampler2D replica; // hud4: linear, clamp to edge
layout(push_constant) uniform Push { vec4 rect; vec4 color; vec4 color2; vec4 param; vec4 shape; vec4 screen; } p;
layout(location = 0) out vec4 o;

float cover(float d) { return clamp(0.5 - d, 0.0, 1.0); }
float roundedBox(vec2 q, vec4 r, float radius) {
    const vec2 c = (r.xy + r.zw) * 0.5, h = (r.zw - r.xy) * 0.5;
    const vec2 d = abs(q - c) - h + radius;
    return length(max(d, vec2(0.0))) + min(max(d.x, d.y), 0.0) - radius;
}
// Inigo Quilez's exact distance to a regular five-pointed star (outer radius r, inner radius rf * r), y up, a point up.
float star5(vec2 q, float r, float rf) {
    const vec2 k1 = vec2(0.809016994375, -0.587785252292);
    const vec2 k2 = vec2(-k1.x, k1.y);
    q.x = abs(q.x);
    q -= 2.0 * max(dot(k1, q), 0.0) * k1;
    q -= 2.0 * max(dot(k2, q), 0.0) * k2;
    q.x = abs(q.x);
    q.y -= r;
    const vec2 ba = rf * vec2(-k1.y, k1.x) - vec2(0.0, 1.0);
    const float h = clamp(dot(q, ba) / dot(ba, ba), 0.0, r);
    return length(q - ba * h) * sign(q.y * ba.x - q.x * ba.y);
}
vec3 tap(vec2 t) { return texture(radar, t * p.screen.zw).rgb; }
// 5-tap Catmull-Rom (the four corner taps dropped), tap centres clamped to [lo, hi] (texel centres of the map's edge).
vec3 catmullRom(vec2 t, vec2 lo, vec2 hi) {
    const vec2 s = t - 0.5;
    const vec2 i = floor(s), f = s - i;
    const vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const vec2 w3 = f * f * (-0.5 + 0.5 * f);
    const vec2 w12 = w1 + w2;
    const vec2 c = i + 0.5;
    const vec2 t0 = clamp(c - 1.0, lo, hi), t3 = clamp(c + 2.0, lo, hi), t12 = clamp(c + w2 / w12, lo, hi);
    const vec3 r = tap(vec2(t12.x, t0.y)) * (w12.x * w0.y) + tap(vec2(t0.x, t12.y)) * (w0.x * w12.y) + tap(t12) * (w12.x * w12.y) +
                   tap(vec2(t3.x, t12.y)) * (w3.x * w12.y) + tap(vec2(t12.x, t3.y)) * (w12.x * w3.y);
    const float sum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return clamp(r / sum, 0.0, 1.0);
}
bool inMap(vec2 centre) { return all(greaterThan(centre, p.color2.xy)) && all(lessThan(centre, p.color2.zw)); }
// Bilinear over the margin's texels only (a map texel weighs as transparent), straight alpha out.
vec4 margin(vec2 t) {
    const vec2 s = t - 0.5;
    const vec2 i = floor(s), f = s - i;
    const int last = int(1.0 / p.screen.z) - 1;
    vec3 rgb = vec3(0.0);
    float a = 0.0;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            const vec2 c = i + vec2(float(x), float(y));
            const float w = (x == 1 ? f.x : 1.0 - f.x) * (y == 1 ? f.y : 1.0 - f.y);
            if (inMap(c + 0.5)) continue;
            const vec4 v = texelFetch(radar, clamp(ivec2(c), ivec2(0), ivec2(last)), 0);
            rgb += v.rgb * (v.a * w);
            a += v.a * w;
        }
    return vec4(a > 0.0 ? rgb / a : vec3(0.0), a);
}
vec3 table(vec3 c) {
    const uvec3 b = uvec3(round(clamp(c, 0.0, 1.0) * 255.0));
    return vec3(float(lut.v[b.r]), float(lut.v[256u + b.g]), float(lut.v[512u + b.b])) / 255.0;
}
void main() {
    const vec2 q = gl_FragCoord.xy;
    const uint kind = uint(p.param.x);
    if (kind == 1u) {
        const vec2 t = mix(p.shape.xy, p.shape.zw, (q - p.rect.xy) / (p.rect.zw - p.rect.xy));
        vec4 v;
        if (inMap(t)) v = vec4(catmullRom(t, p.color2.xy + 0.5, p.color2.zw - 0.5), 1.0);
        else v = margin(t);
        o = vec4(table(v.rgb) * p.param.z, v.a);
    } else if (kind == 4u) {
        const vec2 size = p.rect.zw - p.rect.xy, texels = p.shape.zw - p.shape.xy;
        const uint flags = uint(p.param.y); // hud6 (v618): 1 = premultiplied alpha (the stars), 2 = area filter when scaled down
        vec4 v;
        if ((flags & 32u) != 0u) { // v822 (thor-radio): a landscape phone app, turned back a quarter: texel u = 1 - screen v, texel v = screen u
            const vec2 s = (q - p.rect.xy) / size;
            v = texture(replica, mix(p.shape.xy, p.shape.zw, vec2(1.0 - s.y, s.x)) * p.color2.xy);
        }
        else if (all(equal(size, texels))) v = texelFetch(replica, ivec2(p.shape.xy + floor(q - p.rect.xy)), 0); // texel for texel: exact
        else if ((flags & 2u) != 0u && texels.x > size.x * 1.25) {
            // scaled down by f: the mean of n x n bilinear taps spread over the f x f texels this pixel covers (n = ceil(f), at most 6)
            const vec2 f = texels / size;
            const int n = int(clamp(ceil(max(f.x, f.y)), 1.0, 6.0));
            const vec2 base = p.shape.xy + (q - p.rect.xy - 0.5) * f;
            v = vec4(0.0);
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i)
                    v += texture(replica, (base + (vec2(float(i), float(j)) + 0.5) * f / float(n)) * p.color2.xy);
            v /= float(n * n);
        } else v = texture(replica, mix(p.shape.xy, p.shape.zw, (q - p.rect.xy) / size) * p.color2.xy);  // bilinear
        if ((flags & 1u) != 0u) { // (over the map; flag 4, hud8 v625: the box's faint shading (alpha ~0.2) dropped, marks and bars kept)
            const float a = (flags & 4u) != 0u ? smoothstep(0.25, 0.55, v.a) : v.a;
            o = vec4(table(v.a > 0.0 ? v.rgb / v.a : vec3(0.0)) * p.param.z, a);
        }
        else o = vec4(table(v.rgb) * p.param.z, (flags & 8u) != 0u ? p.param.w : 1.0); // (v638 flag 8: a constant alpha, param.w)
        if ((flags & 16u) != 0u) { // thor-char (v693): the pause menu's look - gain color.a on a mix (saturation color2.z) of the colour and
            // its luma, plus a per-channel offset color.rgb (display values; loading_layout.h PauseTint, fitted on the game's own pause frames)
            const float l = dot(o.rgb, vec3(0.2126, 0.7152, 0.0722));
            o.rgb = clamp(p.color.a * mix(vec3(l), o.rgb, p.color2.z) + p.color.rgb, 0.0, 1.0);
        }
    } else if (kind == 2u) {
        const float inside = cover(roundedBox(q, p.shape, p.param.y));
        const float split = p.shape.x + p.param.z * (p.shape.z - p.shape.x);
        const vec4 c = mix(p.color2, p.color, clamp(split - q.x + 0.5, 0.0, 1.0));
        o = vec4(c.rgb, c.a * inside);
    } else if (kind == 6u) {
        // thor-maptap (v681): a soft shadow round a rounded box: colour at color.a fading to 0 over param.z pixels outside `shape`
        // (radius param.y), never over the box `color2` itself (the pill it frames keeps its own look; its 1-px edge blends).
        const float d = roundedBox(q, p.shape, p.param.y);
        const float inside = clamp(0.5 - roundedBox(q, p.color2, p.param.y), 0.0, 1.0);
        const float a = 1.0 - smoothstep(0.0, p.param.z, max(d, 0.0));
        o = vec4(p.color.rgb, p.color.a * a * (1.0 - inside));
    } else if (kind == 5u) {
        // thor-maptap (v667): the Resume label at binding 0 (the radar's binding in this pass's set), texel for texel from texel
        // shape.xy; premultiplied in the image, straight alpha out.
        const vec4 v = texelFetch(radar, ivec2(p.shape.xy + floor(q - p.rect.xy)), 0);
        o = vec4(v.a > 0.0 ? v.rgb / v.a : vec3(0.0), v.a);
    } else if (kind == 3u) {
        const float s = min(p.shape.z - p.shape.x, p.shape.w - p.shape.y);
        const float r = s * (0.96 / 1.902 - 0.07);
        const vec2 centre = vec2((p.shape.x + p.shape.z) * 0.5, (p.shape.y + p.shape.w) * 0.5 + 0.0955 * r);
        const float d = star5(vec2(q.x - centre.x, centre.y - q.y), r, p.param.z);
        const float fill = cover(d);
        if (p.param.y > 0.0) {
            const vec4 c = mix(p.color2, p.color, fill);
            o = vec4(c.rgb, max(c.a * cover(d - p.param.y), p.color.a * fill));
        } else {
            o = vec4(p.color.rgb, p.color.a * fill);
        }
    } else {
        o = vec4(p.color.rgb, p.color.a * cover(roundedBox(q, p.shape, p.param.y)));
    }
}
