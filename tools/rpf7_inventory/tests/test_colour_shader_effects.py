"""Authored instructions only; both forward paths are checked, not executed."""
import unittest
from test_raw_shader_analysis import program, cf_exec, cf_alloc, alu
from colour_shader_effects import inventory_colour_pixel_effects
from raw_shader_analysis import RawShaderError


def conditional_program(hidden=None, jump=None):
    # Both paths join at control5. The conditional body's effect must be
    # inspected even when a hypothetical predicate would skip it.
    controls = [cf_alloc(), cf_exec(address=3, opcode=1),
                (11 << 44) | (1 << 14) | 5 if jump is None else jump,
                cf_exec(address=4, opcode=5), 0,
                cf_exec(address=5, opcode=2)]
    return program(controls, [alu(), hidden or alu(), alu()])


class ColourShaderEffectsTests(unittest.TestCase):
    def test_comparison_select_and_scalar_subtract_are_effect_only(self):
        for vector in (5, 14):
            for scalar in (46, 50):
                a, b, c = alu()
                instruction = ((a & ((1 << 26) - 1)) | (scalar << 26), b,
                               (c & ~(31 << 24)) | (vector << 24))
                with self.subTest(vector=vector, scalar=scalar):
                    result = inventory_colour_pixel_effects(conditional_program(instruction))
                    self.assertEqual(result['instructions'][1]['vector'], vector)
                    self.assertEqual(result['instructions'][1]['scalar'], scalar)
                    for key in ('kill_effects', 'depth_exports', 'memory_exports',
                                'arithmetic_verified', 'sampling_verified', 'native_admission'):
                        self.assertFalse(result[key])
                    # A colour-only opcode cannot legitimize a noncolour export.
                    for dest in (32, 61, 63):
                        bad = ((instruction[0] & ~63) | dest, *instruction[1:])
                        with self.assertRaises(RawShaderError):
                            inventory_colour_pixel_effects(conditional_program(bad))

    def test_both_forward_paths_without_numeric_execution(self):
        result = inventory_colour_pixel_effects(conditional_program())
        self.assertEqual([r['address'] for r in result['instructions']], [3, 4, 5])
        self.assertTrue(result['all_forward_paths_inspected'])
        self.assertEqual(result['exports'], [0])
        for key in ('native_admission', 'arithmetic_verified', 'sampling_verified',
                    'control_flow_execution_verified', 'memory_exports', 'depth_exports'):
            self.assertFalse(result[key])

    def test_memory_and_depth_exports_in_hidden_branch_refuse(self):
        for dest in (1, 3, 32, 33, 37, 61, 62, 63):
            with self.subTest(dest=dest), self.assertRaises(RawShaderError):
                inventory_colour_pixel_effects(conditional_program(alu(dest=dest)))

    def test_backward_self_and_outside_jumps_refuse(self):
        for target in (0, 2, 6, 8191):
            with self.subTest(target=target), self.assertRaises(RawShaderError):
                inventory_colour_pixel_effects(conditional_program(jump=(11 << 44) | (1 << 14) | target))

    def test_unknown_loop_call_bool_jump_and_reserved_fields_refuse(self):
        for control in ((7 << 44), (9 << 44), (11 << 44) | 5,
                        (11 << 44) | (1 << 14) | (1 << 32) | 5):
            with self.subTest(control=control), self.assertRaises(RawShaderError):
                inventory_colour_pixel_effects(conditional_program(jump=control))

    def test_unknown_kill_and_address_operations_refuse_even_zero_write_masks(self):
        for vector in (24, 25, 26, 27, 29, 31):
            a, b, c = alu(vector=0)
            bad = (a, b, (c & ~(31 << 24)) | (vector << 24))
            with self.subTest(vector=vector), self.assertRaises(RawShaderError):
                inventory_colour_pixel_effects(conditional_program(bad))
        for scalar in (23, 24, 35, 36, 37, 38, 39, 63):
            a, b, c = alu(vector=0)
            bad = ((a & ((1 << 26) - 1)) | (scalar << 26), b, c)
            with self.subTest(scalar=scalar), self.assertRaises(RawShaderError):
                inventory_colour_pixel_effects(conditional_program(bad))

    def test_relative_export_cannot_hide_memory_destination(self):
        a, b, c = alu()
        for bit in (6, 7):
            with self.subTest(bit=bit), self.assertRaises(RawShaderError):
                inventory_colour_pixel_effects(conditional_program((a | (1 << bit), b, c)))

    def test_invalid_extents_and_missing_end_refuse(self):
        good = conditional_program()
        for bad in (b'', good[:-1], b'\0' * 4104, b'\0' * 12):
            with self.subTest(length=len(bad)), self.assertRaises(RawShaderError):
                inventory_colour_pixel_effects(bad)

    def test_instruction_overlap_refuses(self):
        bad = program([cf_alloc(), cf_exec(address=0, opcode=2)], [alu()])
        with self.assertRaises(RawShaderError):
            inventory_colour_pixel_effects(bad)

    def test_colour_allocation_and_fetch_effect_guards(self):
        for kind in (0, 1, 3):
            bad = program([cf_alloc(kind), cf_exec()], [alu()])
            with self.subTest(kind=kind), self.assertRaises(RawShaderError):
                inventory_colour_pixel_effects(bad)
        for opcode in (0, 18, 24, 31):
            bad = program([cf_alloc(), cf_exec(address=2, sequence=1, opcode=1),
                           cf_exec(address=3), 0], [(opcode, 0, 0), alu()])
            with self.subTest(opcode=opcode), self.assertRaises(RawShaderError):
                inventory_colour_pixel_effects(bad)
        good = program([cf_alloc(), cf_exec(address=2, sequence=1, opcode=1),
                        cf_exec(address=3), 0], [(1 | (12 << 20), 0, 0), alu()])
        self.assertEqual(inventory_colour_pixel_effects(good)['texture_slots'], [12])


if __name__ == '__main__':
    unittest.main()
