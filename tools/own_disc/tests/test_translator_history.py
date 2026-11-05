#!/usr/bin/env python3
"""The earlier translator revisions the builder keeps are unchanged copies: every file of a revision folder has the
size and SHA-256 written down here, the folder holds nothing else that Python could import, and its SOURCES.json
says the same. A changed, added or missing file fails."""
import hashlib
import json
import os
import unittest
from pathlib import Path

HISTORY = Path(os.environ.get('TRANSLATOR_HISTORY', Path(__file__).resolve().parents[2] / 'rpf7_inventory' / 'history'))  # the mutant runner points this at a changed copy
PINNED = {
    'r20260910': {
        'inline_shader_translation.py': (7116, '96141f5aa41ee4d2329e56564a33789eb46bd429c547453e0259efef110a5190'),
        'pixel_sample_dataflow.py': (24072, '5ad1ca3bb4cb453fc241248ee18b3d71fc52802d871d1495279f5eb13edaa52e'),
        'raw_shader_analysis.py': (17252, '93556393d475b3d413c469e4576e65e4231767e58a134353d0dacd3488acf26a'),
        'scalar_constant.py': (1979, '4728479804a7767e668686f998e301af749d44d959d6767dfbbfe23802bac213'),
        'scalar_mul_const.py': (1357, '847c6a1d0e1cd3598cabeda89b7190e90e7336dcceeb91024324fe1c87f60a17'),
        'unorm8_vertex_fetch.py': (2562, 'b4f7bca45bd101ad8690db4a1677a3c1b0a8b1e898148dd2cb3a2d05a6a8dcee'),
        'vertex_arithmetic_dataflow.py': (21385, '96f61d56a75f6fca1d75d2897f869bf206022fac1c66e9654762ac78bdadb0dd'),
    },
}
OTHER_FILES = {'README.md', 'SOURCES.json'}


class History(unittest.TestCase):
    def test_the_folder_holds_the_pinned_revisions_and_no_other(self):
        self.assertEqual(sorted(p.name for p in HISTORY.iterdir() if not p.name.startswith('.')), sorted(PINNED))

    def test_every_file_is_the_pinned_one(self):
        for revision, files in PINNED.items():
            folder = HISTORY / revision
            self.assertEqual(sorted(p.name for p in folder.iterdir() if p.name != '__pycache__'), sorted(set(files) | OTHER_FILES), revision)
            for name, (size, digest) in files.items():
                data = (folder / name).read_bytes()
                self.assertEqual(len(data), size, name)
                self.assertEqual(hashlib.sha256(data).hexdigest(), digest, name)

    def test_the_list_of_sources_says_the_same(self):
        for revision, files in PINNED.items():
            sources = json.loads((HISTORY / revision / 'SOURCES.json').read_text())
            self.assertEqual(sources['revision'], revision)
            self.assertEqual({n: (f['bytes'], f['sha256']) for n, f in sources['files'].items()}, files)
            for entry in sources['files'].values():
                self.assertRegex(entry['commit'], r'^[0-9a-f]{40}$')
                self.assertRegex(entry['git_blob'], r'^[0-9a-f]{40}$')

    def test_a_revision_imports_nothing_from_outside_its_folder(self):
        import ast
        import sys
        for revision, files in PINNED.items():
            for name in files:
                for node in ast.walk(ast.parse((HISTORY / revision / name).read_text())):
                    names = [a.name for a in node.names] if isinstance(node, ast.Import) else [node.module] if isinstance(node, ast.ImportFrom) else []
                    for module in names:
                        top = module.split('.')[0]
                        self.assertTrue(top + '.py' in files or top in sys.stdlib_module_names, f'{revision}/{name} imports {module}')


if __name__ == '__main__':
    unittest.main()
