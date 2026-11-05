#!/usr/bin/env python3
"""macho_sections.code_identity on small iOS objects compiled here (needs Xcode with the iPhoneOS SDK): the same code
compiled with debug information in two folders has other bytes and the same identity; other code has another identity.

Run: DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer python3 tools/own_disc/tests/test_macho_sections.py
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import macho_sections as ms  # noqa: E402

XCODE = Path(os.environ.get('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer'))
CLANG = XCODE / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'
SDK = XCODE / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'


@unittest.skipUnless(CLANG.is_file() and SDK.is_dir(), 'needs Xcode with the iPhoneOS SDK')
class Identity(unittest.TestCase):
    def compile(self, folder: Path, body: str) -> Path:
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'u.c').write_text('static int table[4] = {1, 2, 3, 4};\nint f(int x) { return table[x & 3] * ' + body + '; }\n',
                                    encoding='utf-8')
        obj = folder / 'u.o'
        subprocess.run([str(CLANG), '-target', 'arm64-apple-ios17.4', '-isysroot', str(SDK), '-O2', '-g', '-c', 'u.c', '-o', 'u.o'],
                       cwd=folder, check=True, capture_output=True)
        return obj

    def test_debug_information_only_keeps_the_identity_and_code_changes_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = self.compile(Path(tmp) / 'one', 'x')
            b = self.compile(Path(tmp) / 'a-longer-second-folder', 'x')
            c = self.compile(Path(tmp) / 'three', '(x + 1)')
            self.assertNotEqual(a.read_bytes(), b.read_bytes(), '(the folders are in the debug information)')
            self.assertEqual(ms.code_identity(a), ms.code_identity(b))
            self.assertNotEqual(ms.code_identity(a), ms.code_identity(c))
            self.assertTrue(any(s[0] == '__DWARF' for s in ms.sections(a.read_bytes())), '(there is debug information to leave out)')

    def test_not_a_thin_64_bit_file_is_refused(self):
        with self.assertRaises(ValueError):
            list(ms.sections(b'\xca\xfe\xba\xbe' + b'\0' * 60))


if __name__ == '__main__':
    unittest.main()
