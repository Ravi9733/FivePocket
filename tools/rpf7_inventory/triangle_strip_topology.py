"""Nonindexed strip expansion, not draw or shader admission.

Keep degenerate triangles and their parity. Rotate the original last vertex to
the first list vertex without changing winding. Restart/index buffers are not
part of this contract. The native caller must validate its owned byte extent.
"""
from raw_shader_analysis import RawShaderError

STRIP_CORNERS = ((2, 0, 1), (2, 1, 0))


def screen_strip_indices(count):
    if type(count) is not int or not 3 <= count <= 32768:
        raise RawShaderError('screen strip needs 3..32768 owned stride8 vertices')
    return tuple(triangle + offset for triangle in range(count - 2)
                 for offset in STRIP_CORNERS[triangle & 1])


def screen_strip_index_glsl():
    arrays = ','.join(str(offset) + 'u' for row in STRIP_CORNERS for offset in row)
    return ('    const uint stripCorners[6]=uint[6](' + arrays + ');\n'
            '    uint triangle=uint(gl_VertexIndex)/3u;\n'
            '    uint corner=uint(gl_VertexIndex)%3u;\n'
            '    uint index=triangle+stripCorners[(triangle&1u)*3u+corner];\n')
