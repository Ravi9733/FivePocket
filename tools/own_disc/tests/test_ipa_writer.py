#!/usr/bin/env python3
"""ipa_writer.py on synthetic app folders (no game data): the same bytes on every run, what the archive holds, ZIP64,
the refusals. IPA_WRITER=path selects the module under test.

Run: python3 tools/own_disc/tests/test_ipa_writer.py
"""
import contextlib
import hashlib
import importlib.util
import io
import os
import stat
import tempfile
import time
import unittest
import zipfile
from pathlib import Path
from unittest import mock

TOOL = Path(os.environ.get('IPA_WRITER') or Path(__file__).resolve().parents[1] / 'ipa_writer.py')
spec = importlib.util.spec_from_file_location('ipa_writer_under_test', TOOL)
iw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(iw)


class Ipa(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.app = self.root / 'FivePocket.app'
        (self.app / 'Frameworks').mkdir(parents=True)
        (self.app / 'FivePocket').write_bytes(b'SYNTHETIC executable'); (self.app / 'FivePocket').chmod(0o755)
        (self.app / 'Info.plist').write_bytes(b'<plist/>')
        (self.app / 'Frameworks/libx.dylib').write_bytes(b'SYNTHETIC dylib'); (self.app / 'Frameworks/libx.dylib').chmod(0o755)
        (self.app / '._Info.plist').write_bytes(b'AppleDouble')
        (self.app / '.DS_Store').write_bytes(b'finder')
        self.outputs = self.root / 'outputs'; self.outputs.mkdir()
        (self.outputs / 'catalog.bin').write_bytes(b'SYNTHETIC catalog')

    def write(self, name='out.ipa', adds=None):
        out = self.root / name
        iw.write_ipa(self.app, out, adds)
        return out

    def test_same_folder_same_bytes_whatever_the_times_and_listing_order(self):
        first = self.write('a.ipa', [('r/outputs', self.outputs)])
        os.utime(self.app / 'Info.plist', (time.time() - 99999, time.time() - 99999))
        second = self.write('b.ipa', [('r/outputs', self.outputs)])
        self.assertEqual(hashlib.sha256(first.read_bytes()).digest(), hashlib.sha256(second.read_bytes()).digest())

    def test_what_the_archive_holds(self):
        with zipfile.ZipFile(self.write(adds=[('r/outputs', self.outputs)])) as z:
            names = z.namelist()
            self.assertEqual(names, sorted(names))
            self.assertEqual(names, ['Payload/', 'Payload/FivePocket.app/', 'Payload/FivePocket.app/FivePocket',
                                     'Payload/FivePocket.app/Frameworks/', 'Payload/FivePocket.app/Frameworks/libx.dylib',
                                     'Payload/FivePocket.app/Info.plist', 'Payload/FivePocket.app/r/',
                                     'Payload/FivePocket.app/r/outputs/', 'Payload/FivePocket.app/r/outputs/catalog.bin'])
            modes = {i.filename: stat.S_IMODE(i.external_attr >> 16) for i in z.infolist()}
            self.assertEqual(modes['Payload/FivePocket.app/FivePocket'], 0o755)
            self.assertEqual(modes['Payload/FivePocket.app/Frameworks/libx.dylib'], 0o755)
            self.assertEqual(modes['Payload/FivePocket.app/Info.plist'], 0o644)
            for i in z.infolist():
                self.assertEqual((i.compress_type, i.date_time, i.create_system), (zipfile.ZIP_STORED, (1980, 1, 1, 0, 0, 0), 3))
            self.assertEqual(z.read('Payload/FivePocket.app/r/outputs/catalog.bin'), b'SYNTHETIC catalog')
            self.assertIsNone(z.testzip())

    def test_zip64_records_where_a_file_passes_the_limit(self):
        (self.outputs / 'big.bin').write_bytes(b'Z' * 5000)
        with mock.patch.object(zipfile, 'ZIP64_LIMIT', 4096):       # stands in for 4 GiB: same records, small file
            out = self.write(adds=[('r/outputs', self.outputs)])
        raw = out.read_bytes()
        self.assertIn(b'PK\x06\x06', raw, 'a ZIP64 end record')
        with zipfile.ZipFile(out) as z:
            self.assertEqual(z.read('Payload/FivePocket.app/r/outputs/big.bin'), b'Z' * 5000)
            self.assertIsNone(z.testzip())

    def test_refusals_leave_nothing_behind(self):
        link = self.app / 'link'
        link.symlink_to('Info.plist')
        with self.assertRaisesRegex(iw.Refused, 'a link is not packed'):
            self.write()
        link.unlink()
        # two names that differ only by case (made through an added folder: this disk may itself fold case)
        twin = self.root / 'twin'; twin.mkdir(); (twin / 'LIBX.dylib').write_bytes(b'case twin')
        with self.assertRaisesRegex(iw.Refused, 'two entries named'):
            self.write(adds=[('Frameworks', twin)])
        for place in ('/abs', '../out', ''):
            with self.subTest(place=place), self.assertRaisesRegex(iw.Refused, 'relative path inside the app'):
                self.write(adds=[(place, self.outputs)])
        with self.assertRaisesRegex(iw.Refused, 'not an app folder'):
            iw.write_ipa(self.outputs, self.root / 'x.ipa')
        out = self.write()
        with self.assertRaisesRegex(iw.Refused, 'the output exists'):
            iw.write_ipa(self.app, out)
        self.assertEqual(sorted(p.name for p in self.root.iterdir()), ['FivePocket.app', 'out.ipa', 'outputs', 'twin'])

    def test_an_added_file_over_an_app_file_is_refused(self):
        clash = self.root / 'clash'; clash.mkdir(); (clash / 'libx.dylib').write_bytes(b'other')
        with self.assertRaisesRegex(iw.Refused, 'two entries named'):
            self.write(adds=[('Frameworks', clash)])
        self.assertFalse((self.root / 'out.ipa').exists())
        self.assertFalse((self.root / 'out.ipa.partial').exists())

    def test_the_command_line(self):
        out = self.root / 'cli.ipa'
        with contextlib.redirect_stdout(io.StringIO()) as said:
            self.assertEqual(iw.main(['--app', str(self.app), '--output', str(out), '--add', f'r/outputs={self.outputs}']), 0)
        self.assertIn('IPA cli.ipa: 9 entries', said.getvalue())
        with contextlib.redirect_stderr(io.StringIO()) as said:
            self.assertEqual(iw.main(['--app', str(self.app), '--output', str(out)]), 2)
        self.assertIn('IPA_REFUSED: the output exists', said.getvalue())


if __name__ == '__main__':
    unittest.main()
