"""Translate owned straight-line UI shader code to a variable-size draw ABI.

No game inputs or pre-rendered pixels are embedded. ABI: 16-byte BE header,
256 BE vertex float4s, 256 BE pixel float4s, then the actual vertex bytes.
Binding0 is that SSBO, bindings1/2 the actual swizzled textures. Native state and
resource ownership are checked by the draw consumer, not inferred here.
"""
from vertex_arithmetic_dataflow import analyze_vertex_dataflow
from pixel_sample_dataflow import analyze_pixel_sample_dataflow
from raw_shader_analysis import RawShaderError


COMMON = '''#version 450
layout(set=0,binding=0,std430) readonly buffer Inputs { uint words[]; } source;
uint readBE(uint at) {
    uint v=source.words[at];
    return (v<<24)|((v&0xff00u)<<8)|((v>>8)&0xff00u)|(v>>24);
}
'''


def translate_inline_pair(vertex_code, pixel_code, *, indexed=False):
    vs=analyze_vertex_dataflow(vertex_code,dot4=True)
    ps=analyze_pixel_sample_dataflow(pixel_code,ui_operations=True)
    textured=bool(ps['fetches'])
    expected=set(vs['exports'])
    valid_layouts=({'0','1','62'},{'0','1','2','62'},{'0','1','2','3','62'}) if textured else ({'0','62'},)
    components=lambda key:2 if textured and (key=='0' or (len(expected)==5 and key=='1')) else 4
    if (expected not in valid_layouts or
        any(n is None for key in expected for n in vs['exports'][key][:components(key)]) or
        set(ps['exports'])!={'0'} or None in ps['exports']['0'] or len(ps['fetches'])>2):
        raise RawShaderError('complete supported interpolators, position and color output required')
    if ((len(expected)==5)!=(len(ps['fetches'])==2) or
        len({f['slot'] for f in ps['fetches']})!=len(ps['fetches'])):
        raise RawShaderError('two independent texture slots require the four-interpolator layout')
    fetches=vs['fetches']
    if (not fetches or len({f['stride_words'] for f in fetches})!=1 or
        any(f['fetch_slot']!=95 or vs['nodes'][f['index_expression']]!=('initial',0,0) for f in fetches)):
        raise RawShaderError('one inline auto-indexed vertex stream required')
    stride=fetches[0]['stride_words']
    for f in fetches:
        if f['format'] not in (6,25,37,38) or f['offset_words']+(1 if f['format'] in (6,25) else f['components'])>stride:
            raise RawShaderError('unsupported inline vertex format/extent')
    fetch=ps['fetches'][0] if textured else None
    sample_coordinates=[]
    for sample in ps['fetches']:
        coordinates=[ps['nodes'][i] for i in sample['coordinates']]
        if (len(coordinates)!=2 or any(n[0]!='initial' for n in coordinates) or
            coordinates[0][1]!=coordinates[1][1] or [n[2] for n in coordinates]!=[0,1] or
            str(coordinates[0][1]) not in expected or coordinates[0][1]>=4):
            raise RawShaderError('unchanged provided interpolated UV required')
        sample_coordinates.append(coordinates[0][1])

    def body(proof, vertex):
        nodes=proof['nodes'];statements=[];done={}
        if vertex:
            for i,f in enumerate(fetches):
                at=f"2052u+index*{stride}u+{f['offset_words']}u"
                if f['format'] in (6,25):statements.append(f'uint packed{i}=readBE({at});')
                for lane in range(f['components']):
                    value=(f'float((packed{i}>>{lane*8}u)&255u)*(1.0/255.0)' if f['format']==6 else
                           f'float(bitfieldExtract(int(packed{i}),{lane*16},16))' if f['format']==25 else
                           f'uintBitsToFloat(readBE({at}+{lane}u))')
                    statements.append(f'precise float fetch{i}_{lane}={value};')
        elif textured:
            for i,coordinate in enumerate(sample_coordinates):
                statements.append(f'vec4 sampled{i}=texture(atlas{i},interpolator{coordinate}.xy);')
        def expression(i):
            if i in done:return done[i]
            n=nodes[i];op=n[0]
            if op=='literal':value=f'{n[1]}.0'
            elif op=='initial':
                if vertex and n[1:]==(0,0):value='float(index)'
                elif not vertex and str(n[1]) in expected and n[1]<4 and n[2]<components(str(n[1])):
                    value=f'interpolator{n[1]}[{n[2]}]'
                else:raise RawShaderError('unprovided stage input')
            elif op=='fetch':value=f'fetch{n[1]}_{n[2]}'
            elif op=='sample':
                if vertex or n[1]>=len(sample_coordinates):raise RawShaderError('unexpected texture sample')
                value=f'sampled{n[1]}[{n[2]}]'
            elif op=='constant':value=f'uintBitsToFloat(readBE({(4 if vertex else 1028)+n[1]*4+n[2]}u))'
            elif op in ('mul','add','regular_copy','select_eq0','not_equal','saturate','negate'):
                args=[expression(j) for j in n[1:]]
                if op=='mul':rhs=f'(({args[0]}==0.0 || {args[1]}==0.0)?0.0:({args[0]}*{args[1]}))'
                elif op=='add':rhs=f'({args[0]}+{args[1]})'
                elif op=='regular_copy':rhs=args[0]
                elif op=='select_eq0':rhs=f'({args[0]}==0.0?{args[1]}:{args[2]})'
                elif op=='not_equal':rhs=f'({args[0]}!={args[1]}?1.0:0.0)'
                elif op=='negate':rhs=f'(-{args[0]})'
                else:rhs=f'clamp({args[0]},0.0,1.0)'
                value=f'n{i}';statements.append(f'precise float {value}={rhs};')
            else:raise RawShaderError('unsupported translated expression')
            done[i]=value;return value
        outputs=([('62','gl_Position',4)]+[(s,'interpolator'+s,components(s)) for s in sorted(expected-{'62'})]) if vertex else [('0','color',4)]
        for slot,target,count in outputs:
            values=[expression(n) for n in proof['exports'][slot][:count]]
            statements.append(f'{target}=vec{count}({",".join(values)});')
        if vertex:
            # Host supplies the actual signed viewport and pixel-center
            # mapping into the padded native viewport. Preserve non-unit W.
            statements.extend([
                'gl_Position.xy=gl_Position.xy*viewport.scaleOffset.xy+viewport.scaleOffset.zw*gl_Position.w;'])
        return '\n'.join('    '+s for s in statements)

    varyings=lambda direction:'\n'.join(f'layout(location={s}) {direction} vec{components(s)} interpolator{s};' for s in sorted(expected-{'62'}))+'\n'
    indexing='    uint index=uint(gl_VertexIndex);\n' if indexed else '''
    const uint quad[6]=uint[6](0u,1u,2u,0u,2u,3u);
    uint index=(uint(gl_VertexIndex)/6u)*4u+quad[uint(gl_VertexIndex)%6u];
'''
    vertex=COMMON+'layout(push_constant) uniform Viewport { vec4 scaleOffset; } viewport;\n'+varyings('out')+'void main() {\n'+indexing+body(vs,True)+'\n}\n'
    samplers=''.join(f'layout(set=0,binding={i+1}) uniform sampler2D atlas{i};\n' for i in range(len(sample_coordinates)))
    pixel=COMMON+samplers+varyings('in')+'layout(location=0) out vec4 color;\nvoid main() {\n'+body(ps,False)+'\n}\n'
    return vertex,pixel,dict(vertex=vs,pixel=ps,stride=stride*4,texture_slot=fetch['slot'] if fetch else None,
        texture_slots=[f['slot'] for f in ps['fetches']],indexed=indexed,interpolators=len(expected)-1,abi_version=4)
