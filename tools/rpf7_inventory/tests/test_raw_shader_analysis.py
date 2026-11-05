from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from raw_shader_analysis import analyze_copy_shader, translate_constant_pair, RawShaderError


def cf_exec(address=1, count=1, opcode=2, sequence=0):
    return (opcode << 44) | (1 << 41) | address | (count << 12) | (sequence << 16)


def cf_alloc(kind=2, size=1): return (12 << 44) | (kind << 41) | (size - 1)


def alu(dest=0, vector=15, scalar=0, export=True, zero=False, source=0, swizzle=0):
    return ((50 << 26) | dest | (vector << 16) | (scalar << 20) | (int(export) << 15) | (int(zero) << 14),
            (swizzle << 16) | (swizzle << 8), (2 << 24) | (7 << 29) | (source << 16) | (source << 8))


def program(controls, instructions):
    if len(controls) % 2: controls = [*controls, 0]
    words = []
    for x, y in zip(controls[::2], controls[1::2]):
        words.extend([x & 0xFFFFFFFF, (x >> 32) | ((y & 65535) << 16), y >> 16])
    for ins in instructions: words.extend(ins)
    return struct.pack('>' + 'I' * len(words), *words)


class RawShaderTests(unittest.TestCase):
    def test_constant_exports_and_symbolic_input_copy(self):
        vertex = program([cf_alloc(1), cf_exec()], [alu(dest=62, vector=8, scalar=8, zero=True)])
        result = analyze_copy_shader(vertex, 'vertex')
        self.assertEqual(result['exports'], {'62': ['0', '0', '0', '1']})
        self.assertEqual(result['initial_components_read'], [])
        pixel = program([cf_alloc(), cf_exec()], [alu()])
        result = analyze_copy_shader(pixel, 'pixel')
        self.assertEqual(result['exports'], {'0': ['initial_r0.' + c for c in 'xyzw']})
        self.assertFalse(result['linkage_validated']); self.assertFalse(result['gpu_executed'])

    def test_relative_swizzle_and_parallel_source_snapshot(self):
        data = program([cf_alloc(), cf_exec(count=2)],
                       [alu(export=False, source=0, swizzle=0x55), alu(source=0)])
        self.assertEqual(analyze_copy_shader(data, 'pixel')['exports']['0'],
                         ['initial_r0.' + c for c in 'yzwx'])

    def test_pair_translation_requires_explicit_linkage_and_constant_vertex(self):
        vertex = program([cf_alloc(2), cf_exec(address=2, opcode=1), cf_alloc(1), cf_exec(address=3)],
                         [alu(vector=0, zero=True), alu(dest=62, vector=8, scalar=8, zero=True)])
        pixel = program([cf_alloc(), cf_exec()], [alu()])
        vs, ps, proof = translate_constant_pair(vertex, pixel, 0x1000000E, 0)
        self.assertIn('gl_Position=vec4(0.0,0.0,0.0,1.0)', vs)
        self.assertIn('color0=parameter0', ps)
        self.assertFalse(proof['title_raster_state_replayed'])
        for control, misc in ((0x1004000E, 0), (0x1010000E, 0), (0x1000000E, 1)):
            with self.assertRaises(RawShaderError): translate_constant_pair(vertex, pixel, control, misc)
        bad_vertex = program([cf_alloc(2), cf_exec(address=2, opcode=1), cf_alloc(1), cf_exec(address=3)],
                             [alu(), alu(dest=62, vector=8, scalar=8, zero=True)])
        with self.assertRaises(RawShaderError): translate_constant_pair(bad_vertex, pixel, 0x1000000E, 0)

    def test_constant_masks_for_every_combination(self):
        for mask in range(16):
            data = program([cf_alloc(), cf_exec()], [alu(vector=mask, scalar=mask, zero=True)])
            self.assertEqual(analyze_copy_shader(data, 'pixel')['exports']['0'],
                             ['1' if mask & (1 << c) else '0' for c in range(4)])

    def test_nop_mask_does_not_invent_register_initialization(self):
        data = program([cf_alloc(), cf_exec(count=2)], [alu(vector=0, export=False), alu()])
        self.assertEqual(analyze_copy_shader(data, 'pixel')['exports']['0'][0], 'initial_r0.x')

    def test_rejects_control_and_extent_errors(self):
        for controls in ([cf_alloc(3), cf_exec()], [cf_alloc(), cf_exec(address=0)],
                         [cf_alloc(), cf_exec(address=2)], [cf_alloc(), cf_exec(count=7)],
                         [cf_alloc(), cf_exec(sequence=1)], [cf_alloc(), cf_exec(opcode=3)],
                         [cf_alloc(), cf_exec(opcode=1)], [cf_alloc(), cf_exec(sequence=4)],
                         [cf_alloc() | (1 << 20), cf_exec()], [cf_alloc(), cf_exec() | (1 << 35)]):
            with self.assertRaises(RawShaderError): analyze_copy_shader(program(controls, [alu()]), 'pixel')
        for data in (b'', b'a', bytes(4104)):
            with self.assertRaises(RawShaderError): analyze_copy_shader(data, 'vertex')

    def test_rejects_unsupported_alu_flags_and_operations(self):
        base = alu()
        for word, bit in ((0, 6), (0, 7), (0, 24), (0, 25), (1, 24), (1, 28), (1, 30), (2, 30)):
            words = list(base); words[word] ^= 1 << bit
            with self.assertRaises(RawShaderError): analyze_copy_shader(program([cf_alloc(), cf_exec()], [words]), 'pixel')
        for instruction in (alu(dest=32), alu(source=64), alu(vector=0, scalar=1),
                            (base[0], base[1], base[2] ^ (1 << 8)),
                            (base[0], base[1], base[2] ^ (1 << 24))):
            with self.assertRaises(RawShaderError): analyze_copy_shader(program([cf_alloc(), cf_exec()], [instruction]), 'pixel')


if __name__ == '__main__': unittest.main()
