"""Bounded original rectangle VS for the separate packed10 weight profile.

Do not replace actual vertices with a fullscreen triangle. Execute the original
dataflow, including unused RECIP_IEEE(0), then synthesize the fourth corner from
the original outputs using the existing Xenia rectangle policy. Only proven
PS-consumed XY crosses the stage boundary; an unused infinity is not zeroed.
"""
from raw_shader_analysis import RawShaderError
from vertex_arithmetic_dataflow import analyze_vertex_dataflow
from pixel_sample_dataflow import analyze_pixel_sample_dataflow
from inline_shader_translation import COMMON


def translate_filter_weights_vertex(vertex_code,pixel_code,vertex_fetch):
    if tuple(vertex_fetch)!=(0x02000002,2,0,0x01001249,0,0x200):
        raise RawShaderError('exact literal-zero vertex descriptor required; no address-zero read')
    v=analyze_vertex_dataflow(vertex_code,dot4=True,scalar_constants=True,loading_math=True,
        scalar_gt_zero=True,base_texture_profile=True)
    p=analyze_pixel_sample_dataflow(pixel_code,ui_operations=True,extended_math=True,
        loading_math=True,filter_weights_profile=True)
    # Include all executed arithmetic and fetch coordinates, not just exports.
    initial=set();seen=set();leaf={'initial','constant','literal','sample','gradient'}
    def visit(i):
        if i is None or i in seen:return
        seen.add(i);n=p['nodes'][i]
        if n[0]=='initial':initial.add(tuple(n[1:]));return
        if n[0] in leaf:return
        for child in n[1:]:visit(child)
    for i,n in enumerate(p['nodes']):
        if n[0] not in leaf:visit(i)
    for f in p['fetches']:
        for i in f['coordinates']:visit(i)
    for lanes in p['exports'].values():
        for i in lanes:visit(i)
    nodes=v['nodes'];fetches=v['fetches'];textures=v['texture_fetches']
    if (initial!={(0,0),(0,1)} or set(v['exports'])!={'0','62'} or
            any(i is None for row in v['exports'].values() for i in row) or
            len(textures)!=1 or textures[0]['slot']!=17 or len(fetches)!=2 or
            any(f['fetch_slot']!=95 or f['stride_words']!=6 or nodes[f['index_expression']]!=('initial',0,0) for f in fetches) or
            [(f['format'],f['components'],f['offset_words']) for f in fetches]!=[(57,3,0),(37,2,4)]):
        raise RawShaderError('unsupported rectangle stage or live ZW dependency')
    def copied(i):
        n=nodes[i]
        while n[0]=='maximum' and n[1]==n[2]:
            n=nodes[n[1]]
        return tuple(n)
    if ([copied(i) for i in v['exports']['62']]!=[('fetch',0,0),('fetch',0,1),('fetch',0,2),('literal',1)] or
            [copied(i) for i in v['exports']['0'][:2]]!=[('fetch',1,0),('fetch',1,1)]):
        raise RawShaderError('coverage contract requires unmodified original position and UV fetches')
    lines=[];done={}
    for i,f in enumerate(fetches):
        for c in range(f['components']):
            lines.append(f'precise float fetch{i}_{c}=uintBitsToFloat(readBE(2052u+index*6u+{f["offset_words"]+c}u));')
            lines.append(f'if(isnan(fetch{i}_{c}) || isinf(fetch{i}_{c})) return;')
    def expr(i):
        if i in done:return done[i]
        n=nodes[i];op=n[0]
        if op=='literal':value=f'{float(n[1])}'
        elif op=='fetch':value=f'fetch{n[1]}_{n[2]}'
        elif op=='texture_sample':
            if n[1]!=0:raise RawShaderError('unknown vertex texture result')
            value='0.0' # Proven descriptor literal, NOT fabricated memory contents.
        else:
            args=[expr(j) for j in n[1:]]
            if op=='absolute':rhs=f'abs({args[0]})'
            elif op=='negate':rhs=f'(-{args[0]})'
            elif op=='greater':rhs=f'({args[0]}>{args[1]}?1.0:0.0)'
            elif op=='maximum':rhs=f'({args[0]}>={args[1]}?{args[0]}:{args[1]})'
            elif op=='reciprocal':rhs=f'({args[0]}==0.0?uintBitsToFloat((floatBitsToUint({args[0]})&0x80000000u)|0x7F800000u):1.0/{args[0]})'
            else:raise RawShaderError('unsupported bounded vertex operation '+op)
            value=f'n{i}';lines.append(f'precise float {value}={rhs};')
        done[i]=value;return value
    # Evaluate executed texture coordinates even for a descriptor literal.
    for i in textures[0]['coordinates']:expr(i)
    for i,n in enumerate(nodes):
        if n[0]!='initial':expr(i)
    position=[expr(i) for i in v['exports']['62']]
    xy=[expr(i) for i in v['exports']['0'][:2]]
    lines += ['position=vec4('+','.join(position)+');','xy=vec2('+','.join(xy)+');']
    text=COMMON+'''layout(push_constant) uniform Viewport {vec4 scaleOffset;} viewport;
layout(location=0) out vec2 interpolatorXY;
void originalVertex(uint index,out vec4 position,out vec2 xy){
position=vec4(uintBitsToFloat(0x7FC00000u));xy=vec2(uintBitsToFloat(0x7FC00000u));
'''+ '\n'.join(lines)+'''
}
void main(){
vec4 positions[3];vec2 parameters[3];
for(uint i=0u;i<3u;++i)originalVertex(i,positions[i],parameters[i]);
precise vec3 edges;
for(int i=0;i<3;++i){precise vec2 e=positions[(i+2)%3].xy-positions[(i+1)%3].xy;edges[i]=e.x*e.x+e.y*e.y;}
int a=(edges.x>edges.y && edges.x>edges.z)?0:(edges.y>edges.z?1:2);
int b=(a+1)%3,c=(a+2)%3;
const uint corners[6]=uint[6](0u,1u,2u,2u,1u,3u);
uint corner=corners[uint(gl_VertexIndex)%6u];int chosen=corner==0u?a:corner==1u?b:c;
precise vec4 syntheticPosition=(positions[b]-positions[a])+positions[c];
precise vec2 syntheticParameter=(parameters[b]-parameters[a])+parameters[c];
gl_Position=corner==3u?syntheticPosition:positions[chosen];
interpolatorXY=corner==3u?syntheticParameter:parameters[chosen];
gl_Position.xy=gl_Position.xy*viewport.scaleOffset.xy+viewport.scaleOffset.zw*gl_Position.w;
}
'''
    return text,dict(vertex=v,pixel_initial_components=sorted(initial),vertex_descriptor_literal=True,
        omitted_varying_lanes=[2,3],position_and_uv_fetch_copy=True,
        native_game_pipeline=False,rectangle_coverage_verified=False)
