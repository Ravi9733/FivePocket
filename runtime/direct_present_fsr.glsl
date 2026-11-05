// agent fsr1 (v450 drop-in): AMD FidelityFX Super Resolution 1 (EASU + RCAS, ffx_fsr1.h, MIT; third_party/fsr1, unmodified)
// as the presenter's output scaler. This file is our integration code only: AMD's input callbacks and the per-pixel calls,
// shared verbatim by direct_present_fsr_easu.frag / direct_present_fsr_rcas.frag (GLSL 4.50, after ffx_a.h and ffx_fsr1.h with
// A_GPU + A_GLSL) and by the host model (tools/fsr1/fsr1_shim_model.h: C++ with tools/fsr1/glsl_shim.h), so the host runs this
// text. Only syntax valid in both languages (no parameter qualifiers except through DIRECT_FSR_INOUT).
// The including shader declares: `source` (binding 0), the push block `pc` {ivec4 rect; uvec4 con0..con3} and defines
// DIRECT_FSR_EASU or DIRECT_FSR_RCAS.
//  - EASU reads the map pass's 1280x720 picture (the game's scan-out tables already applied: display-encoded values, which is
//    the perceptual space EASU asks for) with three gathers per tap block (r, g, b) through the presenter's NEAREST +
//    CLAMP_TO_EDGE sampler (gathers ignore the filter; the edge repeats as the cubic's mappedTexel clamp). pc.con0..con3 are
//    FsrEasuCon(1280, 720, 1280, 720, image width, image height) from the CPU (fsr1_cpu.h).
//  - RCAS reads EASU's picture (image-rect sized RGBA8) with texelFetch at clamped coordinates (the edge repeats; AMD's sample
//    reads past the edge as zero through robust image loads, we keep the edge instead). pc.con0 = FsrRcasCon(stops).
#ifdef __cplusplus
#define DIRECT_FSR_INOUT(T) T&
#else
#define DIRECT_FSR_INOUT(T) inout T
#endif
#ifdef DIRECT_FSR_EASU
AF4 FsrEasuRF(AF2 p) { return textureGather(source, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(source, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(source, p, 2); }
// q = the output pixel relative to the image rect (0 .. size - 1); the result in [0, 1].
AF3 directFsrEasu(ASU2 q) {
    AF3 c;
    FsrEasuF(c, AU2(q), pc.con0, pc.con1, pc.con2, pc.con3);
    return c;
}
#endif
#ifdef DIRECT_FSR_RCAS
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(source, clamp(p, ASU2(0, 0), ASU2(pc.rect.z - 1, pc.rect.w - 1)), 0); }
void FsrRcasInputF(DIRECT_FSR_INOUT(AF1) r, DIRECT_FSR_INOUT(AF1) g, DIRECT_FSR_INOUT(AF1) b) {} // (no input conversion)
AF3 directFsrRcas(ASU2 q) {
    AF1 r;
    AF1 g;
    AF1 b;
    FsrRcasF(r, g, b, AU2(q), pc.con0);
    return AF3(r, g, b);
}
#endif
