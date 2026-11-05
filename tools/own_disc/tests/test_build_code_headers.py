#!/usr/bin/env python3
"""Tests of build_code_headers.py on a made-up chunk of generated code."""
import copy
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = Path(os.environ.get('BUILD_CODE_HEADERS', HERE.parent / 'build_code_headers.py'))   # the mutant runner points this at a changed copy
spec = importlib.util.spec_from_file_location('build_code_headers', TOOL)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
sha = lambda text: hashlib.sha256(text.encode()).hexdigest()

FIRST = ['PPC_FUNC_IMPL(__imp__sub_82000010) {', '\tPPC_FUNC_PROLOGUE();', '\t// li r3,0', '\tctx.r3.s64 = 0;',
         '\t__imp__sub_82000020(ctx, base);', '\tif (ctx.cr6.eq) goto loc_82000018;', '\treturn;', '}', '']
CHUNK = '\n'.join(['#include "ppc_recomp_shared.h"', ''] + FIRST + ['PPC_WEAK_FUNC(sub_82000010) {', '\t__imp__sub_82000010(ctx, base);', '}', '',
                  'PPC_FUNC_IMPL(__imp__sub_82000020) {', '\t__imp__sub_82000010(ctx, base);', '}', '']) + '\n'


def part(**change):
    p = {'function': '82000010', 'chunk': 7, 'first_line': 'PPC_FUNC(sub_82000010) {', 'direct_calls': True,
         'edits': [{'at': 1, 'remove': 0, 'removed_sha256': sha(''), 'insert': ['\tconst bool watch=watching(ctx.r3.u32);']},
                   {'at': 5, 'remove': 1, 'removed_sha256': sha(FIRST[5]), 'insert': ['\tif (ctx.cr6.eq) { if(watch) note(); goto loc_82000018; }', '\t__imp__sub_82000020(ctx, base); // ours, kept as written']},
                   {'at': 8, 'remove': 1, 'removed_sha256': sha(''), 'insert': []}]}
    p.update(change)
    return p


EXPECTED = ['PPC_FUNC(sub_82000010) {', '\tconst bool watch=watching(ctx.r3.u32);', '\tPPC_FUNC_PROLOGUE();', '\t// li r3,0', '\tctx.r3.s64 = 0;',
            '\tsub_82000020(ctx, base);', '\tif (ctx.cr6.eq) { if(watch) note(); goto loc_82000018; }',
            '\t__imp__sub_82000020(ctx, base); // ours, kept as written', '\treturn;', '}']


def header(**change):
    text = 'PPC_EXTERN_FUNC(sub_82000020);\n' + '\n'.join(EXPECTED) + '\n'
    h = {'path': 'old/folder/hooked.h', 'bytes': len(text.encode()), 'sha256': sha(text), 'parts': ['PPC_EXTERN_FUNC(sub_82000020);\n', part(), '\n']}
    h.update(change)
    return h, text


class Function(unittest.TestCase):
    def test_a_function_runs_up_to_the_line_before_the_next_one(self):
        self.assertEqual(tool.function_lines(CHUNK, '82000010'), FIRST)
        self.assertEqual(tool.function_lines(CHUNK, '82000020'), ['PPC_FUNC_IMPL(__imp__sub_82000020) {', '\t__imp__sub_82000010(ctx, base);', '}', '', ''])

    def test_a_function_must_be_there_once(self):
        with self.assertRaisesRegex(ValueError, 'not once'):
            tool.function_lines(CHUNK, '82000030')
        with self.assertRaisesRegex(ValueError, 'not once'):
            tool.function_lines(CHUNK + CHUNK, '82000010')

    def test_edits_first_line_and_public_names(self):
        self.assertEqual(tool.function_text(part(), CHUNK), EXPECTED)

    def test_without_the_renaming_the_calls_stay_as_generated(self):
        lines = tool.function_text(part(direct_calls=False), CHUNK)
        self.assertEqual(lines[5], '\t__imp__sub_82000020(ctx, base);')
        self.assertEqual(lines[0], 'PPC_FUNC(sub_82000010) {')

    def test_a_stretch_that_is_not_what_the_edit_was_made_for_is_refused(self):
        p = part()
        p['edits'][1]['removed_sha256'] = sha('\tif (ctx.cr6.ne) goto loc_82000018;')
        with self.assertRaisesRegex(ValueError, 'not the lines it was made for'):
            tool.function_text(p, CHUNK)
        p = part()
        p['edits'][0]['removed_sha256'] = sha('x')              # also where nothing is removed: the hash of nothing is asked for
        with self.assertRaisesRegex(ValueError, 'not the lines it was made for'):
            tool.function_text(p, CHUNK)

    def test_an_edit_beyond_the_function_is_refused(self):
        p = part()
        p['edits'][2] = {'at': 9, 'remove': 1, 'removed_sha256': sha(''), 'insert': []}
        with self.assertRaisesRegex(ValueError, 'beyond the function'):
            tool.function_text(p, CHUNK)

    def test_the_hash_is_of_the_lines_as_generated_not_as_renamed(self):
        p = part(edits=[{'at': 4, 'remove': 1, 'removed_sha256': sha(FIRST[4]), 'insert': ['\tours();']}])
        self.assertEqual(tool.function_text(p, CHUNK)[4], '\tours();')
        self.assertEqual(tool.function_text(p, CHUNK)[5:], FIRST[5:])          # what follows the last edit stays
        p['edits'][0]['removed_sha256'] = sha('\tsub_82000020(ctx, base);')
        with self.assertRaises(ValueError):
            tool.function_text(p, CHUNK)


class Header(unittest.TestCase):
    def test_literal_pieces_and_functions_in_order(self):
        h, text = header()
        self.assertEqual(tool.build_header(h, lambda n: {7: CHUNK}[n]).decode(), text)

    def test_a_header_of_text_only(self):
        text = '#pragma once\n#include "../other/hooked.h"\n'
        h = {'path': 'a/pass.h', 'bytes': len(text), 'sha256': sha(text), 'parts': [text]}
        self.assertEqual(tool.build_header(h, lambda n: self.fail('no chunk is read')).decode(), text)

    def test_a_header_that_differs_is_refused(self):
        h, _ = header()
        h['parts'][0] = 'PPC_EXTERN_FUNC(sub_82000021);\n'
        with self.assertRaisesRegex(ValueError, 'not the header the recipe names'):
            tool.build_header(h, lambda n: CHUNK)
        h, _ = header()
        h['bytes'] += 1
        with self.assertRaisesRegex(ValueError, 'not the header the recipe names'):
            tool.build_header(h, lambda n: CHUNK)


class Recipes(unittest.TestCase):
    def table(self, change=None, part_change=None, edit=None):
        h, _ = header()
        if change: change(h)
        if part_change: h['parts'][1].update(part_change)
        if edit: edit(h['parts'][1]['edits'])
        return {'schema': tool.SCHEMA, 'headers': [h]}

    def refused(self, **kw):
        with self.assertRaises(ValueError):
            tool.check_recipes(self.table(**kw))

    def test_a_good_table_passes(self):
        self.assertEqual(len(tool.check_recipes(self.table())), 1)

    def test_paths_schema_identity(self):
        for path in ('/abs/x.h', 'a/../x.h', 'a\\x.h', 'a/x.cpp', ''):
            self.refused(change=lambda h, path=path: h.update(path=path))
        t = self.table(); t['headers'].append(copy.deepcopy(t['headers'][0]))
        with self.assertRaises(ValueError): tool.check_recipes(t)
        with self.assertRaises(ValueError): tool.check_recipes(dict(self.table(), schema='code-headers/0'))
        self.refused(change=lambda h: h.update(sha256='12'))
        self.refused(change=lambda h: h.update(bytes=0))
        tool.check_recipes(self.table(change=lambda h: h.update(parts=['#pragma once\n#include "../other/hooked.h"\n'])))   # a header that only passes on
        self.refused(change=lambda h: h.update(parts=[]))
        self.refused(change=lambda h: h.update(parts='text'))
        self.refused(change=lambda h: h.update(parts=None))

    def test_function_parts(self):
        for bad in ({'function': '8200001g'}, {'function': '82000a10'}, {'function': '820000100'}, {'chunk': -1}, {'chunk': '7'},
                    {'first_line': 'a\nb'}, {'first_line': 7}, {'direct_calls': 1}, {'edits': None}):
            self.refused(part_change=bad)

    def test_edits(self):
        self.refused(edit=lambda e: e[0].update(at=0))                       # the first line is replaced by first_line, not by an edit
        self.refused(edit=lambda e: e[1].update(at=0))                       # not in line order
        self.refused(edit=lambda e: e[1].update(at=5, remove=4))             # reaches into the next edit
        self.refused(edit=lambda e: e[0].update(remove=-1))
        self.refused(edit=lambda e: e[0].update(removed_sha256='no'))
        self.refused(edit=lambda e: e[0].update(insert='line'))
        self.refused(edit=lambda e: e[0].update(insert=['two\nlines']))
        self.refused(edit=lambda e: e[0].update(insert=[]))                  # removes nothing and inserts nothing
        self.refused(edit=lambda e: e[0].update(at='1'))
        tool.check_recipes(self.table(edit=lambda e: e[1].update(at=5, remove=3)))   # touching the next edit's line is in order


class Build(unittest.TestCase):
    def folders(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        root = Path(tmp.name); (root / 'chunks').mkdir(); (root / 'chunks/ppc_recomp.7.cpp').write_text(CHUNK)
        return root

    def test_headers_are_written_under_their_paths_with_a_report(self):
        root = self.folders(); h, text = header()
        report = tool.build([h], root / 'chunks', root / 'out')
        self.assertEqual(report['status'], 'complete')
        self.assertEqual((root / 'out/old/folder/hooked.h').read_text(), text)
        self.assertEqual(json.loads((root / 'out/report.json').read_text())['headers']['old/folder/hooked.h']['sha256'], h['sha256'])

    def test_a_refused_header_is_not_written_and_fails_the_build(self):
        root = self.folders(); good, _ = header(); bad, _ = header(path='old/folder/other.h'); bad['parts'][0] = 'changed\n'
        missing, _ = header(path='old/folder/third.h'); missing['parts'][1]['chunk'] = 9      # no such chunk file
        report = tool.build([good, bad, missing], root / 'chunks', root / 'out')
        self.assertEqual(report['status'], 'differs')
        self.assertEqual([report['headers'][p]['status'] for p in (good['path'], bad['path'], missing['path'])], ['written', 'refused', 'refused'])
        self.assertFalse((root / 'out/old/folder/other.h').exists())
        self.assertFalse((root / 'out/old/folder/third.h').exists())

    def test_an_existing_output_folder_is_not_written_into(self):
        root = self.folders(); h, _ = header()
        with self.assertRaises(OSError):
            tool.build([h], root / 'chunks', root)

    def run_tool(self, headers, chunks=True):
        root = self.folders()
        (root / 'r.json').write_text(json.dumps({'schema': tool.SCHEMA, 'headers': headers}))
        return subprocess.run([sys.executable, '-B', str(TOOL), '--chunks', str(root / ('chunks' if chunks else 'nowhere')), '--recipes', str(root / 'r.json'),
                               '--output', str(root / 'out')], capture_output=True, text=True)

    def test_exit_codes(self):
        good, _ = header(); bad, _ = header(path='old/folder/other.h'); bad['sha256'] = 'c' * 64
        r = self.run_tool([good]); self.assertEqual((r.returncode, 'CODE_HEADERS complete' in r.stdout), (0, True), r.stdout + r.stderr)
        r = self.run_tool([good, bad]); self.assertEqual(r.returncode, 1); self.assertIn('CODE_HEADER_REFUSED old/folder/other.h', r.stdout)
        r = self.run_tool([]); self.assertEqual(r.returncode, 2); self.assertIn('CODE_HEADERS_REFUSED', r.stderr)
        r = self.run_tool([good], chunks=False); self.assertEqual(r.returncode, 2); self.assertIn('chunk folder is not there', r.stderr)


if __name__ == '__main__':
    unittest.main()
