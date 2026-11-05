"""Explicit face-local base-level point/bilinear sampling policy.

The Thor's hardware filter differed by >half a UNORM8 step on authored
high-contrast arrays. Use texelFetch and precise float operations so tests can
separate coordinate/filter policy from device fixed-point filtering. This is
not proof of Xbox subtexel precision. The native consumer must validate clamp2,
base-only storage, equal min/mag mode and all six owned faces.
"""
CUBE_SAMPLE_GLSL='''
vec4 sampleCubeBase(sampler2DArray sourceCube,vec3 coordinates,bool linearFilter) {
    ivec3 size=textureSize(sourceCube,0);
    int face=int(floor(coordinates.z+0.5));
    precise vec2 pixel=coordinates.xy*vec2(size.xy);
    if(!linearFilter) return texelFetch(sourceCube,ivec3(clamp(ivec2(floor(pixel)),ivec2(0),size.xy-1),face),0);
    precise vec2 centered=pixel-vec2(0.5);
    ivec2 base=ivec2(floor(centered));
    precise vec2 fraction=centered-vec2(base);
    vec4 a=texelFetch(sourceCube,ivec3(clamp(base,ivec2(0),size.xy-1),face),0);
    vec4 b=texelFetch(sourceCube,ivec3(clamp(base+ivec2(1,0),ivec2(0),size.xy-1),face),0);
    vec4 c=texelFetch(sourceCube,ivec3(clamp(base+ivec2(0,1),ivec2(0),size.xy-1),face),0);
    vec4 d=texelFetch(sourceCube,ivec3(clamp(base+ivec2(1,1),ivec2(0),size.xy-1),face),0);
    precise vec4 top=a*(1.0-fraction.x)+b*fraction.x;
    precise vec4 bottom=c*(1.0-fraction.x)+d*fraction.x;
    precise vec4 result=top*(1.0-fraction.y)+bottom*fraction.y;
    return result;
}
'''
