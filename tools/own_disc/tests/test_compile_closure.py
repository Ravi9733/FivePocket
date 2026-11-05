#!/usr/bin/env python3
"""Tests of compile_closure.py: every file the compiler read lands in exactly one group, and an undeclared one fails."""
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = Path(os.environ.get('CLOSURE_TOOL_DIR', HERE.parent)) / 'compile_closure.py'
spec = importlib.util.spec_from_file_location('compile_closure', TOOL)
closure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(closure)

WORK = 'local-analysis/builder/run/work-01'
INPUTS = {
    WORK + '/compile/src/chunk.cpp': {'size': 100},
    'local-analysis/builder/run/work-012/other.h': {'size': 7},          # another run's folder: not this work folder
    'android/app/src/main/cpp/runtime.h': {'size': 20},
    'local-analysis/packs/exposure-v5/pack.h': {'size': 3000},
    'local-analysis/packs/exposure-v5/extra/deep.h': {'size': 30},
    'local-analysis/packs/exposure-v50/pack.h': {'size': 400},           # a folder whose name only starts the same
    'local-analysis/pgo/profile.profdata': {'size': 50000},
    'local-analysis/old/original.h': {'size': 9},
}
TRACKED = {'android/app/src/main/cpp/runtime.h', 'android/app/src/main/cpp/unused.h'}
DECLARED = [
    {'path': 'local-analysis/packs/exposure-v5/', 'what': 'one shader pack', 'game_derived': True},
    {'path': 'local-analysis/packs/exposure-v5/extra/deep.h', 'what': 'a file named by itself', 'game_derived': False},
    {'path': 'local-analysis/pgo/profile.profdata', 'what': 'the optimisation profile', 'game_derived': True},
    {'path': 'local-analysis/never/read.h', 'what': 'a stale entry', 'game_derived': False},
]


class Classify(unittest.TestCase):
    def setUp(self):
        self.r = closure.classify(INPUTS, WORK, TRACKED, DECLARED)

    def test_every_file_in_exactly_one_group(self):
        seen = [p for name in closure.GROUPS for p in self.r['groups'][name]]
        self.assertEqual(sorted(seen), sorted(INPUTS))
        self.assertEqual(len(seen), len(set(seen)))

    def test_the_work_folder_is_a_folder_not_a_prefix(self):
        self.assertEqual(self.r['groups']['work'], [WORK + '/compile/src/chunk.cpp'])
        self.assertIn('local-analysis/builder/run/work-012/other.h', self.r['groups']['undeclared'])

    def test_tracked_files_are_their_own_group(self):
        self.assertEqual(self.r['groups']['tracked'], ['android/app/src/main/cpp/runtime.h'])

    def test_a_declared_folder_covers_what_lies_in_it_and_nothing_beside_it(self):
        self.assertIn('local-analysis/packs/exposure-v5/pack.h', self.r['groups']['declared'])
        self.assertIn('local-analysis/packs/exposure-v50/pack.h', self.r['groups']['undeclared'])

    def test_what_nobody_declared_is_undeclared(self):
        self.assertEqual(self.r['groups']['undeclared'], ['local-analysis/builder/run/work-012/other.h',
                                                         'local-analysis/old/original.h',
                                                         'local-analysis/packs/exposure-v50/pack.h'])

    def test_the_most_specific_entry_owns_a_file(self):
        by = {e['path']: e for e in self.r['declared']}
        self.assertEqual((by['local-analysis/packs/exposure-v5/extra/deep.h']['files'], by['local-analysis/packs/exposure-v5/extra/deep.h']['bytes']), (1, 30))
        self.assertEqual((by['local-analysis/packs/exposure-v5/']['files'], by['local-analysis/packs/exposure-v5/']['bytes']), (1, 3000))

    def test_bytes_per_group(self):
        self.assertEqual(self.r['bytes'], {'work': 100, 'tracked': 20, 'declared': 53030, 'undeclared': 416})

    def test_a_declared_entry_nobody_read_is_reported(self):
        self.assertEqual(self.r['unused'], ['local-analysis/never/read.h'])

    def test_a_file_entry_does_not_cover_a_folder_of_that_name(self):
        r = closure.classify({'local-analysis/pgo/profile.profdata/inner': {'size': 1}}, WORK, set(), DECLARED)
        self.assertEqual(r['groups']['undeclared'], ['local-analysis/pgo/profile.profdata/inner'])

    def test_the_work_folder_must_be_relative(self):
        for bad in ('', '/', '/abs/work'):
            with self.assertRaises(ValueError):
                closure.classify(INPUTS, bad, TRACKED, DECLARED)


class Declaration(unittest.TestCase):
    def load(self, table):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'd.json'
            p.write_text(json.dumps(table))
            return closure.load_declared(p)

    def test_a_good_table_loads(self):
        self.assertEqual(len(self.load({'schema': closure.DECLARED_SCHEMA, 'entries': DECLARED})), 4)

    def test_refusals(self):
        good = {'path': 'a/b.h', 'what': 'x', 'game_derived': False}
        for entries in ([dict(good, path='/abs/b.h')], [dict(good, path='a/../b.h')], [dict(good, path='')],
                        [dict(good, game_derived='no')], [dict(good, what='')], [good, dict(good)],
                        [{'path': 'a/b.h', 'what': 'x'}]):
            with self.assertRaises(ValueError, msg=str(entries)):
                self.load({'schema': closure.DECLARED_SCHEMA, 'entries': entries})
        with self.assertRaises(ValueError):
            self.load({'schema': 'declared-inputs/0', 'entries': [good]})


class CommandLine(unittest.TestCase):
    def run_tool(self, inputs, declared):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'm.json').write_text(json.dumps({'repo_root': '/nowhere', 'inputs': inputs}))
            (tmp / 'd.json').write_text(json.dumps({'schema': closure.DECLARED_SCHEMA, 'entries': declared}))
            (tmp / 't.txt').write_text('\n'.join(sorted(TRACKED)) + '\n')
            r = subprocess.run([sys.executable, '-B', str(TOOL), '--manifest', str(tmp / 'm.json'), '--work', WORK,
                                '--declared', str(tmp / 'd.json'), '--tracked', str(tmp / 't.txt'), '--report', str(tmp / 'r.json')],
                               capture_output=True, text=True)
            report = json.loads((tmp / 'r.json').read_text()) if (tmp / 'r.json').exists() else None
            return r, report

    def test_an_undeclared_file_fails_the_check_and_is_named(self):
        r, report = self.run_tool(INPUTS, DECLARED)
        self.assertEqual(r.returncode, 1)
        self.assertIn('COMPILE_CLOSURE UNDECLARED_INPUTS', r.stdout)
        self.assertIn('UNDECLARED          9  local-analysis/old/original.h', r.stdout)
        self.assertEqual(len(report['groups']['undeclared']), 3)

    def test_everything_declared_passes(self):
        more = DECLARED + [{'path': 'local-analysis/old/', 'what': 'old', 'game_derived': True},
                           {'path': 'local-analysis/packs/exposure-v50/', 'what': 'p', 'game_derived': True},
                           {'path': 'local-analysis/builder/run/work-012/', 'what': 'w', 'game_derived': False}]
        r, report = self.run_tool(INPUTS, more)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('COMPILE_CLOSURE complete', r.stdout)
        self.assertIn('declared GAME-DERIVED', r.stdout)
        self.assertEqual(report['groups']['undeclared'], [])

    def test_a_manifest_without_inputs_is_refused(self):
        r, _ = self.run_tool({}, DECLARED)
        self.assertEqual(r.returncode, 2)
        self.assertIn('COMPILE_CLOSURE_REFUSED', r.stderr)


if __name__ == '__main__':
    unittest.main()
