"""Prove narrow split inline switches with missing one-instruction tail cases.

No function-map widening or automatic patching. Existing mapped targets remain
on their original path. Only an unmapped exact unconditional relative branch to
an existing mapped function, or an exact BLR, can be replaced, retaining the
live table check. An exact BGT-LR default keeps its original early return.
"""
from field_product_fixture import d, x, rotate, conditional
from switch_tables import function_words, signed16


def verify_split_tail_switch(image, image_base, base, mapped):
    words = function_words(image,image_base,base,36)
    op, selector, pointer = words[0]>>26,(words[0]>>21)&31,(words[0]>>16)&31
    offset = signed16(words[0]); count = (words[1]&0xFFFF)+1
    if op not in (34,40) or selector != 11 or not pointer or not 1 <= count <= min(4096,1<<(8 if op==34 else 16)):
        raise ValueError('not a bounded byte/halfword selector load')
    default_returns=words[2]==0x4D990020
    if words[1] != d(10,24,11,count-1) or (not default_returns and words[2]&0xFFFF0003 != conditional(12,25,0)):
        raise ValueError('not the exact unsigned guard')
    branch = words[2]&0xFFFC
    if branch&0x8000:branch-=0x10000
    default = 0 if default_returns else base+8+branch
    low, shift = (words[4],words[5]) if words[4]>>26==14 else (words[5],words[4])
    if words[3]&0xFFFF0000 != d(15,12,0,0) or low&0xFFFF0000 != d(14,12,12,0) or \
            shift != rotate(11,0,2,0,29) or list(words[6:]) != [x(0,12,0,23),x(0,9,0,467),0x4E800420]:
        raise ValueError('not the exact absolute dispatch register chain')
    table_address = (((words[3]&0xFFFF)<<16)+signed16(low))&0xFFFFFFFF
    if table_address != base+36 or (not default_returns and default not in mapped):
        raise ValueError('table must be inline and default already mapped')
    labels = function_words(image,image_base,table_address,count*4)
    table_end = table_address+4*count
    missing,returns = {},set()
    for target in set(labels):
        if target%4 or base <= target < table_end:
            raise ValueError('target enters dispatch/table data')
        if target in mapped:continue
        instruction = function_words(image,image_base,target,4)[0]
        if instruction==0x4E800020:
            returns.add(target);continue
        if instruction&0xFC000003 != 0x48000000:
            raise ValueError('unmapped case is not one unconditional relative tail branch')
        displacement = instruction&0x3FFFFFC
        if displacement&0x2000000:displacement-=0x4000000
        destination = target+displacement
        if destination not in mapped or base <= destination < table_end:
            raise ValueError('tail must reach an existing non-table mapping')
        missing[target]=destination
    if not missing and not returns:raise ValueError('no missing tail entry to repair')
    return dict(base=base,selector=11,pointer=pointer,offset=offset,load='lbz' if op==34 else 'lhz',
                count=count,default=default,table_address=table_address,labels=tuple(labels),
                high=signed16(words[3]),low=signed16(low),low_first=words[4]>>26==14,missing=missing,
                returns=tuple(sorted(returns)),default_returns=default_returns)


def original_head(proof):
    p=proof; reg=p['pointer']; offset=p['offset']; base=p['base']; target=p['default']
    bits=8 if p['load']=='lbz' else 16
    high=f"\t// lis r12,{p['high']}\n\tctx.r12.s64 = {p['high']*65536};\n"
    low=f"\t// addi r12,r12,{p['low']}\n\tctx.r12.s64 = ctx.r12.s64 + {p['low']};\n"
    shift="\t// rlwinm r0,r11,2,0,29\n\tctx.r0.u64 = __builtin_rotateleft64(ctx.r11.u32 | (ctx.r11.u64 << 32), 2) & 0xFFFFFFFC;\n"
    guard=("\t// bgtlr cr6\n\tif (ctx.cr6.gt) return;\n" if p['default_returns'] else
        f"\t// bgt cr6,0x{target:x}\n\tif (ctx.cr6.gt) {{\n\t\tsub_{target:X}(ctx, base);\n\t\treturn;\n\t}}\n")
    return (f"PPC_FUNC_IMPL(__imp__sub_{base:X}) {{\n\tPPC_FUNC_PROLOGUE();\n"
            f"\t// {p['load']} r11,{offset}(r{reg})\n\tctx.r11.u64 = PPC_LOAD_U{bits}(ctx.r{reg}.u32 + {offset});\n"
            f"\t// cmplwi cr6,r11,{p['count']-1}\n\tctx.cr6.compare<uint32_t>(ctx.r11.u32, {p['count']-1}, ctx.xer);\n"
            +guard+high+(low+shift if p['low_first'] else shift+low)+
            "\t// lwzx r0,r12,r0\n\tctx.r0.u64 = PPC_LOAD_U32(ctx.r12.u32 + ctx.r0.u32);\n"
            "\t// mtctr r0\n\tctx.ctr.u64 = ctx.r0.u64;\n\t// bctr \n\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n\treturn;\n}")


def patched_head(function,proof):
    if function != original_head(proof):raise ValueError('generated head differs from exact original form')
    lines=[]
    for index,target in enumerate(proof['labels']):
        if target in proof['returns']:
            lines.append(f"\tif (ctx.r11.u32 == {index} && ctx.ctr.u32 == 0x{target:X}U) return;")
            continue
        if target not in proof['missing']:continue
        destination=proof['missing'][target]
        lines.append(f"\tif (ctx.r11.u32 == {index} && ctx.ctr.u32 == 0x{target:X}U) {{ sub_{destination:X}(ctx, base); return; }}")
    return function.replace('\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);','\n'.join(lines)+'\n\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);')
