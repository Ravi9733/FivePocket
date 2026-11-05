#!/usr/bin/env python3
"""taint_scan.py on synthetic bytes: a copied run of the "retail" file is found wherever it sits, zero fill and unrelated
bytes are not.

Run: python3 tools/own_disc/tests/test_taint_scan.py
"""
import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import taint_scan as ts  # noqa: E402


class Taint(unittest.TestCase):
    def test_a_copied_run_is_found_and_padding_is_not(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            retail = os.urandom(4096) + b''.join(i.to_bytes(4, 'big') for i in range(256)) * 4    # the same counters
            (t / 'retail.bin').write_bytes(retail)
            folder = t / 'kit'; folder.mkdir()
            (folder / 'copied.h').write_bytes(b'/* ours */' + retail[1001:1001 + 48] + b'/* ours */')   # 48 >= 32 + 16 - 1
            (folder / 'padding.bin').write_bytes(b'\0' * 4096 + retail[:31])                              # shorter than a window
            (folder / 'ours.cpp').write_bytes(os.urandom(4096))
            (folder / 'counters.bin').write_bytes(b''.join(i.to_bytes(4, 'big') for i in range(256)) * 4)    # small integers
            with contextlib.redirect_stdout(io.StringIO()) as said:
                code = ts.main(['--retail', str(t / 'retail.bin'), '--scan', str(folder), '--report', str(t / 'r.json')])
            self.assertEqual(code, 1)
            self.assertIn('copied.h', said.getvalue())
            self.assertNotIn('padding.bin', said.getvalue())
            self.assertNotIn('ours.cpp', said.getvalue())
            self.assertNotIn('counters.bin', said.getvalue(), 'runs of small integers say nothing about origin')
            hits, first = ts.scan(folder / 'copied.h', ts.index([t / 'retail.bin'], 16))
            self.assertGreater(hits, 0)
            self.assertEqual((first[0] - len(b'/* ours */') + 1001) % 16, 0, 'the first hit is a window the index holds')
            (folder / 'copied.h').unlink()
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(ts.main(['--retail', str(t / 'retail.bin'), '--scan', str(folder)]), 0)


    def test_planted_controls_are_all_found_and_a_blind_scan_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            (t / 'retail.bin').write_bytes(os.urandom(8192))
            (t / 'kit').mkdir()
            keys = ts.index([t / 'retail.bin'], 16)
            self.assertEqual(ts.controls([t / 'retail.bin'], keys, 16, 64), (64, 64))
            # an index that holds nothing (a broken index or a rule that hides everything): every control missed
            self.assertEqual(ts.controls([t / 'retail.bin'], set(), 16, 64)[1], 0)
            with mock.patch.object(ts, 'index', return_value=set()), contextlib.redirect_stderr(io.StringIO()) as said:
                self.assertEqual(ts.main(['--retail', str(t / 'retail.bin'), '--scan', str(t / 'kit')]), 2)
            self.assertIn('planted controls 0/', said.getvalue())


if __name__ == '__main__':
    unittest.main()
