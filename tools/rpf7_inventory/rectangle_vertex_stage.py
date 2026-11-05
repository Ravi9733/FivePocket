"""Bounded float-copy vertex + explicit axis-aligned rectangle test adapter.

Not a general Xenos rectangle/viewport/depth model. Three source vertices define
TL,TR,BR; native triangles use TL,TR,BR / TL,BR,BL. All six native invocations
validate all three source vertices before publishing any in-clip position.
"""
import math
import re
import struct
from raw_shader_analysis import (RawShaderError,analyze_copy_shader,
    translate_float_vertex_copy_compute,evaluate_float_vertex_copy)
from unorm8_vertex_fetch import f32


def _extent(vertex_bytes,width,height):
    if type(vertex_bytes) is not int or not 0<vertex_bytes<=4096 or vertex_bytes%4:
        raise RawShaderError('bounded aligned vertex bytes required')
    if any(type(v) is not int or not 1<=v<=4096 for v in (width,height)):
        raise RawShaderError('explicit bounded test width/height required')


def evaluate_rectangle(code,source,width,height):
    _extent(len(source),width,height)
    _,proof=translate_float_vertex_copy_compute(code)
    # Validate the original integer encoding, before any host/GPU float
    # conversion can flush a subnormal to zero. This adapter does not promise
    # subnormal arithmetic or rasterization on either backend.
    for index in range(3):
        for fetch in proof['vertex_fetches']:
            at=(index*fetch['stride_words']+fetch['offset_words'])*4
            if at+fetch['components']*4>len(source):raise RawShaderError('rectangle fetch outside source')
            for lane in range(fetch['components']):
                bits=struct.unpack_from('>I',source,at+lane*4)[0];exponent=bits&0x7F800000
                if exponent==0x7F800000 or (exponent==0 and bits&0x007FFFFF):
                    raise RawShaderError('nonfinite or subnormal raw rectangle fetch unsupported')
    exports=[evaluate_float_vertex_copy(code,{0:source},i) for i in range(3)]
    positions=[e['62'] for e in exports]
    colors=[e['0'] if '0' in e else [e['62'][2]]*3+[1] for e in exports]
    a,b,c=positions
    if (any(p[3]!=1 or not 0<=p[2]<=1 or abs(p[0])>16384 or abs(p[1])>16384 for p in positions) or
        a[1]!=b[1] or b[0]!=c[0] or not a[0]<b[0] or not b[1]<c[1] or a[2]!=b[2] or b[2]!=c[2] or
        colors[0]!=colors[1] or colors[1]!=colors[2] or
        any(not math.isfinite(v) or abs(v)>16 or (v!=0 and abs(v)<2**-126) for v in colors[0])):
        raise RawShaderError('explicit TL/TR/BR rectangle, shared depth/W1 and bounded uniform color required')
    positions.append([a[0],c[1],a[2],1])
    def mapped(v,size):return f32(f32(f32(f32(v+.5)/size)*2)-1)
    return {'source_exports':exports,'clip_corners':[[mapped(p[0],width),mapped(p[1],height),p[2],1] for p in positions],
        'uniform_color':colors[0],'source_bounds':[a[0],a[1],c[0],c[1]],
        'source_color_export_present':'0' in proof['exports'],'title_raster_state_replayed':False,
        'raw_float_bits_guarded_before_decode':True,'subnormal_fetch_supported':False}


def translate_rectangle_vertex(code,vertex_bytes,width,height):
    _extent(vertex_bytes,width,height)
    _,proof=translate_float_vertex_copy_compute(code)  # Existing strict copy/fetch profile, unchanged.
    statements=[]
    for i,fetch in enumerate(proof['vertex_fetches']):
        start=f"(index*{fetch['stride_words']}u+{fetch['offset_words']}u)"
        statements.append(f"if ({start}+{fetch['components']}u>{vertex_bytes//4}u) return false;")
        for lane in range(fetch['components']):
            name=f"fetch{i}_{'xyzw'[lane]}"
            statements.extend((f'uint {name}_bits=readBE({start}+{lane}u);',
                f'uint {name}_exponent={name}_bits&0x7f800000u;',
                f'if ({name}_exponent==0x7f800000u || ({name}_exponent==0u && ({name}_bits&0x007fffffu)!=0u)) return false;',
                f'float {name}=uintBitsToFloat({name}_bits);'))
    def expression(value):
        if value in ('0','1'):return value+'.0'
        match=re.fullmatch(r'fetch([0-9]+)\.([xyzw])',value)
        if not match:raise RawShaderError('unresolved rectangle export')
        return f'fetch{match[1]}_{match[2]}'
    for slot,name in (('62','position'),('0','color')):
        if slot in proof['exports']:
            statements.append(f'{name}=vec4('+','.join(expression(v) for v in proof['exports'][slot])+');')
    if '0' not in proof['exports']:statements.append('color=vec4(position.zzz,1);')
    source='''#version 450
layout(location=0) out vec4 parameter0;
layout(set=0,binding=0,std430) readonly buffer Source { uint words[]; } source;
uint readBE(uint at) {
    uint v=source.words[at];
    return (v<<24)|((v&0xff00u)<<8)|((v>>8)&0xff00u)|(v>>24);
}
bool readVertex(uint index,out vec4 position,out vec4 color) {
'''+''.join('    '+s+'\n' for s in statements)+'''    return true;
}
bool validPosition(vec4 p) {
    return p.w==1.0 && p.z>=0.0 && p.z<=1.0 && abs(p.x)<=16384.0 && abs(p.y)<=16384.0;
}
bool regular(float v) {
    uint bits=floatBitsToUint(v)&0x7fffffffu;
    return bits==0u || (bits>=0x00800000u && bits<0x7f800000u);
}
void main() {
    gl_Position=vec4(0,0,2,1); parameter0=vec4(0);
'''+f'    if(gl_VertexIndex<0 || gl_VertexIndex>=6 || source.words.length()<{vertex_bytes//4}) return;\n'+'''
    vec4 a,b,c,ca,cb,cc;
    if(!readVertex(0u,a,ca) || !readVertex(1u,b,cb) || !readVertex(2u,c,cc)) return;
    if(!validPosition(a) || !validPosition(b) || !validPosition(c)) return;
    if(a.y!=b.y || b.x!=c.x || a.x>=b.x || b.y>=c.y || a.z!=b.z || b.z!=c.z) return;
    if(any(notEqual(ca,cb)) || any(notEqual(cb,cc)) || any(greaterThan(abs(ca),vec4(16)))) return;
    if(!regular(ca.x) || !regular(ca.y) || !regular(ca.z) || !regular(ca.w)) return;
    const uint corners[6]=uint[6](0u,1u,2u,0u,2u,3u);
    vec4 p=vec4(a.x,c.y,a.z,1);
    uint corner=corners[gl_VertexIndex];
    if(corner==0u) p=a; else if(corner==1u) p=b; else if(corner==2u) p=c;
    precise vec2 shifted=p.xy+vec2(0.5);
'''+f'    precise vec2 normalized=shifted/vec2({width}.0,{height}.0);\n'+'''
    precise vec2 doubled=normalized*2.0;
    precise vec2 clip=doubled-1.0;
    gl_Position=vec4(clip,p.z,1);parameter0=ca;
}
'''
    return source,dict(proof,probe_stage='vertex',input_bytes=vertex_bytes,explicit_test_extent=[width,height],
        expansion='TL-TR-BR/TL-BR-BL',native_vertices=6,guest_source_vertices=3,
        pixel_center_offset_policy=0.5,title_raster_state_replayed=False,
        color_source='captured-parameter0' if '0' in proof['exports'] else 'synthetic-depth-grayscale',
        all_source_vertices_checked_per_invocation=True,whole_primitive_rejection_guaranteed=True,
        raw_float_bits_guarded_before_decode=True,subnormal_fetch_supported=False,
        native_vertex_pipeline=False,game_frame=False,guest_writeback=False)


def passthrough_fragment(pixel_code=None):
    if pixel_code is not None:
        proof=analyze_copy_shader(pixel_code,'pixel')
        if proof['exports']!={'0':['initial_r0.'+c for c in 'xyzw']}:
            raise RawShaderError('exact pass-through pixel copy required')
    return ('#version 450\nlayout(location=0) in vec4 parameter0;\n'
        'layout(location=0) out vec4 color0;\nvoid main(){color0=parameter0;}\n')
