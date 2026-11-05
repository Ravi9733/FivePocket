"""ABI33: bounded ordered-fragment producer, separate from legacy native ABIs.

Original geometry-LOD expressions remain unchanged. This emits fragments, not
target writes or draw/query completion. Native consumers owe current shader and
resource identities, original finite positions, raster state, fences, overflow
checks and ordered physical output-merger execution.
"""
from inline_shader_translation import translate_inline_pair
from geometry_output_merger import GEOMETRY_DEPTH_ENCODING_GLSL
from raw_shader_analysis import RawShaderError

ORDERED_FRAGMENT_PREFIX = '''layout(location=16) noperspective in float mergerNdc;
layout(set=0,binding=6,std430) buffer PixelTrace { uint words[]; } pixelTrace;
layout(push_constant) uniform PrimitivePlane { layout(offset=16) uint plane; layout(offset=20) uint capacity; } primitivePlane;
'''+GEOMETRY_DEPTH_ENCODING_GLSL

ORDERED_FRAGMENT_SUFFIX = '''
    if(gl_HelperInvocation)return;
    if(any(isnan(color)) || any(isinf(color)) || isnan(mergerNdc) || isinf(mergerNdc)) {
        atomicOr(pixelTrace.words[5],1u);return;
    }
    precise float sampleDepth=1.0-clamp(mergerNdc,0.0,1.0);
    if(sampleDepth<0.0 || sampleDepth>1.0) {atomicOr(pixelTrace.words[5],2u);return;}
    uvec2 xy=uvec2(gl_FragCoord.xy);
    if(xy.x>=1280u || xy.y>=720u || primitivePlane.plane>=4u) {atomicOr(pixelTrace.words[5],4u);return;}
    uint record=atomicAdd(pixelTrace.words[4],1u);
    if(record>=primitivePlane.capacity || record>=65536u) {atomicOr(pixelTrace.words[5],8u);return;}
    uint base=8u+record*10u;
    pixelTrace.words[base]=primitivePlane.plane;
    pixelTrace.words[base+1u]=xy.y*1280u+xy.x;
    for(uint c=0u;c<4u;++c)pixelTrace.words[base+2u+c]=floatBitsToUint(color[c]);
    pixelTrace.words[base+6u]=floatBitsToUint(sampleDepth);
    pixelTrace.words[base+7u]=geometryDepth20e4(floatBitsToUint(sampleDepth==0.0?0.0:sampleDepth));
    pixelTrace.words[base+8u]=floatBitsToUint(gl_FragCoord.z);
}
'''

def translate_ordered_fragment_pair(vertex_code,pixel_code):
    vertex,pixel,proof=translate_inline_pair(vertex_code,pixel_code,indexed=True,
        extended_profile=True,loading_profile=True,geometry_profile=True,
        vertex_predicate_profile=True,geometry_lod_profile=True)
    if proof['abi_version']!=32 or len(proof['texture_slots'])>5 or \
            any(int(s)>=16 for s in proof['geometry_interpolator_masks']):
        raise RawShaderError('ordered fragment texture/interpolator layout exceeds fixed contract')
    def replace(text,old,new):
        if text.count(old)!=1:raise RawShaderError('ordered fragment producer template changed')
        return text.replace(old,new,1)
    vertex=replace(vertex,'void main() {',
        'layout(location=16) noperspective out float mergerNdc;\nvoid main() {')
    vertex=replace(vertex,'    gl_Position.xy=',
        '    mergerNdc=gl_Position.z/gl_Position.w;\n    gl_Position.xy=')
    pixel=replace(pixel,'void main() {',ORDERED_FRAGMENT_PREFIX+'void main() {')
    if not pixel.rstrip().endswith('}') or 'discard' in pixel:
        raise RawShaderError('ordered fragment requires its complete no-discard color0 shader')
    pixel=pixel.rstrip()[:-1]+ORDERED_FRAGMENT_SUFFIX
    return vertex,pixel,{**proof,'abi_version':33,'ordered_fragment_stream_version':1,
        'ordered_fragment_completion_binding':6,'ordered_fragment_capacity':65536,
        'ordered_fragment_words':10,'ordered_fragment_header_words':8,
        'ordered_fragment_primitive_limit':4,'ordered_fragment_target_extent':[1280,720],
        'ordered_fragment_position_check_required':True,
        'native_draw_admission':False,'query_completion':False}
