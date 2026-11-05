// framegen-1b (frame generation, stage 1b): the arithmetic of the generator's five passes. Our own code.
// framegen-cost (2026-10-06): the GAIN pass is drawn in two steps (GAIN COLUMNS, then GAIN: six shader modules); every
// other function below is v467's text, and their four modules in fg_gen_spirv.h are v467's words (tools/spirv_check.py).
// framegen-cost-b (2026-10-06, stage B1): the CLASSIFY pass is drawn in two steps (TERMS, then CLASSIFY: seven shader
// modules). EXACT: the block map has the bytes it had (see pass 3). Every other function is v468's text, and the modules
// Low, Gain, Gaincols, Resolve and Synth are v468's words (framegen-cost-b/tools/spirv_check.py).
// fg-scaled (2026-10-10): the picture may be any of the renderer's sizes up to 1280x720 (pc.a holds it, as before). Two
// functions are ADDED for a picture that is not a whole number of blocks (fgTermsEdge, fgClassifyEdge: two more shader
// modules, see behind pass 3); every function that was here is v513's text, and the seven modules made from them are
// v513's words (local-analysis/agents/fg-scaled/tools/build_shaders.py refuses to write anything else).
// fg-fixer (2026-10-23, v630; every edit is marked fg-fixer): thin surfaces in front of far ones (palms, poles, wires against
// the sky) are judged and moved by the NEAREST depth around a pixel (fgNear, stored by TERMS in its A channel and read by
// SYNTHESIS, whose uD is now the terms' picture: fg_gen.h Bindings), and a refused block whose own pixels say "static world"
// is static world (fgPromote). The modules Terms, Termsedge, Classify, Classifyedge and Synth change; the block map no longer
// has v468's bytes (that was the point: the v623 mosaic). Low, Gain, Gaincols and Resolve are v623's words.
// The algorithm is DESIGN-stage1.md section 2 (local-analysis/agents/frame-gen/), operation for operation the reference
// host-model/fg_reference.h (classify with the low-passed, exposure-matched costs; interpolate, stage-1 form).
//
// ONE TEXT, TWO COMPILERS: glslc compiles it into the passes (fg_gen_*.frag include it), and the host tests compile the same
// text as C++ (tests/glsl_shim.h), so the offline evaluator runs what the phone runs. To keep that possible:
//   float literals carry the f suffix; no swizzles; no out / inout parameters; vectors are built only as vec2(x, y),
//   vec3(x, y, z), vec4(x, y, z, w), ivec2(x, y); plain for loops; nothing but the functions used below.
//   (framegen-cost-b: and macros with arguments that expand to plain statements: FG_TERM, the unrolled sum of pass 3b.)
//
// Bindings (the same set layout for every pass; what a binding holds depends on the pass):
//   uA, uB  two pictures (RGBA8, the picture's size)     uD  the scene depth (reversed Z; at least the picture's size)
//   uE      the gain picture (1x1) or the block map (160x90)
//   (the GAIN pass alone reads the column sums, 160x2, through uA; framegen-cost-b: the CLASSIFY pass alone reads the
//   terms, one RGBA32F texel per pixel, through uA)
//   (fg-scaled: 160x90 and 160x2 are the sizes for 1280x720. In general the block map is ceil(width / 8) x
//   ceil(height / 8), the column sums ceil(width / 8) x 2, the terms' picture whole blocks: fg_gen.h Geometry.)
// Push block pc: m0..m3 = rows of the cur -> prev matrix (row-vector convention: clip = nx*m0 + ny*m1 + depth*m2 + m3);
//   a = (picture width, picture height, phase, DISAGREE); b = (FAIL, AMBIGUOUS, show, 0).
// Pictures are 8-bit: every texel is taken as its integer 0..255 (fgByte), so the sums below are exact in float32.
#define FG_CAMERA 0
#define FG_ZERO 1
#define FG_NONE 3
#define FG_BLOCK 8

float fgByte(float t) { return floor(t * 255.0f + 0.5f); }

// A texel as integers 0..255, clamped to the picture's edge.
vec3 fgTexel(sampler2D s, int x, int y) {
    const ivec2 size = textureSize(s, 0);
    const vec4 t = texelFetch(s, ivec2(clamp(x, 0, size.x - 1), clamp(y, 0, size.y - 1)), 0);
    return vec3(fgByte(t.x), fgByte(t.y), fgByte(t.z));
}

// Bilinear sample at a continuous position (pixel centres at x + 0.5), from four texels with explicit weights: the same
// bits on every GPU and on the host. Positions far outside the picture read the edge texel.
vec3 fgBilinear(sampler2D s, float x, float y) {
    const float u = x - 0.5f, v = y - 0.5f;
    const float fx = floor(u), fy = floor(v);
    const int ix = int(clamp(fx, -16384.0f, 16384.0f)), iy = int(clamp(fy, -16384.0f, 16384.0f));
    const float ax = u - fx, ay = v - fy;
    const vec3 a = fgTexel(s, ix, iy), b = fgTexel(s, ix + 1, iy), c = fgTexel(s, ix, iy + 1), d = fgTexel(s, ix + 1, iy + 1);
    return (a * (1.0f - ax) + b * ax) * (1.0f - ay) + (c * (1.0f - ax) + d * ax) * ay;
}

float fgAbsDiff(vec3 a, vec3 b) { return (abs(a.x - b.x) + abs(a.y - b.y) + abs(a.z - b.z)) * (1.0f / 3.0f); }
// framegen-cost-b: fgAbsDiff before its multiplication by a third (what the TERMS pass stores; pass 3 says why).
float fgAbsSum(vec3 a, vec3 b) { return abs(a.x - b.x) + abs(a.y - b.y) + abs(a.z - b.z); }

// The scene depth under a continuous position: the nearest texel, clamped to the PICTURE (the depth image may be taller).
// (fg-fixer, v630: no pass reads it any more; fgNear reads the depth.)
float fgDepth(float x, float y) {
    const int ix = int(clamp(floor(x), 0.0f, pc.a.x - 1.0f)), iy = int(clamp(floor(y), 0.0f, pc.a.y - 1.0f));
    return texelFetch(uD, ivec2(ix, iy), 0).x;
}

// fg-fixer (2026-10-23, v630): the NEAREST surface around pixel (x, y): the largest scene depth (reversed Z) of the 6 x 6
// texels x - 2 .. x + 3, y - 2 .. y + 3, clamped to the picture. Read with nine gathers at texel corners (each one the 2 x 2
// texels around it, component 0); a corner is kept inside the picture, so no gather reads outside it (the depth image may be
// taller than the picture) and the clamped window is exactly the texels of the nine gathers.
// Why: a thin surface in front of a far one (a palm frond, a pole, a wire against the sky) is one to three pixels wide. The
// 4 x 4 low-pass of pass 3 spreads it over pixels whose own depth is the sky's, and the synthesis' search for the source of a
// pixel started on the sky never lands on it. Measured with the pixel's own depth, its 8 x 8 block failed the static-world
// test under any camera translation and was drawn unmoved: the Thor's v623 mosaic on palms and poles (G4's limit). With the
// nearest depth of the window the thin surface moves with its own camera vector in both passes.
float fgNear(int x, int y) {
    const ivec2 size = textureSize(uD, 0);
    const float sx = 1.0f / float(size.x), sy = 1.0f / float(size.y);
    const int xmax = int(pc.a.x) - 1, ymax = int(pc.a.y) - 1;
    float dn = 0.0f;
    for(int j = 0; j <= 2; ++j) for(int i = 0; i <= 2; ++i) {
        const vec4 g = textureGather(uD, vec2(float(clamp(x - 1 + 2 * i, 1, xmax)) * sx, float(clamp(y - 1 + 2 * j, 1, ymax)) * sy));
        if(g.x > dn) dn = g.x;
        if(g.y > dn) dn = g.y;
        if(g.z > dn) dn = g.z;
        if(g.w > dn) dn = g.w;
    }
    return dn;
}

// The camera vector of the surface seen at (x, y) with this depth: where it is now minus where it was in the previous real
// frame, in pixels, if it did not move in the world. Zero behind the previous camera.
vec2 fgCameraVector(float x, float y, float depth) {
    const float nx = 2.0f * x / pc.a.x - 1.0f, ny = 1.0f - 2.0f * y / pc.a.y;
    const float cx = nx * pc.m0.x + ny * pc.m1.x + depth * pc.m2.x + pc.m3.x;
    const float cy = nx * pc.m0.y + ny * pc.m1.y + depth * pc.m2.y + pc.m3.y;
    const float cw = nx * pc.m0.w + ny * pc.m1.w + depth * pc.m2.w + pc.m3.w;
    if(!(cw > 1e-6f)) return vec2(0.0f, 0.0f);
    const float px = (cx / cw + 1.0f) * 0.5f * pc.a.x, py = (1.0f - cy / cw) * 0.5f * pc.a.y;
    return vec2(x - px, y - py);
}

// ---- pass 1, LOW (target: a picture). uA = the real picture through a LINEAR sampler.
// The 4 x 4 box around the texel (texels x-2 .. x+1, y-2 .. y+1, clamped to the edge): four bilinear taps on texel corners.
// The exact mean is a multiple of 1/16; the 64th of a level added before rounding keeps a mean that ends on .5 on the upper
// side whatever the sampler's last bit does (the reference rounds such a mean up) and moves no other mean across a step.
vec4 fgLow(float px, float py) {
    const float ix = 1.0f / pc.a.x, iy = 1.0f / pc.a.y;
    const vec4 s = textureLod(uA, vec2((px - 1.5f) * ix, (py - 1.5f) * iy), 0.0f) + textureLod(uA, vec2((px + 0.5f) * ix, (py - 1.5f) * iy), 0.0f)
                 + textureLod(uA, vec2((px - 1.5f) * ix, (py + 0.5f) * iy), 0.0f) + textureLod(uA, vec2((px + 0.5f) * ix, (py + 0.5f) * iy), 0.0f);
    const float k = 255.0f * 0.25f, r = 0.5f + 0.015625f;
    return vec4((floor(s.x * k + r) + 0.25f) / 255.0f, (floor(s.y * k + r) + 0.25f) / 255.0f, (floor(s.z * k + r) + 0.25f) / 255.0f, 1.0f);
}

// ---- pass 2, GAIN, in two steps (framegen-cost, 2026-10-06: EXACT, the gain picture has the bytes it had).
// The current picture's brightness over the previous one's, from one texel per block (each a 4 x 4 mean), clamped to
// 0.8 .. 1.25, stored as 16 bits in R (high byte) and G (low byte).
// Until v467 ONE fragment added up all blocks of both pictures (2 x 160 x 90 = 28,800 texel reads in one loop on one GPU
// thread). Now the same numbers are added in two steps the GPU can run side by side:
//   2a GAIN COLUMNS (target: one texel per block column, two rows): row 0 holds, per column, the sum of the current
//      picture's samples of that column's blocks (90 reads per fragment), row 1 the same of the previous picture;
//   2b GAIN (target: 1x1): adds the 160 column sums of each row (320 reads) and forms the ratio as before.
// Why the bytes are the same: every sample is R + G + B of one texel, three integers 0..255 (fgTexel), so every partial
// sum in both forms is an integer. A column sum is at most 765 x blocks per column (68,850 for 720 lines), the whole sum
// at most 765 x blocks (11,016,000 for 1280x720); both are below 2^24 = 16,777,216, the largest range in which float32
// holds every integer, so each addition is exact and the order of the additions cannot change the result (the bound
// holds up to 21,931 blocks; the generator runs at 14,400 only). A column sum is written as three bytes (R = bits 16-23,
// G = bits 8-15, B = bits 0-7) into an RGBA8 texel, the format of every other picture of the generator, and read back
// with fgByte: exact. So sc and sp below are the two numbers the single loop gave, and the lines after the loop are
// unchanged. (tests: fg_gen_shader_test "GAIN, two steps"; the single loop is kept there, from v467's text.)
// uA = this frame's low-passed picture, uB = the previous one's.
vec4 fgGainColumns(float fragX, float fragY) {
    const int bx = int(fragX), bh = int(pc.a.y) / FG_BLOCK;
    float s = 0.0f;
    if(int(fragY) == 0) {
        for(int by = 0; by < bh; ++by) { const vec3 c = fgTexel(uA, bx * FG_BLOCK + 4, by * FG_BLOCK + 4); s += c.x + c.y + c.z; }
    } else {
        for(int by = 0; by < bh; ++by) { const vec3 p = fgTexel(uB, bx * FG_BLOCK + 4, by * FG_BLOCK + 4); s += p.x + p.y + p.z; }
    }
    const float hi = floor(s / 65536.0f);
    const float rest = s - hi * 65536.0f;
    const float mid = floor(rest / 256.0f);
    return vec4((hi + 0.25f) / 255.0f, (mid + 0.25f) / 255.0f, (rest - mid * 256.0f + 0.25f) / 255.0f, 1.0f);
}
// One column sum as the pass above stored it. uA = the column sums.
float fgColumnSum(int bx, int row) {
    const vec4 t = texelFetch(uA, ivec2(bx, row), 0);
    return fgByte(t.x) * 65536.0f + fgByte(t.y) * 256.0f + fgByte(t.z);
}
// uA = the column sums (row 0: the current picture, row 1: the previous one).
vec4 fgGainPass() {
    float sc = 0.0f, sp = 0.0f;
    const int bw = int(pc.a.x) / FG_BLOCK;
    for(int bx = 0; bx < bw; ++bx) { sc += fgColumnSum(bx, 0); sp += fgColumnSum(bx, 1); }
    const float g = sp < 1.0f ? 1.0f : clamp(sc / sp, 0.8f, 1.25f);
    const float n = floor((g - 0.75f) * 2.0f * 65535.0f + 0.5f);
    const float hi = floor(n / 256.0f);
    return vec4((hi + 0.25f) / 255.0f, (n - hi * 256.0f + 0.25f) / 255.0f, 0.0f, 1.0f);
}
float fgGain() {
    const vec4 t = texelFetch(uE, ivec2(0, 0), 0);
    return 0.75f + (fgByte(t.x) * 256.0f + fgByte(t.y)) * (0.5f / 65535.0f);
}

// ---- pass 3, CLASSIFY, in two steps (framegen-cost-b, 2026-10-06: EXACT, the block map has the bytes it had).
// Which explanation of "where was this block in the previous real frame" fits the two pictures: static world (the camera
// vector of each pixel), did not move on screen, or neither. Output: R = class, G = 1 while both fit (the neighbours decide in
// the resolve passes), B = the chosen cost x 4.
// Until v468 ONE fragment per block looped over the block's 64 pixels (per turn: a depth read, a camera vector, a four-read
// bilinear sample, two more reads) and added three numbers per pixel to three running sums. The phone measured that pass
// at 2.98 ms of a made picture's 3.64. Now the same numbers are made and added in two steps:
//   3a TERMS (target: one RGBA32F texel per PIXEL, no loop): what one turn of the loop computed for its pixel, as three
//      32-bit floats: R = the camera cost's three absolute differences added up, G = the same for "did not move",
//      B = the camera vector's |x| + |y|;
//   3b CLASSIFY (target: the block map): reads the 64 stored texels of its block and adds them to the three running sums
//      in the loop's order (rows from the top, in a row from the left: ONE chain per sum), then decides as before.
// Why the bytes are the same:
//   - a turn of the loop read nothing a turn before it wrote: its three numbers depend on the pixel alone. fgTerms below
//     is the loop's body, the same functions on the same arguments (tools/spirv_check.py compares the two modules'
//     instructions: check "terms");
//   - an RGBA32F texel holds a 32-bit float as it is (no blending, no conversion), and texelFetch gives it back;
//   - R and G are stored BEFORE fgAbsDiff's multiplication by a third, and 3b multiplies and adds with the loop's own
//     instruction: the loop's `cam += fgAbsDiff(..)` is compiled into ONE fused multiply-add instruction per turn
//     (Fma(sum of the three differences, 1/3, cam)), which a GPU may round once or twice. Had 3a stored the product, the
//     phone could round twice where the loop rounded once. As it is, both forms hand the same three numbers to the same
//     instruction in the same order, whatever the GPU does with it (check "chain");
//   - float addition is not associative: 3b adds in the loop's order and nothing else (64 texels, written out as 64
//     FG_TERM lines so that no fragment loops at all);
//   - from the division by 64 on, the text and the instructions are the loop's (check "tail").
// (tests: fg_gen_shader_test "CLASSIFY, two steps"; the loop is kept there, from v468's text.)
// 3a. uA = this frame's low-passed picture, uB = the previous one's, uD = depth, uE = the gain picture.
vec4 fgTerms(float fragX, float fragY) {
    const float g = fgGain();
    const int x = int(fragX), y = int(fragY);
    const vec3 c = fgTexel(uA, x, y);
    const float px = float(x) + 0.5f, py = float(y) + 0.5f;
    const float dn = fgNear(x, y); // fg-fixer (v630): the nearest depth of the window (was the pixel's own); stored in A for SYNTHESIS
    const vec2 v = fgCameraVector(px, py, dn);
    return vec4(fgAbsSum(c, fgBilinear(uB, px - v.x, py - v.y) * g), fgAbsSum(c, fgTexel(uB, x, y) * g), abs(v.x) + abs(v.y), dn);
}
// 3b. uA = the terms. One term into the three running sums, as a turn of the loop added it.
// fg-fixer (v630): FG_TERM also counts the block's pixels by what their own terms say (agree: the sum of the three differences
// at most 3 x DISAGREE): moved and static world (ns: "did not move" fails, the camera vector agrees), held on the screen (nz:
// the camera vector fails, "did not move" agrees), neither (nn). See fgPromote.
#define FG_TERM(i, j) { const vec4 t = texelFetch(uA, ivec2(x0 + i, y0 + j), 0); cam = fma(t.x, 1.0f / 3.0f, cam); zero = fma(t.y, 1.0f / 3.0f, zero); move += t.z; ns += t.y > agree && t.x <= agree ? 1.0f : 0.0f; nn += t.y > agree && t.x > agree ? 1.0f : 0.0f; nz += t.y <= agree && t.x > agree ? 1.0f : 0.0f; }
// fg-fixer (v630): a REFUSED block whose pixels say it is static world: at least 4 moved and agree with their camera vector,
// at most one eighth as many are held on the screen (the HUD, the player) and at most as many fit neither. The mean costs
// above are dominated by a few high-contrast pixels when a thin surface crosses a flat one (a frond against the sky moves
// by its own vector, and the frond's edge pixels are disoccluded every frame): the whole block was refused although nearly
// all of its pixels are static world. Such a block is static world (not open: the resolve rounds keep it). Only refused
// blocks are promoted: "did not move" and static-world blocks keep their class. (Evaluator, local-analysis/agents/fg-fixer:
// the captures' and fixtures' moving objects, HUD and player's car keep their bars; the checkered box of crossing_pan, a
// periodic texture whose squares agree with the camera vector by chance, loses 2 points.)
bool fgPromote(float ns, float nz, float nn) { return ns >= 4.0f && nz * 8.0f <= ns && nn <= ns; }
#define FG_TERM_ROW(j) FG_TERM(0, j) FG_TERM(1, j) FG_TERM(2, j) FG_TERM(3, j) FG_TERM(4, j) FG_TERM(5, j) FG_TERM(6, j) FG_TERM(7, j)
vec4 fgClassify(float fragX, float fragY) {
    const int x0 = int(fragX) * FG_BLOCK, y0 = int(fragY) * FG_BLOCK;
    float cam = 0.0f, zero = 0.0f, move = 0.0f, ns = 0.0f, nz = 0.0f, nn = 0.0f; const float agree = 3.0f * pc.a.w; // (fg-fixer: ns, nz, nn, agree)
    FG_TERM_ROW(0) FG_TERM_ROW(1) FG_TERM_ROW(2) FG_TERM_ROW(3) FG_TERM_ROW(4) FG_TERM_ROW(5) FG_TERM_ROW(6) FG_TERM_ROW(7)
    const float n = float(FG_BLOCK * FG_BLOCK);
    cam /= n; zero /= n; move /= n;
    const float fail = pc.b.x, ambiguous = pc.b.y;
    int mode = FG_NONE; float open = 0.0f, cost = min(cam, zero);
    if(move < 0.5f) { cost = cam; if(cam <= fail) mode = FG_CAMERA; }
    else if(cam <= fail && zero <= fail && abs(cam - zero) <= ambiguous) { mode = FG_CAMERA; open = 1.0f; cost = cam; }
    else if(cam <= zero && cam <= fail) { mode = FG_CAMERA; cost = cam; }
    else if(zero <= fail) { mode = FG_ZERO; cost = zero; }
    if(mode == FG_NONE && fgPromote(ns, nz, nn)) { mode = FG_CAMERA; cost = cam; } // fg-fixer (v630)
    return vec4((float(mode) + 0.25f) / 255.0f, open, (min(floor(cost * 4.0f + 0.5f), 255.0f) + 0.25f) / 255.0f, 1.0f);
}

// ---- fg-scaled (2026-10-10): pass 3 for a picture that is NOT a whole number of blocks (960x540 is 67.5 block rows).
// The block map has a block for every STARTED block: ceil(width / 8) x ceil(height / 8) (fgMode() below reads it for
// every pixel of the picture). A block that crosses the picture's right or bottom edge is judged from the pixels it has
// INSIDE the picture and from nothing else: their three sums in the usual order, divided by their number. That is the
// reference's rule (host-model/fg_reference.h classify: its loops stop at the picture's edge and divide by the pixels
// they counted), so the block classes stay the reference's at every size.
// How, in the two steps of pass 3:
//   3a' TERMS: the target is whole blocks (ceil(width / 8) * 8 x ceil(height / 8) * 8). A texel inside the picture is
//       fgTerms()'s. A texel outside it is not a pixel: it holds zero, and nothing is read for it;
//   3b' CLASSIFY: the block's 64 stored texels are added as in fgClassify (every one lies inside the terms' picture). A
//       zero texel changes none of the three sums (fma(0, 1/3, s) = s and s + 0 = s exactly, whether a GPU rounds the
//       fused instruction once or twice), so each sum is the sum over the block's pixels inside the picture, in the
//       loop's order (rows from the top, in a row from the left); n is the number of those pixels (never 0: the block
//       map has no block that starts outside the picture).
// A picture of whole blocks (1280x720, 640x360) is drawn with fgTerms and fgClassify, unchanged. For such a picture
// the two functions below give the same bytes (no texel is outside, n = 64): tests "edge == plain".
// Neither GAIN step changes: both count WHOLE blocks only (int(pc.a.y) / FG_BLOCK rows of int(pc.a.x) / FG_BLOCK
// columns), every sample (8 bx + 4, 8 by + 4) of a whole block lies inside the picture, and the bound of pass 2 holds
// (the pictures the generator takes have at most 14,400 blocks: fg_gen.h sizeOk).
vec4 fgTermsEdge(float fragX, float fragY) {
    if(fragX >= pc.a.x || fragY >= pc.a.y) return vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return fgTerms(fragX, fragY);
}
vec4 fgClassifyEdge(float fragX, float fragY) {
    const int x0 = int(fragX) * FG_BLOCK, y0 = int(fragY) * FG_BLOCK;
    float cam = 0.0f, zero = 0.0f, move = 0.0f, ns = 0.0f, nz = 0.0f, nn = 0.0f; const float agree = 3.0f * pc.a.w; // (fg-fixer: ns, nz, nn, agree)
    FG_TERM_ROW(0) FG_TERM_ROW(1) FG_TERM_ROW(2) FG_TERM_ROW(3) FG_TERM_ROW(4) FG_TERM_ROW(5) FG_TERM_ROW(6) FG_TERM_ROW(7)
    const float n = float(min(FG_BLOCK, int(pc.a.x) - x0) * min(FG_BLOCK, int(pc.a.y) - y0));
    cam /= n; zero /= n; move /= n;
    const float fail = pc.b.x, ambiguous = pc.b.y;
    int mode = FG_NONE; float open = 0.0f, cost = min(cam, zero);
    if(move < 0.5f) { cost = cam; if(cam <= fail) mode = FG_CAMERA; }
    else if(cam <= fail && zero <= fail && abs(cam - zero) <= ambiguous) { mode = FG_CAMERA; open = 1.0f; cost = cam; }
    else if(cam <= zero && cam <= fail) { mode = FG_CAMERA; cost = cam; }
    else if(zero <= fail) { mode = FG_ZERO; cost = zero; }
    if(mode == FG_NONE && fgPromote(ns, nz, nn)) { mode = FG_CAMERA; cost = cam; } // fg-fixer (v630)
    return vec4((float(mode) + 0.25f) / 255.0f, open, (min(floor(cost * 4.0f + 0.5f), 255.0f) + 0.25f) / 255.0f, 1.0f);
}

// ---- pass 4, RESOLVE (target: the other block map; run three times). uA = the block map.
// A block both explanations fit takes the majority of its decided neighbours; with none decided it stays open (and is
// static world when the rounds are over: its R is FG_CAMERA already).
vec4 fgResolve(float fragX, float fragY) {
    const ivec2 size = textureSize(uA, 0);
    const int bx = int(fragX), by = int(fragY);
    const vec4 t = texelFetch(uA, ivec2(bx, by), 0);
    if(t.y < 0.5f) return t;
    int cam = 0, zero = 0;
    for(int dy = -1; dy <= 1; ++dy) for(int dx = -1; dx <= 1; ++dx) {
        const int x = bx + dx, y = by + dy;
        if((dx == 0 && dy == 0) || x < 0 || y < 0 || x >= size.x || y >= size.y) continue;
        const vec4 q = texelFetch(uA, ivec2(x, y), 0);
        if(q.y >= 0.5f) continue;
        const int m = int(fgByte(q.x));
        if(m == FG_CAMERA) ++cam;
        if(m == FG_ZERO) ++zero;
    }
    if(cam + zero == 0) return t;
    return vec4((float(zero > cam ? FG_ZERO : FG_CAMERA) + 0.25f) / 255.0f, 0.0f, t.z, 1.0f);
}

// The class of the block under a continuous position (clamped to the picture). uE = the resolved block map.
int fgMode(float x, float y) {
    const int ix = int(clamp(floor(x), 0.0f, pc.a.x - 1.0f)), iy = int(clamp(floor(y), 0.0f, pc.a.y - 1.0f));
    return int(fgByte(texelFetch(uE, ivec2(ix / FG_BLOCK, iy / FG_BLOCK), 0).x));
}

// fg-fixer (v630): the depth SYNTHESIS uses at a continuous position: the near depth TERMS stored for that pixel (A of the
// terms' picture, which SYNTHESIS has bound as uD; the position clamped to the picture, as fgDepth clamps).
float fgNearAt(float x, float y) {
    const int ix = int(clamp(floor(x), 0.0f, pc.a.x - 1.0f)), iy = int(clamp(floor(y), 0.0f, pc.a.y - 1.0f));
    return texelFetch(uD, ivec2(ix, iy), 0).w;
}

// ---- pass 5, SYNTHESIS (target: the made picture). uA = the current real picture, uB = the previous real picture,
// uD = the terms' picture (fg-fixer, v630: its A is fgNear; until v629 uD was the depth), uE = the resolved block map.
// phase = pc.a.z: 0 = the previous real frame, 1 = the current one.
//   - a block that did not move, or that nothing explains: the current frame's pixel, untouched (nothing is mixed, so
//     nothing ghosts; the HUD and the player's car are exact);
//   - static world: the surface that is at p at the in-between time is at q in the current frame, q - (1 - phase) v(q) = p
//     (two fixed-point steps from p); it is read from both frames (previous at q - v, current at q) and mixed by phase.
//     Where the two disagree by more than DISAGREE the current frame's sample is used alone (an occlusion edge); where q has
//     left the picture the previous frame's alone (road running out under the edge). A source inside a block that is not
//     static world is refused: the current frame's pixel.
//   - fixer2 G4 (2026-10-21): a pixel of a REFUSED block ("neither") whose own surface is static world - the previous
//     frame, read where the camera vector of the pixel's own depth says the surface was, agrees with the pixel within
//     DISAGREE - is drawn as static world by the rule above (its source must lie in a static-world block, as for every
//     pixel). Until v572 the whole refused block kept the current frame's pixels, unmoved, while its static-world
//     neighbours moved by (1 - phase) v: under a moving camera every refused block on a static edge showed as an 8 x 8
//     step (the Thor's blocky truck edges, G4). A pixel that is not static world itself (a moving object, or a surface the
//     previous frame did not show) keeps the current frame's pixel, as before; static-world and "did not move" blocks are
//     drawn exactly as before.
//   - fg-fixer (2026-10-23, v630): every depth below is the nearest one around the pixel (fgNearAt). Searching the source
//     of a pixel next to a thin surface with the far side's depth jumped over the thin surface: a pole drawn twice, a lamp
//     arm cut, a frond missing at its in-between place. With the nearest depth the search lands on the thin surface.
vec4 fgSynth(float px, float py) {
    const vec3 r = fgTexel(uA, int(px), int(py));
    const int m = fgMode(px, py);
    vec3 o = r;
    bool own = m == FG_CAMERA;
    if(m == FG_NONE) {
        const vec2 w = fgCameraVector(px, py, fgNearAt(px, py));
        own = fgAbsDiff(fgBilinear(uB, px - w.x, py - w.y), r) <= pc.a.w;
    }
    if(own) {
        const float phase = pc.a.z, back = 1.0f - pc.a.z;
        float qx = px, qy = py;
        vec2 v = vec2(0.0f, 0.0f);
        for(int k = 0; k <= 2; ++k) {
            v = fgCameraVector(qx, qy, fgNearAt(qx, qy));
            if(k < 2) { qx = px + back * v.x; qy = py + back * v.y; }
        }
        if(fgMode(qx, qy) == FG_CAMERA) {
            const vec3 a = fgBilinear(uB, qx - v.x, qy - v.y);
            if(qx < 0.0f || qy < 0.0f || qx >= pc.a.x || qy >= pc.a.y) o = a;
            else {
                const vec3 b = fgBilinear(uA, qx, qy);
                if(fgAbsDiff(a, b) > pc.a.w) o = b;
                else o = a * back + b * phase;
            }
        }
    }
    if(pc.b.z > 0.5f && m != FG_CAMERA) { // diag-fg-show: what the classifier refused (red) or held in place (blue)
        const vec3 tint = m == FG_ZERO ? vec3(40.0f, 80.0f, 255.0f) : vec3(255.0f, 40.0f, 40.0f);
        o = o * 0.5f + tint * 0.5f;
    }
    return vec4((floor(clamp(o.x + 0.5f, 0.0f, 255.0f)) + 0.25f) / 255.0f, (floor(clamp(o.y + 0.5f, 0.0f, 255.0f)) + 0.25f) / 255.0f,
                (floor(clamp(o.z + 0.5f, 0.0f, 255.0f)) + 0.25f) / 255.0f, 1.0f);
}
