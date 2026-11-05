"""Bounded unsigned normalized packed vertex fetch, not a whole shader decoder."""
import math
import struct
from raw_shader_analysis import RawShaderError, _vertex_fetch_layout


def decode_unorm8_fetch(words, previous=None, registers=None, address=0):
    if (len(words) != 3 or any(type(w) is not int or not 0 <= w <= 0xFFFFFFFF for w in words) or
            (words[1] >> 16) & 63 != 6 or words[1] & 0x3FC0F000):
        raise RawShaderError('only unsigned normalized 8_8_8_8 with zero modifiers supported')
    if registers is None:
        registers = [[f'initial_r{r}.{axis}' for axis in 'xyzw'] for r in range(64)]
    return _vertex_fetch_layout(words, previous, registers, address, 6, 4)


def f32(value):
    return struct.unpack('>f', struct.pack('>f', value))[0]


def read_unorm8_fetch(plan, source, index, prior=(None, None, None, None)):
    """BE dword, low-byte-first lanes, float32 reciprocal-multiply convention.

    Unknown KEEP lanes stay None. This does not verify retail normalized rounding.
    Source is a complete explicitly owned buffer and index is the saved full index.
    """
    if (not isinstance(source, bytes) or not 0 < len(source) <= 4096 or len(source) % 4 or
            type(index) is not int or not 0 <= index < 1 << 24 or len(prior) != 4 or
            plan['format'] != 6 or plan['components'] != 4):
        raise RawShaderError('invalid bounded packed vertex source/index/format')
    for key, low, high in (('stride_words', 1, 256), ('offset_words', 0, 0x400000),
                          ('fetch_slot', 0, 96), ('destination', 0, 64)):
        if type(plan[key]) is not int or not low <= plan[key] < high:
            raise RawShaderError('invalid packed vertex layout')
    swizzle = plan['destination_swizzle']
    if len(swizzle) != 4 or any(type(s) is not int or s not in (0, 1, 2, 3, 4, 5, 7) for s in swizzle):
        raise RawShaderError('invalid packed vertex swizzle')
    start = (index * plan['stride_words'] + plan['offset_words']) * 4
    if start + 4 > len(source): raise RawShaderError('packed fetch exceeds captured buffer')
    word = int.from_bytes(source[start:start+4], 'big')
    values = [f32(((word >> (i*8)) & 255) * f32(1.0/255.0)) for i in range(4)]
    output = list(prior)
    for lane, select in enumerate(swizzle):
        if select == 7:
            if output[lane] is not None and not math.isfinite(output[lane]):
                raise RawShaderError('non-finite kept vertex lane')
        else: output[lane] = values[select] if select < 4 else float(select-4)
    return output
