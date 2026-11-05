"""Existing generic override checks promoted from v56 and dusk-speckle generators.
No game input or generated shader payload.
"""

from __future__ import annotations   # the Mac builder runs on Xcode's Python 3.9 (str | None in signatures)

import collections, re, struct

EXEC_OPCODES = {1, 2, 3, 4, 5, 6, 13, 14}

HELPER = '''
// v56: a fetch with tx_coord_denorm set addresses texels, not [0,1] (XenosRecomp drops the bit).
float4 tfetch2DUnnormalized(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord / float2(getTexture2DDimensions(texture)), offset);
}
'''

ANCHOR = 'float4 tfetch2DArray(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, float3 offset)'

def texture_fetches(microcode: bytes) -> list[bool]:
    """tx_coord_denorm of every texture fetch the control flow executes."""
    words = struct.unpack(f'>{len(microcode) // 4}I', microcode)
    end, slot, slots = len(words) // 3, 0, set()
    while slot < end:
        w0, w1, w2 = words[3 * slot:3 * slot + 3]
        for lo, hi in ((w0, w1 & 0xFFFF), ((w1 >> 16) | ((w2 << 16) & 0xFFFFFFFF), w2 >> 16)):
            if (hi >> 12) & 0xF in EXEC_OPCODES:
                address, count, sequence = lo & 0xFFF, (lo >> 12) & 7, (lo >> 16) & 0xFFF
                if address:
                    end = min(end, address)
                slots.update(address + i for i in range(count) if (sequence >> (2 * i)) & 1)
        slot += 1
    out = []
    for f in sorted(slots):
        w0 = words[3 * f]
        if (w0 & 0x1F) == 1:  # texture fetch
            out.append(bool((w0 >> 25) & 1))
    return out

HELPER_BLOCK = """
// dusk-speckle (register LOD): a tfetch whose UseRegisterLOD bit is set samples the mip level a setTexLOD put in the
// LOD register (the sampler still applies the fetch constant's LOD bias and mip range).
float4 tfetch2DLod(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float lod)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / getTexture2DDimensions(texture), lod);
}

float4 tfetchCubeLod(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, float lod)
{
    float3 dir = cubeDir(texCoord);
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], dir, lod);
}
"""

def opcode_counts(code: bytes) -> collections.Counter:
    w = struct.unpack(f'<{len(code) // 4}I', code); c = collections.Counter(); k = 5
    while k < len(w):
        n = w[k] >> 16
        if n == 0: raise SystemExit('malformed SPIR-V')
        c[w[k] & 0xFFFF] += 1; k += n
    return c

def hlsl_change(old: str, new: str, n_set: int, n_fetch: int) -> str | None:
    """None when `new` is `old` plus exactly what the patch may add; else what is wrong. The patch's additions are taken
    OUT of the new text (the helper block, the declaration, the texLod assignments, "Lod" in the opening line and
    ", texLod" in the argument line of each register-LOD fetch): what is left must be the old text, byte for byte."""
    if new.count(HELPER_BLOCK) != 1: return f'the helper block occurs {new.count(HELPER_BLOCK)} times'
    rest, decl, sets, opens, args = [], 0, 0, 0, 0
    for l in new.replace(HELPER_BLOCK, '', 1).split('\n'):
        if l == '\tfloat texLod = 0.0;': decl += 1; continue
        if re.fullmatch(r'\t+texLod = r\d+\.[xyzw];', l): sets += 1; continue
        if re.search(r'= tfetch(2D|Cube)Lod\($', l): opens += 1; l = l[:-len('Lod(')] + '('
        elif re.search(r', texLod\)\.[xyzw01]+;$', l): args += 1; l = re.sub(r', texLod(\)\.[xyzw01]+;)$', r'\1', l)
        rest.append(l)
    if decl != 1: return f'{decl} declarations of texLod'
    if sets != n_set: return f'{sets} texLod assignments for {n_set} setTexLOD instructions'
    if opens != n_fetch or args != n_fetch: return f'{opens}/{args} register-LOD calls for {n_fetch} register-LOD fetches'
    if 'texLod' in '\n'.join(rest): return 'texLod is used elsewhere'
    if '\n'.join(rest) != old:
        a, b = old.split('\n'), rest
        k = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
        return f'the text differs otherwise at line {k + 1}: {a[k] if k < len(a) else "<end>"!r} / {b[k] if k < len(b) else "<end>"!r}'
    return None
