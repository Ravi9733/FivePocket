from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from inline_capture_preflight import (shader_requirements, literal_register_requirements,
                                      draw_register, compare_state, capture_requirements,
                                      submitted_texture_requirements, compact_summary)
from test_raw_shader_analysis import program, cf_alloc, cf_exec, alu
from test_inline_shader_translation import vertex, pixel


class CapturePreflightTests(unittest.TestCase):
    def test_scalar_fan_inventory_is_explicit_and_never_query_admission(self):
        from test_raw_vertex_fetch_analysis import fetch
        v=program([cf_exec(address=3,sequence=1,opcode=1),cf_alloc(2),cf_alloc(1),
                   cf_exec(address=4,count=2),0,0],
                  [fetch(dst=1,slot=95,fmt=38,stride=4,swizzle=0x688),
                   alu(dest=0,source=1,vector=1),alu(dest=62,source=1)])
        p=program([cf_alloc(),cf_exec()],[alu(swizzle=0x6C)])
        draw=dict(primitive=5,vertex_count=24,stride=16,index_count=0,textures=[])
        payloads={'vertex-shader.bin':v,'pixel-shader.bin':p,
                  'vertices.bin':bytes(24*16),'device-state.bin':bytes(13860)}
        report=capture_requirements(draw,payloads,{},'scalar-fan')
        self.assertEqual(report['selected_profile']['translation'],
                         dict(accepted=True,native_admission=False,abi=31))
        self.assertFalse(compact_summary(report)['native_admission'])
        self.assertFalse(capture_requirements(draw,payloads,{},'loading')['selected_profile']['translation']['accepted'])
        for replacement in (dict(primitive=4),dict(index_count=3)):
            r=capture_requirements({**draw,**replacement},payloads,{},'scalar-fan')
            self.assertFalse(r['selected_profile']['translation']['accepted'])

    def test_screen_profile_reaches_generated_xy_without_native_admission(self):
        from test_screen_space_rectangle import position_vertex, pixel as screen_pixel
        draw = dict(primitive=8, vertex_count=3, stride=16, index_count=0, textures=[{'slot':2}])
        payloads = {'vertex-shader.bin':position_vertex(), 'pixel-shader.bin':screen_pixel(),
                    'vertices.bin':bytes(48), 'device-state.bin':bytes(16384)}
        report = capture_requirements(draw, payloads, {}, 'screen-space')
        translation = report['selected_profile']['translation']
        self.assertEqual(translation, dict(accepted=True, abi=26, native_admission=False,
            screen_space_parameter_register=0, screen_space_native_state_verified=False))
        self.assertFalse(compact_summary(report)['native_admission'])
        previous = capture_requirements(draw, payloads, {}, 'composite')
        self.assertFalse(previous['selected_profile']['translation']['accepted'])

    def test_screen_inventory_does_not_invent_generated_zw_or_registers(self):
        from test_screen_space_rectangle import position_vertex, pixel as screen_pixel
        draw = dict(primitive=8, vertex_count=3, stride=16, index_count=0, textures=[{'slot':2}])
        for shader in (screen_pixel(source=1), screen_pixel(export_source=0)):
            payloads = {'vertex-shader.bin':position_vertex(), 'pixel-shader.bin':shader,
                        'vertices.bin':bytes(48), 'device-state.bin':bytes(16384)}
            report = capture_requirements(draw, payloads, {}, 'screen-space')
            self.assertFalse(report['selected_profile']['translation']['accepted'])
            self.assertFalse(report['selected_profile']['translation']['native_admission'])

    def test_selected_lod_profile_counts_state_writes_not_texture_slots(self):
        from test_explicit_lod_dataflow import code, set_lod, lod_fetch
        shader = code([set_lod(), lod_fetch(), alu()], 5)
        draw = dict(primitive=8, vertex_count=3, stride=16, index_count=0, textures=[{'slot':5}])
        payloads = {'vertex-shader.bin':vertex(1), 'pixel-shader.bin':shader,
                    'vertices.bin':bytes(48), 'device-state.bin':bytes(16384)}
        report = capture_requirements(draw, payloads, {}, 'explicit-lod')
        selected = report['selected_profile']
        self.assertEqual(selected['translation'], dict(accepted=True, abi=9, native_admission=False))
        self.assertFalse(report['loading_shader_translation']['accepted'])  # Explicitly older ABI6.
        self.assertEqual([r['kind'] for r in selected['pixel']['instructions']], ['set_lod','fetch','alu'])
        self.assertNotIn('slot', selected['pixel']['instructions'][0])
        self.assertEqual(compact_summary(report)['stages']['pixel']['instruction_kinds'],
                         dict(set_lod=1,fetch=1,alu=1))

    def test_current_inventory_keeps_all_predicates_after_first_scalar_failure(self):
        first = list(alu()); first[0] = (first[0] & ((1 << 26)-1)) | (28 << 26)
        second = list(alu()); second[1] |= 1 << 28
        third = list(alu()); third[1] |= (1 << 28) | (1 << 27)
        code = program([cf_alloc(),cf_exec(count=3)], [first,second,third])
        proof = shader_requirements(code, 'pixel', 'explicit-lod')
        self.assertEqual([i['kind'] for i in proof['issues']],
                         ['analyzer_opcode', 'instruction_predication', 'instruction_predication'])
        self.assertEqual([i['condition'] for i in proof['issues'][1:]], [False,True])
        self.assertEqual(proof['issues'][0]['opcode'], 28)
        self.assertFalse(proof['dataflow']['accepted'])
        self.assertEqual(len(proof['instructions']),3)

    def test_inventory_and_translator_share_loading_opcode_sets(self):
        from pixel_sample_dataflow import pixel_supported_opcodes
        for scalar in pixel_supported_opcodes(ui_operations=True,extended_math=True,loading_math=True)[1]:
            instruction = list(alu()); instruction[0] = (instruction[0]&((1<<26)-1)) | (scalar<<26)
            proof = shader_requirements(program([cf_alloc(),cf_exec(count=1)], [instruction]), 'pixel', 'explicit-lod')
            self.assertFalse(any(i['kind']=='analyzer_opcode' for i in proof['issues']), scalar)
        with self.assertRaises(ValueError):
            shader_requirements(bytes(12), 'pixel', 'guess')

    def test_submission_diagnostics_preserve_unsupported_and_unbound_metadata(self):
        words = [2 | (4 << 22) | (1 << 31), 0x12345014, 0, 0,
                 (1 << 2) | (3 << 6), 0x23456000 | (1 << 9) | 2048]
        slots = [None] * 32; slots[7] = words; slots[31] = [0] * 6
        draw = {'submitted_textures': slots, 'submitted_texture_source': 'driver-upload',
                'textures': [{'slot': 7, 'fetch': words}]}
        result = submitted_texture_requirements(draw, [7, 15, 31])
        self.assertTrue(result['available']); self.assertEqual(len(result['slots']), 32)
        self.assertEqual(result['slots'][7]['mip_address'], 0x23456000)
        self.assertEqual(result['slots'][7]['mip_min'], 1)
        self.assertEqual(result['slots'][7]['mip_max'], 3)
        self.assertEqual(result['slots'][7]['pitch_texels'], 128)
        self.assertTrue(result['slots'][7]['packed_mips'])
        self.assertFalse(result['slots'][7]['ownership_verified'])
        self.assertEqual([i['kind'] for i in result['issues']],
                         ['required_submission_unavailable', 'required_nontexture_submission'])
        draw['textures'][0]['fetch'] = [0] * 6
        self.assertEqual(submitted_texture_requirements(draw, [7])['issues'][0]['kind'],
                         'captured_submission_mismatch')
        for bad in ([0] * 5, [0] * 5 + [True], [0] * 5 + [1 << 32]):
            slots[7] = bad
            self.assertEqual(submitted_texture_requirements(draw, [7])['issues'][0]['kind'],
                             'invalid_submitted_descriptor_words')

    def test_old_or_invalid_capture_never_invents_submitted_state(self):
        for draw in ({}, {'textures': [{'slot': 7, 'fetch': [2] * 6}]}):
            result = submitted_texture_requirements(draw, [7])
            self.assertFalse(result['available']); self.assertEqual(result['slots'], [])
        for slots, source in (([None] * 31, 'driver-upload'), ([None] * 32, 'cpu-shadow')):
            result = submitted_texture_requirements({'submitted_textures': slots,
                'submitted_texture_source': source}, [7])
            self.assertFalse(result['available'])

    def test_inventory_continues_after_first_unsupported_opcode(self):
        first = list(alu()); first[0] = (first[0] & ((1 << 26) - 1)) | (44 << 26)
        second = list(alu()); second[0] = (second[0] & ((1 << 26) - 1)) | (19 << 26)
        second[2] = (second[2] & ~(31 << 24)) | (17 << 24)
        proof = shader_requirements(program([cf_alloc(), cf_exec(count=2)], [first, second]), 'pixel')
        self.assertEqual([(r['address'],r['unit'],r['opcode']) for r in proof['issues']],
                         [(1,'scalar',44),(2,'vector',17),(2,'scalar',19)])
        self.assertFalse(proof['dataflow']['accepted'])
        self.assertEqual(len(proof['instructions']), 2)

    def test_unknown_control_does_not_invent_instruction_schedule(self):
        proof = shader_requirements(bytes(12), 'vertex')
        self.assertEqual(proof['instructions'], [])
        self.assertEqual(proof['issues'][0]['kind'], 'control_flow_unknown')
        with self.assertRaises(ValueError): shader_requirements(bytes(12), 'guess')

    def test_literal_pairs_and_all_missing_or_mismatched_state(self):
        required = literal_register_requirements('{{0x2000,0x12}},{{0x2001,3}},{{0x210F,9}}')
        state = bytearray(10376)
        struct.pack_into('>I', state, 10368, 0x12)
        rows = compare_state(state, [0,0,0], required)
        self.assertEqual(rows, [{'register':'0x2001','expected':3,'actual':0}])
        self.assertEqual(len(compare_state(b'', [], required)), 2) # Dynamic viewport separate.
        self.assertIsNone(draw_register(state, [], 0x2081))
        self.assertIsNone(draw_register(state, [], 0x9000))
        self.assertEqual(draw_register(state, [0,7,8], 0x2081), 7)
        with self.assertRaises(ValueError): literal_register_requirements('{{0x2000,1}},{{0x2000,2}}')

    def test_multiple_draw_and_state_requirements_survive_pair_rejection(self):
        payloads = {'vertex-shader.bin':vertex(1), 'pixel-shader.bin':pixel((2,2)),
                    'vertices.bin':bytes(96), 'device-state.bin':bytes(16384)}
        draw = {'primitive':4,'vertex_count':6,'stride':16,'index_count':0,
                'scissor':[0,0,0], 'textures':[]}
        report = capture_requirements(draw, payloads, {'state':'{{0x2000,1}},{{0x2201,2}}'})
        self.assertEqual({r['kind'] for r in report['draw_issues']},
                         {'topology','quad_expansion_extent','repeated_texture_slot',
                          'interpolator_layout','missing_captured_texture_slots'})
        self.assertEqual(len(report['literal_state_mismatches']), 2)
        self.assertFalse(report['inline_pair']['accepted'])
        self.assertEqual(report['legacy_requirements_abi'], 4)
        self.assertFalse(report['extended_shader_translation']['accepted']) # Second UV was not exported.
        self.assertIn('ABI5 native state/coverage/ownership and installed shader identity', report['unverified'])
        self.assertIn('render target identity/clear/resolve lifetime', report['unverified'])
        self.assertEqual(len(report['payload_sha256']), len(payloads))

    def test_extended_translation_is_separate_from_legacy_and_native_admission(self):
        payloads = {'vertex-shader.bin':vertex(1), 'pixel-shader.bin':pixel((2,2),second_source=0),
                    'vertices.bin':bytes(96), 'device-state.bin':bytes(16384)}
        draw = {'primitive':4,'vertex_count':6,'stride':16,'index_count':0,
                'scissor':[0,0,0], 'textures':[{'slot':2}]}
        report = capture_requirements(draw, payloads, {})
        self.assertFalse(report['inline_pair']['accepted'])
        self.assertTrue(report['extended_shader_translation']['accepted'])
        self.assertTrue(report['extended_shader_translation']['direct_triangles'])
        # This synthetic VS has no affine full-rectangle proof. Translation
        # success must not imply it is admitted by the new offscreen renderer.
        self.assertIsNone(report['extended_shader_translation']['affine_position_xy'])
        self.assertTrue(report['unverified'])

    def test_existing_linkable_pair_is_not_called_complete_compatibility(self):
        draw = {'primitive':4,'vertex_count':6,'stride':16,'index_count':6,
                'scissor':[0,0,0], 'textures':[{'slot':2},{'slot':3}]}
        payloads = {'vertex-shader.bin':vertex(), 'pixel-shader.bin':pixel(),
                    'vertices.bin':bytes(96), 'device-state.bin':bytes(16384)}
        report = capture_requirements(draw, payloads, {})
        self.assertEqual(report['draw_issues'], [])
        self.assertTrue(report['inline_pair']['accepted'])
        self.assertTrue(report['unverified'])

    def test_geometry_profiles_are_selectable_and_cover_their_own_opcodes(self):
        # A capture needing a geometry-only opcode was previously reported as an
        # unsupported opcode against a profile it could never be analysed under,
        # because no --profile name reached geometry_profile at all.
        from inline_capture_preflight import PROFILES, GEOMETRY_PROFILES, profile_options
        self.assertTrue(set(GEOMETRY_PROFILES) <= set(PROFILES))

        # Scalar 11 (FRAC) is admitted by the geometry scalar set and by no
        # other profile this tool exposes. Assert both halves.
        frac = list(alu()); frac[0] = (frac[0] & ((1 << 26)-1)) | (11 << 26)
        code = program([cf_alloc(), cf_exec(count=1)], [frac])
        unsupported = lambda profile: [
            tuple(i['opcode']) if isinstance(i.get('opcode'), list) else i
            for r in [shader_requirements(code, 'pixel', profile)]
            for i in r['issues'] if i['kind'] == 'analyzer_opcode']
        for profile in ('loading', 'explicit-lod', 'predicated-lod'):
            self.assertTrue(unsupported(profile), profile)
        for profile in GEOMETRY_PROFILES:
            self.assertEqual(unsupported(profile), [], profile)

        # Every exposed profile must produce a usable option set, and the
        # geometry flags must stay off for every non-geometry profile.
        for profile in PROFILES:
            options = profile_options(profile)
            self.assertEqual(options['geometry_profile'], profile in GEOMETRY_PROFILES, profile)
            if profile not in GEOMETRY_PROFILES:
                self.assertFalse(options['geometry_predicate_profile'], profile)
                self.assertFalse(options['geometry_gradient_depth_profile'], profile)
                self.assertFalse(options['geometry_fixed16_effects_profile'], profile)

        # Naming the profiles must not admit anything: an unrelated capture is
        # still rejected, by a structural rule rather than by an opcode.
        draw = dict(primitive=8, vertex_count=3, stride=16, index_count=0, textures=[])
        payloads = {'vertex-shader.bin': vertex(1), 'pixel-shader.bin': pixel(),
                    'vertices.bin': bytes(48), 'device-state.bin': bytes(16384)}
        report = capture_requirements(draw, payloads, {}, 'geometry')
        self.assertFalse(report['selected_profile']['translation']['accepted'])


if __name__ == '__main__': unittest.main()
