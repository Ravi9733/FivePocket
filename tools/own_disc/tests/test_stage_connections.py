#!/usr/bin/env python3
"""Each heavy stage must consume what the stage before it produced in this run, not a file from an earlier build.
Synthetic fixtures only. STAGE_TOOL names the stage runner (the mutant runner points it at changed copies).
From the builder-rehearsal lane's tests of 2026-10-07, with the portable link words added."""
import importlib.util
import json
import os
from pathlib import Path
import re
import tempfile
import unittest

TARGET = Path(os.environ.get('STAGE_TOOL', Path(__file__).resolve().parents[1] / 'compile_rehearsal.py'))
spec = importlib.util.spec_from_file_location('under_test', TARGET)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
ID = 'dc6659d7f4d87473d1ea10ed2fd7e99ac20ddcfe'


class Connections(unittest.TestCase):
    def test_generator_consumes_extracted_executable(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'old').write_text('old fixture')
            (root / 'new').write_text('new fixture')
            text = 'file_path = "old"\nswitch_table_file_path = "switch"\nflags = false\n'
            result = m.generator_config(text, root, root / 'new')
            path = json.loads(re.search(r'^file_path = (.*)$', result, re.M)[1])
            self.assertEqual(Path(path).read_text(), 'new fixture')
            self.assertIn('flags = false', result)
            self.assertIn(str((root / 'switch').resolve()), result)

    def test_the_generator_and_its_header_are_this_hosts_when_named(self):
        from unittest import mock
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            (root / 'local-analysis/regen/flags-off').mkdir(parents=True)
            (root / 'local-analysis/regen/flags-off/recomp.toml').write_text('file_path = "old"\nswitch_table_file_path = "switch"\n')
            for work, named in ((root / 'w1', {}), (root / 'w2', {'generator': root / 'host/XenonRecomp.exe', 'context': root / 'host/ppc_context.h'})):
                seen = []
                with mock.patch.object(m.subprocess, 'run', side_effect=lambda command, **k: seen.append(command)):
                    m.run_codegen(root, work, **named)
                command, = seen
                self.assertEqual(command[command.index('--generator') + 1], str(named.get('generator', root / 'local-analysis/xenonrecomp-build/build/XenonRecomp/XenonRecomp')))
                self.assertEqual(command[command.index('--context') + 1], str(named.get('context', root / 'local-analysis/xenonrecomp-build/src/XenonUtils/ppc_context.h')))
                self.assertEqual(command[command.index('--work') + 1], str(work / 'codegen'))
                self.assertIn(str(work / 'input/default.xex'), (work / 'codegen-input/recomp.toml').read_text())
            # the command line hands both on
            seen = []
            argv = ['compile_rehearsal.py', 'codegen', '--repo', str(root), '--work', str(root / 'local-analysis/builder/w3'),
                    '--generator', str(root / 'host/XenonRecomp.exe'), '--context', str(root / 'host/ppc_context.h')]
            with mock.patch.object(m.sys, 'argv', argv), mock.patch.object(m, 'run_codegen', side_effect=lambda *a: seen.append(a)), \
                 mock.patch.object(m, 'run_recipe', side_effect=AssertionError('the generator stage was asked for')):
                m.main()
            self.assertEqual(seen, [(root, root / 'local-analysis/builder/w3', root / 'host/XenonRecomp.exe', root / 'host/ppc_context.h')])
            seen = []
            with mock.patch.object(m.sys, 'argv', argv[:6]), mock.patch.object(m, 'run_codegen', side_effect=lambda *a: seen.append(a)):
                m.main()
            self.assertEqual(seen, [(root, root / 'local-analysis/builder/w3', None, None)])
            seen = []
            with mock.patch.object(m.sys, 'argv', ['compile_rehearsal.py', 'recipe'] + argv[2:6]), mock.patch.object(m, 'run_recipe', side_effect=lambda *a: seen.append(a)), \
                 mock.patch.object(m, 'run_codegen', side_effect=AssertionError('the recipe stage was asked for')):
                m.main()
            self.assertEqual(seen, [(root, root / 'local-analysis/builder/w3', None)])
            seen = []
            with mock.patch.object(m.sys, 'argv', ['compile_rehearsal.py', 'recipe'] + argv[2:6] + ['--recipe', str(root / 'other/recipe.json')]), \
                 mock.patch.object(m, 'run_recipe', side_effect=lambda *a: seen.append(a)):
                m.main()
            self.assertEqual(seen, [(root, root / 'local-analysis/builder/w3', root / 'other/recipe.json')])
            self.assertEqual(m.recipe_file(root), root / 'local-analysis/recipes/recipe-v455.json')
            self.assertEqual(m.recipe_file(root, root / 'other/recipe.json'), root / 'other/recipe.json')
            # the stage reads the record it is given: a named file that is not a recipe fails in that file, not in the default
            (root / 'other').mkdir(); (root / 'other/recipe.json').write_text('{"not": "a recipe"}')
            (root / 'tools/fp_codegen').mkdir(parents=True); (root / 'tools/fp_codegen/recipe_build.py').write_text('')
            with self.assertRaises(KeyError):
                m.run_recipe(root, root / 'local-analysis/builder/w4', root / 'other/recipe.json')
            with self.assertRaises(FileNotFoundError):
                m.run_recipe(root, root / 'local-analysis/builder/w4')

    def test_the_recipe_tool_is_asked_to_check_against_recorded_hashes_only(self):
        a = m.recipe_build_arguments(Path('/w/recipe/connected-recipe.json'), Path('/w'), Path('/root'))
        self.assertTrue(a.expect_only)                       # the earlier build's chunk files are not read
        self.assertEqual((a.cmd, a.recipe, a.out, a.data_root, a.write, a.chunks, a.stitch_cache),
                         ('build', '/w/recipe/connected-recipe.json', '/w/recipe/chunks', '/root', 'all', None, None))

    def test_recipe_reads_current_producers(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            recipe = {'target': 'old', 'corpus': 'old-corpus', 'inputs': {'image': {'path': 'old-image', 'sha256': 'a'}},
                      'generator_base': {'dir': 'old-gen'}, 'chunks': {
                          '0': {'base': {'layer': 'validated-ppc', 'path': 'old/ppc_recomp.0.cpp'}},
                          '1': {'base': {'layer': 'generator+passes', 'path': 'old/ppc_recomp.1.cpp',
                                         'corpus': {'path': 'old-corpus/ppc_recomp.1.cpp'}}}}}
            connected = m.connect_recipe(recipe, work, work)
            for name, base in [('0', 'recipe/corpus'), ('1', 'codegen/ppc')]:
                path = work / base / ('ppc_recomp.' + name + '.cpp')
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('current producer ' + name)
                self.assertEqual(Path(connected['chunks'][name]['base']['path']).read_text(), 'current producer ' + name)
            self.assertEqual(connected['inputs']['image']['path'], str(work / 'decode/default.image.bin'))
            self.assertEqual(connected['corpus'], str(work / 'recipe/corpus'))
            self.assertEqual(recipe['target'], 'old')

    def test_compiler_consumes_recipe_output_and_caps_jobs(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            f = {'name': 'ppc_recomp.0.cpp', 'ppc_chunk': 0, 'source': 'old-corpus/file'}
            original = {'build_jobs': 5, 'files': [f, {'name': 'host.cpp', 'origin': 'host.cpp'}]}
            result = m.connected_spec(original, work, work, work)
            (work / f['name']).write_text('rebuilt fixture')
            self.assertEqual(Path(result['files'][0]['source']).read_text(), 'rebuilt fixture')
            self.assertEqual(result['build_jobs'], 4)
            self.assertEqual(original['files'][0]['source'], 'old-corpus/file')

    def test_bad_producer_hash_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'fixture'
            path.write_bytes(b'fixture')
            m.require_hash(path, m.sha(path))
            with self.assertRaises(ValueError):
                m.require_hash(path, '0' * 64)

    def test_portable_link_drops_debug_sections_and_pins_the_build_id(self):
        self.assertEqual(m.portable_link_words(ID), ['-Wl,--strip-debug', '-Wl,--build-id=0x' + ID])

    def test_a_build_id_of_another_shape_is_refused(self):
        for bad in ('', None, ID[:-1], ID + '0', ID.upper(), 'g' * 40, '0x' + ID[2:]):
            with self.assertRaises(ValueError, msg=repr(bad)):
                m.portable_link_words(bad)

    def test_link_words_follow_the_attempts_own_and_leave_the_original_alone(self):
        original = {'files': [], 'link_extra': ['-Wl,--symbol-ordering-file=order.txt']}
        words = m.portable_link_words(ID)
        result = m.connected_spec(original, Path('.'), Path('.'), Path('.'), words)
        self.assertEqual(result['link_extra'], ['-Wl,--symbol-ordering-file=order.txt'] + words)
        self.assertEqual(original['link_extra'], ['-Wl,--symbol-ordering-file=order.txt'])
        self.assertEqual(m.connected_spec({'files': []}, Path('.'), Path('.'), Path('.'))['link_extra'], [])

    def test_pack_folders_made_in_this_run_go_first_on_the_include_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            root, packs = Path(tmp) / 'repo', Path(tmp) / 'packs'
            (packs / 'a/pack-v1').mkdir(parents=True)
            (packs / 'b').mkdir()
            (packs / 'd').write_text('a file, not a folder')
            private = str(root / 'local-analysis')
            command = ['clang++', '-I' + private + '/a/pack-v1', '-I' + private + '/c/not-made', '-I/usr/include',
                       '-I' + str(root / 'local-analysis-other/a/pack-v1'), '-I' + private + '/a/pack-v1', '-DX=1',
                       '-I' + private + '/d', '-I' + private + '/b', '-iquote', private + '/b']
            self.assertEqual(m.pack_include_words(command, root, packs), ['-I' + str(packs / 'a/pack-v1'), '-I' + str(packs / 'b')])
            self.assertEqual(m.pack_include_words(['clang++', '-DX=1'], root, packs), [])

    def test_a_runtime_file_gets_its_pack_folders_behind_its_own_words_and_game_code_gets_none(self):
        chunk = {'name': 'ppc_recomp.0.cpp', 'ppc_chunk': 0, 'source': 'old-corpus/file'}
        original = {'files': [chunk, {'name': 'hook.cpp', 'origin': 'x/hook.cpp', 'defines': ['-DOWN=1']},
                              {'name': 'other.cpp', 'origin': 'x/other.cpp'}, {'name': 'plain.cpp', 'origin': 'x/plain.cpp', 'defines': ['-DKEPT']}]}
        first = {'hook.cpp': ['-I/packs/a', '-I/packs/b'], 'other.cpp': ['-I/packs/a'], 'ppc_recomp.0.cpp': ['-I/packs/never']}
        result = m.connected_spec(original, Path('.'), Path('.'), Path('.'), (), first)
        by = {f['name']: f for f in result['files']}
        self.assertEqual(by['hook.cpp']['defines'], ['-DOWN=1', '-I/packs/a', '-I/packs/b'])
        self.assertEqual(by['other.cpp']['defines'], ['-I/packs/a'])
        self.assertEqual(by['plain.cpp']['defines'], ['-DKEPT'])
        self.assertNotIn('defines', by['ppc_recomp.0.cpp'])
        self.assertEqual(original['files'][1]['defines'], ['-DOWN=1'])
        self.assertNotIn('defines', m.connected_spec(original, Path('.'), Path('.'), Path('.'))['files'][2])

    def test_game_code_chunks_find_this_runs_generator_headers_first(self):
        original = {'ppc_defines': ['-DOWN=1', '-iquote', '/helpers'], 'files': []}
        result = m.connected_spec(original, Path('.'), Path('.'), Path('.'), (), None, Path('/work/compile/generated-headers'))
        self.assertEqual(result['ppc_defines'], ['-DOWN=1', '-iquote', '/helpers', '-iquote', '/work/compile/generated-headers'])
        self.assertEqual(original['ppc_defines'], ['-DOWN=1', '-iquote', '/helpers'])
        self.assertEqual(m.connected_spec({'files': []}, Path('.'), Path('.'), Path('.'), (), None, Path('/g'))['ppc_defines'], ['-iquote', '/g'])
        self.assertNotIn('ppc_defines', m.connected_spec({'files': []}, Path('.'), Path('.'), Path('.')))

    def test_work_must_lie_in_the_ignored_builder_folder(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            m.check_work(root, root / 'local-analysis/builder/run-1/work')
            for bad in (root / 'local-analysis/attempts/v507', root / 'tools/work', root / 'local-analysis/builder/../attempts/x'):
                with self.assertRaises(SystemExit, msg=str(bad)):
                    m.check_work(root, bad)


if __name__ == '__main__':
    unittest.main()
