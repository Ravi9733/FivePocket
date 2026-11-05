"""Explicit post-filter Xenos PWL conversion, not hardware sRGB.

Pinned ReXGlue/Xenia spirv_translator_fetch.cpp converts the filtered, swizzled
sample. Do not gamma-decode texels before filtering. This matches that reference
implementation, not a measured Xbox filtering/rounding oracle.
"""
PWL_TEXTURE_GLSL='''
float xenosPwlSample(float gamma) {
    gamma=isnan(gamma)?0.0:clamp(gamma,0.0,1.0);
    precise float scale=gamma>=192.0/255.0?8.0/1024.0:
        gamma>=96.0/255.0?4.0/1024.0:gamma>=64.0/255.0?2.0/1024.0:1.0/1024.0;
    precise float offset=gamma>=192.0/255.0?-1024.0:
        gamma>=96.0/255.0?-256.0:gamma>=64.0/255.0?-64.0:0.0;
    precise float linear=((gamma*(255.0*1024.0))*scale)+offset;
    linear=linear+trunc(linear*scale);
    return linear*(1.0/1023.0);
}
'''
