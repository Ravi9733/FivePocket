"""ABI24 tonemap profile: the four scalar opcodes it adds and the gate on them.

SIN/COS are Xenia AluScalarOpcode 48/49 and are the first periodic
transcendentals this translator admits. They take radians, as the hardware
instruction does. Nothing here claims to reproduce the Xbox's own
approximation; what is pinned is the operation, its finite-normal input
discipline, its bounded argument range, and that it is unreachable without the
explicit profile.
"""
from pathlib import Path
import math
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from loading_shader_math import LOADING_OPERATIONS, evaluate_loading_operation
from raw_shader_analysis import RawShaderError
from pixel_sample_dataflow import pixel_supported_opcodes
from vertex_arithmetic_dataflow import vertex_supported_opcodes
from inline_shader_translation import translate_inline_pair
from test_raw_shader_analysis import alu
from test_extended_shader_math import vertex_code
from test_pixel_sample_dataflow import pixel

RECTANGLE = dict(indexed=False, extended_profile=True, loading_profile=True,
                 rectangle_list=True, offset_sample_profile=True)


def two_stage(interpolator_mask):
    """A rectangle pair whose vertex stage writes that mask of interpolator 0."""
    vertex = vertex_code(alu(dest=0, vector=interpolator_mask, source=0),
                         tail=[alu(dest=62, vector=15, source=0)])
    return vertex, pixel([alu(dest=0, vector=15, source=0)])

LOADING = dict(ui_operations=True, extended_math=True, loading_math=True)


class TonemapScalarMathTests(unittest.TestCase):
    def test_sine_and_cosine_are_loading_operations(self):
        self.assertIn('sine', LOADING_OPERATIONS)
        self.assertIn('cosine', LOADING_OPERATIONS)

    def test_radian_argument_matches_the_host_operation(self):
        for value in (0.0, 0.5, -0.5, 1.0, 3.0, -2.25, 1024.0):
            self.assertEqual(evaluate_loading_operation('sine', [value]),
                             self.float32(math.sin(value)))
            self.assertEqual(evaluate_loading_operation('cosine', [value]),
                             self.float32(math.cos(value)))

    def test_bounded_argument_range(self):
        # Beyond this the host's own range reduction, not the operation, would
        # be what the result depends on.
        evaluate_loading_operation('sine', [2.0 ** 24])
        for value in (2.0 ** 25, -(2.0 ** 25)):
            for op in ('sine', 'cosine'):
                with self.assertRaises(RawShaderError):
                    evaluate_loading_operation(op, [value])

    def test_inputs_keep_the_finite_normal_discipline(self):
        for value in (float('inf'), float('nan'), 2.0 ** -130):
            for op in ('sine', 'cosine'):
                with self.assertRaises(RawShaderError):
                    evaluate_loading_operation(op, [value])

    def test_arity_is_one(self):
        for op in ('sine', 'cosine'):
            with self.assertRaises(RawShaderError):
                evaluate_loading_operation(op, [1.0, 2.0])

    def test_opcodes_are_unreachable_without_the_profile(self):
        _, without = pixel_supported_opcodes(rectangle_mrt_profile=True, **LOADING)
        _, with_it = pixel_supported_opcodes(rectangle_mrt_profile=True, tonemap_profile=True, **LOADING)
        for opcode in (11, 16, 48, 49):
            self.assertNotIn(opcode, without)
            self.assertIn(opcode, with_it)

    def test_vertex_dot2add_is_admitted_without_packed_geometry(self):
        plain, _ = vertex_supported_opcodes(dot4=True, scalar_constants=True, loading_math=True)
        tonemap, _ = vertex_supported_opcodes(dot4=True, scalar_constants=True, loading_math=True, tonemap=True)
        self.assertNotIn(17, plain)
        self.assertIn(17, tonemap)
        # SGE belongs to the packed profile and does not come along with it.
        self.assertNotIn(6, tonemap)

    def test_interpolator_width_follows_what_the_vertex_stage_wrote(self):
        # Four lanes is what the shipped ABI23/ABI24 pairs write, and stays a
        # vec4. A two-lane prefix now declares vec2 instead of being refused.
        for mask, declaration in ((15, 'vec4 interpolator0'), (3, 'vec2 interpolator0')):
            vertex, fragment = two_stage(mask)
            translated, _, _ = translate_inline_pair(vertex, fragment, **RECTANGLE)
            self.assertIn(declaration, translated)

    def test_narrowing_never_admits_an_unwritten_lane(self):
        # The point of the previous test is that it costs nothing: a lane the
        # pixel stage reads but the vertex stage never wrote still fails.
        vertex, fragment = two_stage(1)
        with self.assertRaises(RawShaderError) as caught:
            translate_inline_pair(vertex, fragment, **RECTANGLE)
        self.assertIn('unprovided stage input', str(caught.exception))

    def test_lane_gaps_are_refused(self):
        # vecN cannot carry a lane the vertex stage skipped over.
        vertex, fragment = two_stage(0b0101)
        with self.assertRaises(RawShaderError) as caught:
            translate_inline_pair(vertex, fragment, **RECTANGLE)
        self.assertIn('contiguous prefix', str(caught.exception))

    def test_vertex_only_needs_its_own_profile_and_an_absent_pixel_stage(self):
        # ABI25 emits no fragment stage at all. It is reachable only through the
        # direct triangle loading profile, and only with a pixel stage that is
        # genuinely absent -- passing one is a contradiction, not an override.
        vertex, fragment = two_stage(15)
        direct = dict(indexed=False, extended_profile=True, loading_profile=True,
                      direct_triangles=True)
        with self.assertRaises(RawShaderError) as caught:
            translate_inline_pair(vertex, fragment, **direct, vertex_only=True)
        self.assertIn('absent pixel stage', str(caught.exception))
        # Without the direct triangle profile it is refused outright.
        with self.assertRaises(RawShaderError) as caught:
            translate_inline_pair(vertex, b'', indexed=False, extended_profile=True,
                                  loading_profile=True, vertex_only=True)
        self.assertIn('direct triangle', str(caught.exception))

    def test_terminating_exec_may_carry_no_instructions(self):
        # The stencil pass's vertex stage ends its control block with an
        # EXEC_END that executes nothing. An ordinary EXEC still may not.
        import struct
        from vertex_arithmetic_dataflow import schedule
        def control_words(*blocks):
            words = []
            for first, second in blocks:
                words.extend([first & 0xFFFFFFFF,
                              ((first >> 32) & 0xFFFF) | ((second & 0xFFFF) << 16),
                              second >> 16])
            return words

        def exec_block(op, address, count, sequence=0):
            return (op << 44) | (sequence << 16) | (count << 12) | address

        alloc = (12 << 44) | (1 << 41)
        # Two instruction triples follow the single control triple.
        body = [0] * 6
        ended = control_words((exec_block(1, 1, 1, 1), exec_block(2, 1, 0)))
        words, controls = schedule(struct.pack('>' + 'I' * (len(ended) + len(body)), *ended, *body))
        self.assertEqual(controls[-1], ('exec', 1, 0, 0))
        # The same shape with an ordinary EXEC is still refused.
        empty = control_words((exec_block(1, 1, 1, 1), exec_block(1, 1, 0)))
        with self.assertRaises(RawShaderError):
            schedule(struct.pack('>' + 'I' * (len(empty) + len(body)), *empty, *body))
        self.assertTrue(alloc)

    def test_export_zero_fill_is_allowed_only_on_an_export(self):
        # Bit 14 zero-fills the lanes a vector/scalar mask pair leaves unwritten.
        # It only means that on an export, and both parsers apply it themselves
        # before this decoder runs, so the decoder must not refuse it there --
        # and must still refuse it off an export.
        from scalar_constant import decode_scalar_constant
        words = ((42 << 26) | (1 << 15) | (1 << 14), 0, 0)
        decode_scalar_constant(words, coissued=True, allow_clamp=True, allow_export=True)
        with self.assertRaises(RawShaderError):
            decode_scalar_constant(words, coissued=True, allow_clamp=True, allow_export=False)
        off_export = ((42 << 26) | (1 << 14), 0, 0)
        with self.assertRaises(RawShaderError):
            decode_scalar_constant(off_export, coissued=True, allow_clamp=True, allow_export=False)

    @staticmethod
    def float32(value):
        import struct
        return struct.unpack('>f', struct.pack('>f', value))[0]


if __name__ == '__main__':
    unittest.main()
