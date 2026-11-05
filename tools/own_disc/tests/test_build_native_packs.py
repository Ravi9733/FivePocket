#!/usr/bin/env python3
"""Tests of build_native_packs.py with stand-ins for the disc inventory, the translator and glslc: the connection
recipe -> shader from the disc -> translator -> compiler -> header text, and every refusal on the way."""
import copy
import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = Path(os.environ.get('BUILD_NATIVE_PACKS', HERE.parent / 'build_native_packs.py'))   # the mutant runner points this at a changed copy
spec = importlib.util.spec_from_file_location('build_native_packs', TOOL)
packs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packs)

sha = lambda b: hashlib.sha256(b).hexdigest()
VERTEX_RAW, PIXEL_RAW = bytes(range(10, 34)), bytes(range(100, 112))
BINDINGS = [{'address': 1, 'stride': 7}, {'address': 2, 'stride': 9}]
CANON_V, CANON_P = 'a' * 64, 'b' * 64
ITEMS = [{'canonical_sha256': CANON_V, 'stage': 'vertex', 'file': 'v1'}, {'canonical_sha256': CANON_P, 'stage': 'pixel', 'file': 'p1'},
         {'canonical_sha256': CANON_V, 'stage': 'pixel', 'file': 'other-stage'}]
FILES = {'v1': VERTEX_RAW, 'p1': PIXEL_RAW, 'other-stage': b'\x00' * 12}


def read_code(row):
    return FILES[row['file']]


def bind(raw, bindings):                      # stand-in for the fetch numbers: they end up in the bytes
    return raw + bytes(b['stride'] for b in bindings)


VERTEX = bind(VERTEX_RAW, BINDINGS)
CALLS = []


def translate(module, function, arguments, options):
    CALLS.append((module, function, arguments, options))
    text = f'{module}.{function}|' + '|'.join(a.hex() if isinstance(a, bytes) else json.dumps(a) for a in arguments) + '|' + json.dumps(options, sort_keys=True)
    return ('V:' + text, 'F:' + text, {'proof': True})


def compile_glsl(text, kind, flags):
    digest = hashlib.sha256((text + kind + ' '.join(flags)).encode()).digest()
    return struct.pack('<5I', packs.SPIRV_MAGIC, len(text), packs.KINDS.index(kind), *struct.unpack('<2I', digest[:8]))


def words_of(text, kind, flags):
    return list(struct.unpack('<5I', compile_glsl(text, kind, flags)))


def recipe():
    call = {'module': 'inline_shader_translation', 'function': 'translate_inline_pair', 'arguments': ['$s0', '$s1', [3, 4]], 'options': {'indexed': True}}
    glsl = translate(call['module'], call['function'], [VERTEX, PIXEL_RAW, [3, 4]], call['options'])
    CALLS.clear()
    flags = ['--target-env=vulkan1.1', '-O0']
    header = {'path': 'packs/one/native_one_shader_pack.h',
              'shaders': [{'id': 's0', 'stage': 'vertex', 'canonical_sha256': CANON_V, 'sha256': sha(VERTEX), 'bindings': BINDINGS},
                          {'id': 's1', 'stage': 'pixel', 'canonical_sha256': CANON_P, 'sha256': sha(PIXEL_RAW)}],
              'modules': [{'id': 'm0', 'call': call, 'result': 0, 'kind': 'vert', 'glslc': flags, 'sha256': sha(compile_glsl(glsl[0], 'vert', flags))},
                          {'id': 'm1', 'call': call, 'result': 1, 'kind': 'frag', 'glslc': flags, 'sha256': sha(compile_glsl(glsl[1], 'frag', flags))}],
              'text': ['#pragma once\nP p{{', {'bytes': 's0', 'style': 'hex', 'separator': ','}, '},\n{', {'bytes': 's1', 'style': 'hexu', 'separator': ', '},
                       '},{', {'words': 'm0', 'style': 'hex', 'separator': ','}, '},{', {'words': 'm1', 'style': 'hex2', 'separator': ',\n'}, '}};\n']}
    # the expected text, written out here by hand and not with the tool's own renderer
    expected = ('#pragma once\nP p{{' + ','.join(hex(b) for b in VERTEX) + '},\n{' + ', '.join('0x%Xu' % b for b in PIXEL_RAW)
                + '},{' + ','.join(hex(w) for w in words_of(glsl[0], 'vert', flags)) + '},{'
                + ',\n'.join('0x%02x' % w for w in words_of(glsl[1], 'frag', flags)) + '}};\n')
    header['bytes'] = len(expected.encode())
    header['sha256'] = sha(expected.encode())
    return header, expected


def make(header, items=ITEMS, read=read_code, binder=bind, tr=translate, comp=compile_glsl):
    return packs.build_header(header, items, read, binder, tr, comp)


class Header(unittest.TestCase):
    def test_the_header_is_made_from_the_disc_shaders_the_translator_and_the_compiler(self):
        header, expected = recipe()
        self.assertEqual(make(header).decode(), expected)

    def test_the_translator_gets_the_shader_bytes_the_other_arguments_and_the_options(self):
        header, _ = recipe()
        make(header)
        self.assertEqual(CALLS[0], ('inline_shader_translation', 'translate_inline_pair', [VERTEX, PIXEL_RAW, [3, 4]], {'indexed': True}))

    def test_a_list_among_the_options_reaches_the_translator_as_a_tuple(self):
        header, _ = recipe()
        module = header['modules'][0]
        module['call'] = dict(module['call'], options={'indexed': True, 'formats': [0, None, 1, None], 'mask': 7, 'filter': 'linear'})
        text = translate('inline_shader_translation', 'translate_inline_pair', [VERTEX, PIXEL_RAW, [3, 4]], module['call']['options'])[0]
        module['sha256'] = sha(compile_glsl(text, 'vert', module['glslc']))
        CALLS.clear()
        packs.module_words(module, {'s0': VERTEX, 's1': PIXEL_RAW}, translate, compile_glsl)
        options = CALLS[0][3]
        self.assertEqual(options, {'indexed': True, 'formats': (0, None, 1, None), 'mask': 7, 'filter': 'linear'})
        self.assertIs(type(options['formats']), tuple)
        self.assertIs(type(CALLS[0][2][2]), list)              # an argument that is a list stays one
        self.assertEqual(module['call']['options']['formats'], [0, None, 1, None])   # the recipe itself is not changed

    def test_a_call_can_name_an_earlier_revision_of_the_translator(self):
        header, _ = recipe()
        module, shaders, seen = header['modules'][0], {'s0': VERTEX, 's1': PIXEL_RAW}, []

        def tr(name, function, arguments, options, *revision):
            seen.append(revision)
            return translate(name, function, arguments, options)
        packs.module_words(module, shaders, tr, compile_glsl)
        self.assertEqual(seen[-1], ())                                     # no revision named: the translator of today, as before
        module['call'] = dict(module['call'], revision='r20260910')
        packs.module_words(module, shaders, tr, compile_glsl)
        self.assertEqual(seen[-1], ('r20260910',))

    def test_a_shader_that_is_not_on_the_disc_is_refused(self):
        header, _ = recipe()
        with self.assertRaisesRegex(ValueError, 'not in the disc inventory'):
            make(header, items=[i for i in ITEMS if i['file'] != 'p1'])

    def test_a_container_of_the_other_stage_is_not_taken(self):
        header, _ = recipe()
        with self.assertRaisesRegex(ValueError, 'not in the disc inventory'):
            make(header, items=[i for i in ITEMS if i['file'] != 'v1'])

    def test_the_fetch_numbers_are_applied_and_the_result_is_checked(self):
        header, _ = recipe()
        header['shaders'][0]['bindings'] = [{'address': 1, 'stride': 7}, {'address': 2, 'stride': 8}]
        with self.assertRaisesRegex(ValueError, 'shader made from the disc does not have the hash'):
            make(header)

    def test_an_instruction_the_runtime_drops_is_replaced_by_the_null_instruction(self):
        header, _ = recipe()
        nulled = VERTEX[:12] + packs.NULL_INSTRUCTION + VERTEX[24:]
        self.assertEqual(packs.NULL_INSTRUCTION, bytes.fromhex('c80000000000000002000000'))
        spec = dict(header['shaders'][0], nulled=[1], sha256=sha(nulled))
        self.assertEqual(packs.shader_code(spec, ITEMS, read_code, bind), nulled)
        both = VERTEX[:12] + packs.NULL_INSTRUCTION + packs.NULL_INSTRUCTION
        self.assertEqual(len(VERTEX), 26)                      # the stand-in's two fetch bytes follow two whole slots
        with self.assertRaisesRegex(ValueError, 'beyond the shader'):
            packs.shader_code(dict(spec, nulled=[2]), ITEMS, read_code, bind)
        with self.assertRaisesRegex(ValueError, 'shader made from the disc does not have the hash'):
            packs.shader_code(dict(spec, nulled=[0]), ITEMS, read_code, bind)

    def test_the_four_number_forms(self):
        header = {'text': [{'bytes': 's', 'style': 'hex', 'separator': ','}, '|', {'bytes': 's', 'style': 'hexu', 'separator': ','}, '|',
                           {'bytes': 's', 'style': 'hex2', 'separator': ','}, '|', {'words': 'm', 'style': 'hex8u', 'separator': ','}]}
        self.assertEqual(packs.render(header, {'s': bytes([1, 0xab])}, {'m': [0x07230203, 0x1f]}), '0x1,0xab|0x1u,0xABu|0x01,0xab|0x07230203u,0x0000001fu')

    def test_numbers_can_be_laid_out_so_many_to_a_line(self):
        piece = {'words': 'm', 'style': 'hex', 'separator': ',', 'per_line': 3}
        seven = packs.render({'text': ['{\n', piece, ',\n};']}, {}, {'m': [1, 2, 3, 4, 5, 6, 7]})
        self.assertEqual(seven, '{\n0x1,0x2,0x3,\n0x4,0x5,0x6,\n0x7,\n};')
        six = packs.render({'text': [piece]}, {}, {'m': [1, 2, 3, 4, 5, 6]})
        self.assertEqual(six, '0x1,0x2,0x3,\n0x4,0x5,0x6')                 # a full last line gets no empty line after it
        spaced = packs.render({'text': [dict(piece, separator=', ', per_line=2, style='hexu')]}, {}, {'m': [10, 11, 12]})
        self.assertEqual(spaced, '0xAu, 0xBu, \n0xCu')
        self.assertEqual(packs.render({'text': [dict(piece, per_line=1)]}, {}, {'m': [1, 2]}), '0x1,\n0x2')

    def test_names_are_changed_whole_and_all_at_once(self):
        text = 'float n293=1.0; float n294=n293*0x293u; float n2934=v.n293+n294; float xa1u=0xa1u+1.e5;'
        swapped = packs.renamed(text, [['n293', 'n294'], ['n294', 'n293']])
        self.assertEqual(swapped, 'float n294=1.0; float n293=n294*0x293u; float n2934=v.n293+n293; float xa1u=0xa1u+1.e5;')
        self.assertEqual(packs.renamed('float xa1u=0xa1u; e5=1.e5+2e5;', [['xa1u', 'q'], ['e5', 'r']]), 'float q=0xa1u; r=1.e5+2e5;')
        with self.assertRaisesRegex(ValueError, 'not in the translated shader'):
            packs.renamed(text, [['n293', 'n500'], ['n999', 'n501']])
        with self.assertRaisesRegex(ValueError, 'not in the translated shader'):
            packs.renamed('a=0xa1u;', [['xa1u', 'q']])                       # the tail of a number is not a name
        with self.assertRaisesRegex(ValueError, 'one name'):
            packs.renamed(text, [['n293', 'n294']])                          # n294 stays what it is: two variables would become one
        self.assertEqual(packs.renamed(text, [['n293', 'n300']]).count('n300'), 2)

    def test_a_translated_module_can_have_names_put_back(self):
        header, _ = recipe()
        module = header['modules'][0]
        glsl = translate('inline_shader_translation', 'translate_inline_pair', [VERTEX, PIXEL_RAW, [3, 4]], {'indexed': True})[0]
        self.assertIn('"indexed"', glsl)
        changed = glsl.replace('indexed', 'counted')
        flags = module['glslc']
        module.update(rename=[['indexed', 'counted']], sha256=sha(compile_glsl(changed, 'vert', flags)))
        shaders = {'s0': VERTEX, 's1': PIXEL_RAW}
        self.assertEqual(packs.module_words(module, shaders, translate, compile_glsl), words_of(changed, 'vert', flags))
        module['rename'] = [['absent', 'counted']]
        with self.assertRaisesRegex(ValueError, 'not in the translated shader'):
            packs.module_words(module, shaders, translate, compile_glsl)

    def test_a_hand_written_module_is_compiled_from_the_text_in_the_recipe(self):
        header, expected = recipe()
        text = '#version 450\nvoid main(){}\n'
        flags = ['--target-env=vulkan1.1', '-O0']
        before = len(CALLS)
        module = {'id': 'm1', 'source': text, 'kind': 'frag', 'glslc': flags, 'sha256': sha(compile_glsl(text, 'frag', flags))}
        self.assertEqual(packs.module_words(module, {}, translate, compile_glsl), words_of(text, 'frag', flags))
        self.assertEqual(len(CALLS), before)                   # no translator is asked
        with self.assertRaisesRegex(ValueError, 'SPIR-V module does not have the hash'):
            packs.module_words(dict(module, source=text + '// changed\n'), {}, translate, compile_glsl)

    def test_a_pixel_shader_is_taken_as_it_stands_and_checked(self):
        header, _ = recipe()
        with self.assertRaisesRegex(ValueError, 'shader made from the disc does not have the hash'):
            make(header, read=lambda row: FILES[row['file']] if row['file'] != 'p1' else PIXEL_RAW[:-1] + b'\x00')

    def test_differing_disc_shaders_of_one_canonical_form_are_refused(self):
        header, expected = recipe()
        twin = ITEMS + [{'canonical_sha256': CANON_P, 'stage': 'pixel', 'file': 'p2'}]
        files = dict(FILES, p2=PIXEL_RAW)
        self.assertEqual(make(header, items=twin, read=lambda row: files[row['file']]).decode(), expected)      # equal copies agree
        files['p2'] = PIXEL_RAW[:-1] + b'\x01'
        with self.assertRaisesRegex(ValueError, 'cannot choose'):
            make(header, items=twin, read=lambda row: files[row['file']])

    def test_a_module_with_another_hash_is_refused(self):
        header, _ = recipe()
        header['modules'][1]['glslc'] = ['--target-env=vulkan1.1']
        with self.assertRaisesRegex(ValueError, 'SPIR-V module does not have the hash'):
            make(header)

    def test_the_recipe_must_point_at_glsl_the_translator_returned(self):
        header, _ = recipe()
        header['modules'][0]['result'] = 2          # the proof record, not a text
        with self.assertRaisesRegex(ValueError, 'did not return the GLSL'):
            make(header)
        header['modules'][0]['result'] = 3
        with self.assertRaisesRegex(ValueError, 'did not return the GLSL'):
            make(header)

    def test_what_the_compiler_wrote_must_be_spirv(self):
        header, _ = recipe()
        bad = lambda text, kind, flags: b'\x00\x00\x00\x00' + compile_glsl(text, kind, flags)[4:]
        header['modules'][0]['sha256'] = sha(bad(translate('inline_shader_translation', 'translate_inline_pair', [VERTEX, PIXEL_RAW, [3, 4]], {'indexed': True})[0], 'vert', header['modules'][0]['glslc']))
        with self.assertRaisesRegex(ValueError, 'did not write SPIR-V'):
            make(header, comp=lambda text, kind, flags: bad(text, kind, flags) if kind == 'vert' else compile_glsl(text, kind, flags))
        odd = lambda text, kind, flags: compile_glsl(text, kind, flags) + b'\x01'
        with self.assertRaisesRegex(ValueError, 'SPIR-V module does not have the hash'):
            make(recipe()[0], comp=odd)
        # also when the recipe itself names the hash of such a file: a refusal, not a crash while reading words
        header, _ = recipe()
        glsl = translate('inline_shader_translation', 'translate_inline_pair', [VERTEX, PIXEL_RAW, [3, 4]], {'indexed': True})
        header['modules'][0]['sha256'] = sha(odd(glsl[0], 'vert', header['modules'][0]['glslc']))
        with self.assertRaises(ValueError):
            make(header, comp=lambda text, kind, flags: odd(text, kind, flags) if kind == 'vert' else compile_glsl(text, kind, flags))

    def test_a_header_that_differs_from_the_named_one_is_refused(self):
        header, _ = recipe()
        header['text'][0] = '#pragma once\nQ p{{'
        with self.assertRaisesRegex(ValueError, 'is not the header the recipe names'):
            make(header)
        header, expected = recipe()
        header['bytes'] += 1
        with self.assertRaisesRegex(ValueError, 'is not the header the recipe names'):
            make(header)


class Recipes(unittest.TestCase):
    def table(self, change=None):
        header, _ = recipe()
        if change:
            change(header)
        return {'schema': packs.SCHEMA, 'headers': [header]}

    def test_a_good_table_passes(self):
        self.assertEqual(len(packs.check_recipes(self.table())), 1)

    def refused(self, change):
        with self.assertRaises(ValueError):
            packs.check_recipes(self.table(change))

    def test_paths(self):
        for path in ('/abs/x.h', 'a/../x.h', 'a\\x.h', 'a/x.txt', ''):
            self.refused(lambda h, path=path: h.update(path=path))
        table = self.table()
        table['headers'].append(copy.deepcopy(table['headers'][0]))
        with self.assertRaises(ValueError):
            packs.check_recipes(table)

    def test_schema_and_expected_identity(self):
        with self.assertRaises(ValueError):
            packs.check_recipes(dict(self.table(), schema='native-packs/0'))
        self.refused(lambda h: h.update(sha256='zz'))
        self.refused(lambda h: h.update(bytes=0))

    def test_only_translate_functions_of_named_modules(self):
        self.refused(lambda h: h['modules'][0]['call'].update(function='system'))
        self.refused(lambda h: h['modules'][0]['call'].update(module='os.path'))
        self.refused(lambda h: h['modules'][0]['call'].update(arguments='$s0'))
        self.refused(lambda h: h['modules'][0]['call'].update(arguments={'x': 1}))
        self.refused(lambda h: h['modules'][0]['call'].update(arguments=['$s9']))

    def test_compiler_flags(self):
        for flags in (['-o', 'x'], ['-ofile'], ['-I/tmp'], ['--foo=bar'], ['plain']):
            self.refused(lambda h, flags=flags: h['modules'][0].update(glslc=flags))
        packs.check_recipes(self.table(lambda h: h['modules'][0].update(glslc=['--target-env=vulkan1.1', '-O'])))

    def test_shader_entries(self):
        self.refused(lambda h: h['shaders'][1].update(stage='geometry'))
        self.refused(lambda h: h['shaders'][1].update(bindings=[]))           # fetch numbers on a pixel shader
        self.refused(lambda h: h['shaders'].append(dict(h['shaders'][0])))    # the same id twice, everything else in order
        self.refused(lambda h: h['shaders'][0].update(canonical_sha256='1234'))

    def test_nulled_slots(self):
        packs.check_recipes(self.table(lambda h: h['shaders'][0].update(nulled=[0, 1])))
        for bad in ([1, 0], [1, 1], [-1], ['1'], 1):
            self.refused(lambda h, bad=bad: h['shaders'][0].update(nulled=bad))
        self.refused(lambda h: h['shaders'][1].update(nulled=[0]))            # a pixel shader has none

    def test_hand_written_modules(self):
        def own(h, **more):
            m = h['modules'][1]
            del m['call'], m['result']
            m['source'] = '#version 450\nvoid main(){}\n'
            m.update(more)
        packs.check_recipes(self.table(own))
        self.refused(lambda h: own(h, source='void main(){}'))                # not a whole GLSL text
        self.refused(lambda h: own(h, source=7))
        self.refused(lambda h: own(h, result=0))                              # a result index belongs to a translator call
        self.refused(lambda h: h['modules'][1].update(source='#version 450\n'))  # both a call and a text
        self.refused(lambda h: h['modules'][1].pop('call'))                    # neither

    def test_module_entries(self):
        self.refused(lambda h: h['modules'][0].update(kind='geom'))
        self.refused(lambda h: h['modules'][0].update(result=-1))
        self.refused(lambda h: h['modules'].append(dict(h['modules'][0])))    # the same id twice, everything else in order
        self.refused(lambda h: h['modules'][0].update(sha256=None))

    def test_renames(self):
        ok = lambda h: h['modules'][0].update(rename=[['a', 'b'], ['b', 'a']])
        self.assertEqual(len(packs.check_recipes(self.table(ok))), 1)
        self.refused(lambda h: h['modules'][0].update(rename=[]))                              # named and empty
        self.refused(lambda h: h['modules'][0].update(rename='a=b'))
        self.refused(lambda h: h['modules'][0].update(rename=7))
        self.refused(lambda h: h['modules'][0].update(rename=[['a', 'b', 'c']]))
        self.refused(lambda h: h['modules'][0].update(rename=['ab']))                          # two letters are not a pair of names
        self.refused(lambda h: h['modules'][0].update(rename=[['a', 'a']]))
        self.refused(lambda h: h['modules'][0].update(rename=[['a', 'b'], ['a', 'c']]))        # one name sent two ways
        self.refused(lambda h: h['modules'][0].update(rename=[['a', 'c'], ['b', 'c']]))        # two names made one
        self.refused(lambda h: h['modules'][0].update(rename=[['a', 'b c']]))
        self.refused(lambda h: h['modules'][0].update(rename=[['a', '1b']]))
        self.refused(lambda h: h['modules'][0].update(rename=[['a', 7]]))
        text = '#version 450\nvoid main(){}\n'

        def hand_written(h):
            h['modules'][1] = {'id': 'm1', 'source': text, 'kind': 'frag', 'glslc': [], 'sha256': 'c' * 64, 'rename': [['main', 'other']]}
        self.refused(hand_written)                                                             # the recipe holds that text itself

    def test_revisions(self):
        ok = lambda h: h['modules'][0].update(call=dict(h['modules'][0]['call'], revision='r20260910'))
        self.assertEqual(len(packs.check_recipes(self.table(ok))), 1)
        for bad in ('', 'R1', '../r1', 'a/b', 'a.b', '-a', 'x' * 41, 7, None, ['r1']):
            self.refused(lambda h: h['modules'][0].update(call=dict(h['modules'][0]['call'], revision=bad)))
        self.assertEqual(len(packs.check_recipes(self.table(lambda h: h['modules'][0].update(call=dict(h['modules'][0]['call'], revision='x' * 40))))), 1)

    def test_numbers_per_line(self):
        ok = lambda h: h['text'].__setitem__(1, {'bytes': 's0', 'style': 'hex', 'separator': ',', 'per_line': 12})
        self.assertEqual(len(packs.check_recipes(self.table(ok))), 1)
        for bad in (0, -1, '12', 1.0, True, None):
            self.refused(lambda h: h['text'].__setitem__(1, {'bytes': 's0', 'style': 'hex', 'separator': ',', 'per_line': bad}))

    def test_text(self):
        self.refused(lambda h: h['text'].__setitem__(1, {'bytes': 's0', 'style': 'octal', 'separator': ','}))
        self.refused(lambda h: h['text'].__setitem__(1, {'bytes': 's0', 'style': 'hex', 'separator': ';'}))
        self.refused(lambda h: h['text'].__setitem__(1, {'bytes': 'm0', 'style': 'hex', 'separator': ','}))    # a module as bytes
        self.refused(lambda h: h['text'].__setitem__(5, {'words': 's0', 'style': 'hex', 'separator': ','}))    # a shader as words
        packs.check_recipes(self.table(lambda h: h['text'].__setitem__(1, 'no shader bytes here')))           # s0 is still what the modules are translated from
        self.refused(lambda h: h['shaders'].append(dict(h['shaders'][1], id='s2')))                           # a shader neither in the text nor translated from
        self.refused(lambda h: h['text'].__setitem__(5, 'no module here'))                                    # a module that is described and not written
        self.refused(lambda h: h['text'].append({'bytes': 'm0', 'style': 'hex', 'separator': ','}))           # every id used, one of them as the wrong kind
        self.refused(lambda h: h['text'].append({'words': 's1', 'style': 'hex', 'separator': ','}))
        self.refused(lambda h: h.update(text=[]))
        plain = {'schema': packs.SCHEMA, 'headers': [{'path': 'a/b.h', 'bytes': 3, 'sha256': sha(b'abc'), 'text': ['abc']}]}
        self.assertEqual(len(packs.check_recipes(plain)), 1)
        plain['headers'][0]['text'] = 'abc'                                   # not a list of pieces
        with self.assertRaises(ValueError):
            packs.check_recipes(plain)


class Build(unittest.TestCase):
    class Stand:
        read_code = staticmethod(read_code)
        bind = staticmethod(bind)
        translate = staticmethod(translate)
        compile_glsl = staticmethod(compile_glsl)

    def test_headers_are_written_under_their_paths_with_a_report(self):
        header, expected = recipe()
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'out'
            report = packs.build([header], ITEMS, self.Stand, out)
            self.assertEqual(report['status'], 'complete')
            self.assertEqual((out / 'packs/one/native_one_shader_pack.h').read_text(), expected)
            self.assertEqual(json.loads((out / 'report.json').read_text())['headers'][header['path']]['sha256'], header['sha256'])

    def test_a_header_that_cannot_be_made_is_not_written_and_fails_the_build(self):
        good, _ = recipe()
        bad, _ = recipe()
        bad['path'] = 'packs/two/native_two_shader_pack.h'
        bad['text'][0] = 'changed'
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'out'
            report = packs.build([good, bad], ITEMS, self.Stand, out)
            self.assertEqual(report['status'], 'differs')
            self.assertEqual(report['headers'][bad['path']]['status'], 'refused')
            self.assertFalse((out / bad['path']).exists())
            self.assertTrue((out / good['path']).exists())

    def test_only_selects_headers(self):
        good, _ = recipe()
        other, _ = recipe()
        other['path'] = 'packs/two/native_two_shader_pack.h'
        with tempfile.TemporaryDirectory() as tmp:
            report = packs.build([good, other], ITEMS, self.Stand, Path(tmp) / 'out', only='two')
            self.assertEqual(list(report['headers']), ['packs/two/native_two_shader_pack.h'])

    def test_an_existing_output_folder_is_not_written_into(self):
        header, _ = recipe()
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(OSError):
                packs.build([header], ITEMS, self.Stand, Path(tmp))


class RealTools(unittest.TestCase):
    def test_a_translator_outside_the_translator_folder_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            tools = packs.Tools(Path(tmp), Path('/nonexistent/glslc'))
            self.addCleanup(tools.tmp.cleanup)
            with self.assertRaisesRegex(ValueError, 'not in tools/rpf7_inventory'):
                tools.translate('os', 'translate_anything', [], {})

    def test_a_container_outside_the_inventory_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'inv').mkdir()
            (Path(tmp) / 'secret.bin').write_bytes(b'x' * 64)
            tools = packs.Tools(Path(tmp) / 'inv', Path('/nonexistent/glslc'))
            self.addCleanup(tools.tmp.cleanup)
            with self.assertRaisesRegex(ValueError, 'inventory path refused'):
                tools.read_code({'file': '../secret.bin', 'stage': 'pixel'})


class Revisions(unittest.TestCase):
    """An earlier revision of a translator: its own folder, its own process, nothing of today's."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.today = Path(tmp.name).resolve() / 'translators'
        old = self.today / 'history' / 'r1'
        old.mkdir(parents=True)
        (self.today / 'demo_pack_translation.py').write_text(
            "import demo_pack_helper\ndef translate_demo(code, extra, **options):\n    return ('today ' + demo_pack_helper.tag(code, extra, options), 7)\n")
        (self.today / 'demo_pack_helper.py').write_text("def tag(code, extra, options):\n    return 'helper of today'\n")
        (self.today / 'demo_pack_only_today.py').write_text("VALUE = 'today'\n")
        (old / 'demo_pack_translation.py').write_text(
            "import demo_pack_helper\n"
            "def translate_demo(code, extra, **options):\n    return ('earlier ' + demo_pack_helper.tag(code, extra, options), {'not': 'text'}, 'second')\n"
            "def translate_needs_today(code):\n    import demo_pack_only_today\n    return (demo_pack_only_today.VALUE,)\n"
            "def translate_fails(code):\n    raise RuntimeError('the earlier translator refuses this shader')\n"
            "def translate_one(code):\n    return 'one text'\n")
        (old / 'demo_pack_helper.py').write_text(
            "def tag(code, extra, options):\n    return 'helper of then|%s|%s|%s|%s' % (type(code).__name__, code.hex(), type(extra).__name__, sorted((k, type(v).__name__, v) for k, v in options.items()))\n")
        before, path = packs.TRANSLATORS, list(sys.path)
        packs.TRANSLATORS = self.today

        def restore():
            packs.TRANSLATORS = before
            sys.path[:] = path
            for name in [n for n in sys.modules if n.startswith('demo_pack_')]:
                del sys.modules[name]
        self.addCleanup(restore)
        self.tools = packs.Tools(self.today, Path('/nonexistent/glslc'))
        self.addCleanup(self.tools.tmp.cleanup)
        self.then = ('earlier helper of then|bytes|0102|list|' + repr([('formats', 'tuple', (0, None)), ('mask', 'int', 7)]), None, 'second')

    def earlier(self):
        return self.tools.translate('demo_pack_translation', 'translate_demo', [b'\x01\x02', [3, 4]], {'formats': (0, None), 'mask': 7}, 'r1')

    def now(self):
        return self.tools.translate('demo_pack_translation', 'translate_demo', [b'\x01\x02', [3, 4]], {'formats': (0, None), 'mask': 7})

    def test_the_revision_runs_with_its_own_modules_and_gets_bytes_and_tuples(self):
        self.assertEqual(self.earlier(), self.then)                        # its own helper; what is not text comes back as None
        self.assertEqual(self.now(), ('today helper of today', 7))        # the same call without a revision is today's, in this process
        self.assertEqual(self.earlier(), self.then)

    def test_todays_modules_loaded_first_do_not_reach_the_revision(self):
        self.assertEqual(self.now(), ('today helper of today', 7))
        self.assertIn('demo_pack_helper', sys.modules)
        self.assertEqual(self.earlier(), self.then)

    def test_a_single_text_comes_back_as_one_result(self):
        self.assertEqual(self.tools.translate('demo_pack_translation', 'translate_one', [b'\x00'], {}, 'r1'), ('one text',))

    def test_a_module_the_revision_lacks_is_refused_and_not_taken_from_today(self):
        with self.assertRaisesRegex(ValueError, 'not in tools/rpf7_inventory'):
            self.tools.translate('demo_pack_only_today', 'translate_anything', [], {}, 'r1')
        with self.assertRaisesRegex(ValueError, 'not in tools/rpf7_inventory'):
            self.tools.translate('demo_pack_translation', 'translate_demo', [b'\x01', []], {}, 'r2')      # no such revision
        saved = os.environ.get('PYTHONPATH')
        os.environ['PYTHONPATH'] = str(self.today)
        try:
            with self.assertRaisesRegex(ValueError, '(?s)earlier translator revision failed.*demo_pack_only_today'):
                self.tools.translate('demo_pack_translation', 'translate_needs_today', [b'\x01'], {}, 'r1')
        finally:
            if saved is None:
                del os.environ['PYTHONPATH']
            else:
                os.environ['PYTHONPATH'] = saved

    def test_a_revision_that_fails_is_an_error_with_its_reason(self):
        with self.assertRaisesRegex(ValueError, '(?s)earlier translator revision failed.*refuses this shader'):
            self.tools.translate('demo_pack_translation', 'translate_fails', [b'\x01'], {}, 'r1')


class CommandLine(unittest.TestCase):
    def run_tool(self, recipes, inventory):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'r.json').write_text(json.dumps(recipes))
            (tmp / 'inv').mkdir()
            if inventory is not None:
                (tmp / 'inv/inventory.json').write_text(json.dumps(inventory))
            return subprocess.run([sys.executable, '-B', str(TOOL), '--inventory', str(tmp / 'inv'), '--recipes', str(tmp / 'r.json'),
                                   '--glslc', str(tmp / 'no-glslc'), '--output', str(tmp / 'out')], capture_output=True, text=True)

    def test_refusals_end_with_exit_2(self):
        header, _ = recipe()
        good = {'schema': packs.SCHEMA, 'headers': [header]}
        for recipes, inventory, why in (({'schema': 'x', 'headers': []}, {'containers': ITEMS}, 'recipe file refused'), (good, None, 'inventory.json'),
                                        (good, {'containers': []}, 'lists no shader containers'), (good, {'containers': ITEMS}, 'glslc not found')):
            r = self.run_tool(recipes, inventory)
            self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
            self.assertIn('NATIVE_PACKS_REFUSED', r.stderr)
            self.assertIn(why, r.stderr)

    def main_with_stand_ins(self, headers):
        """main() in this process, with the stand-in tools in place of the translator, the compiler and the reader."""
        class Stand(Build.Stand):
            def __init__(self, inventory, glslc):
                pass
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'r.json').write_text(json.dumps({'schema': packs.SCHEMA, 'headers': headers}))
            (tmp / 'inv').mkdir()
            (tmp / 'inv/inventory.json').write_text(json.dumps({'containers': ITEMS}))
            (tmp / 'glslc').write_text('')
            real, argv = packs.Tools, sys.argv
            packs.Tools, sys.argv = Stand, ['x', '--inventory', str(tmp / 'inv'), '--recipes', str(tmp / 'r.json'), '--glslc', str(tmp / 'glslc'), '--output', str(tmp / 'out')]
            try:
                return packs.main()
            finally:
                packs.Tools, sys.argv = real, argv

    def test_exit_0_when_every_header_is_made_and_1_when_one_is_not(self):
        good, _ = recipe()
        bad, _ = recipe()
        bad['path'] = 'packs/two/native_two_shader_pack.h'
        bad['sha256'] = 'c' * 64
        self.assertEqual(self.main_with_stand_ins([good]), 0)
        self.assertEqual(self.main_with_stand_ins([good, bad]), 1)


if __name__ == '__main__':
    unittest.main()
