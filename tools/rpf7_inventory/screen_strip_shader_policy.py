"""Explicit single-sample depth representation, not Xbox raster equivalence.

ABI28's original PS exports colour only. The native D32 attachment requires the
existing Xenia-compatible20e4 truncation policy BEFORE the GREATER depth test.
RGB storage/blend conversion is a separate native target responsibility.
"""
import math
from geometry_output_merger import GEOMETRY_DEPTH_ENCODING_GLSL
from raw_shader_analysis import RawShaderError


def screen_strip_zero_reciprocal(value):
    """Only the exceptional +/-0 case. Other domains stay with strict math."""
    if value != 0 or not math.isfinite(value):
        raise RawShaderError('only signed zero reciprocal is handled here')
    return math.copysign(math.inf, value)


def screen_strip_zero_infinity_product(left, right):
    # Xenia ucode.h: SM3 +/-0 * anything = +0. Limit this new reference
    # exception to an actual infinity and zero; do not admit NaN/subnormals.
    if not ((left == 0 and math.isinf(right)) or (right == 0 and math.isinf(left))):
        raise RawShaderError('only zero times infinity is handled here')
    return 0.0


SCREEN_STRIP_RECIPROCAL_GLSL = '''
// Original scalar19 is RECIP_IEEE, NOT scalar17 clamp-max or18 clamp-zero.
// Preserve signed infinity at zero, then SM3 multiply/select semantics decide
// its use. Never globally replace reciprocal(0) with a finite fallback.
float screenStripReciprocal(float x) {
    return x==0.0?uintBitsToFloat((floatBitsToUint(x)&0x80000000u)|0x7F800000u):1.0/x;
}
'''

SCREEN_STRIP_DEPTH_GLSL = GEOMETRY_DEPTH_ENCODING_GLSL + '''
void screenStripStoreDepth() {
    // The native consumer must prove finite W1 original positions and the
    // pinned reversed depth viewport. Single-sample center evaluation only.
    gl_FragDepth=geometryExpand20e4(geometryDepth20e4(floatBitsToUint(gl_FragCoord.z)));
}
'''

SCREEN_STRIP_BLEND_GLSL = '''
vec4 screenStripPrepareBlend(vec4 value) {
    // Reference AddPSIColorFormatFlags: format3/12 has FLOAT RGB, fixed alpha.
    // Clamp alpha before it contributes ONE_MINUS_SRC_ALPHA; RGB clamps only
    // on final7e3 storage. Do not silently accept nonfinite masked alpha.
    if(any(isnan(value)) || any(isinf(value)))
        return vec4(uintBitsToFloat(0x7FC00000u));
    return vec4(value.rgb,clamp(value.a,0.0,1.0));
}
'''
