#!/usr/bin/env python3
"""ios_sign.py: the inside-out order on a synthetic app, and (on a Mac with Xcode) a real ad-hoc signature of small iOS
Mach-O files built here, checked by codesign. No game data, no keys. IOS_SIGN=path selects the module under test.

Run: DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer python3 tools/own_disc/tests/test_ios_sign.py
"""
import contextlib
import importlib.util
import io
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(os.environ.get('IOS_SIGN') or Path(__file__).resolve().parents[1] / 'ios_sign.py')
spec = importlib.util.spec_from_file_location('ios_sign_under_test', TOOL)
sg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sg)

MACHO = b'\xcf\xfa\xed\xfe' + b'\0' * 28
XCODE = os.environ.get('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
SDK = Path(XCODE) / 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk'


def plist(path: Path, **keys):
    with path.open('wb') as f:
        plistlib.dump(keys, f)


class Order(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.app = Path(tmp.name) / 'FivePocket.app'
        fw = self.app / 'Frameworks/Game.framework'
        fw.mkdir(parents=True)
        plist(self.app / 'Info.plist', CFBundleExecutable='FivePocket')
        (self.app / 'FivePocket').write_bytes(MACHO)
        (fw / 'Game').write_bytes(MACHO)
        (fw / 'libinner.dylib').write_bytes(MACHO)
        (self.app / 'Frameworks/libMoltenVK.dylib').write_bytes(MACHO)
        (self.app / 'data.bin').write_bytes(b'not code')

    def test_nested_code_first_deepest_first_and_never_deep_signing(self):
        calls = []
        def run(argv, **kw):
            calls.append(argv)
            return subprocess.CompletedProcess(argv, 0, '', '')
        done = sg.sign(self.app, '-', run=run)
        self.assertEqual(done, ['Frameworks/Game.framework/libinner.dylib', 'Frameworks/Game.framework',
                                'Frameworks/libMoltenVK.dylib', '.'])
        signing = [c for c in calls if '--sign' in c]
        self.assertFalse(any('--deep' in c for c in signing), 'codesign --deep is never used to sign')
        self.assertEqual(calls[-1][1:4], ['--verify', '--deep', '--strict'])

    def test_entitlements_and_profile_go_to_the_app_only(self):
        ent = self.app.parent / 'e.plist'; plist(ent, **{'get-task-allow': True})
        prof = self.app.parent / 'p.mobileprovision'; prof.write_bytes(b'SYNTHETIC profile')
        calls = []
        sg.sign(self.app, '-', prof, ent, run=lambda argv, **kw: calls.append(argv) or subprocess.CompletedProcess(argv, 0, '', ''))
        with_ent = [c for c in calls if '--entitlements' in c]
        self.assertEqual(len(with_ent), 1)
        self.assertEqual(with_ent[0][-1], str(self.app))
        self.assertEqual((self.app / 'embedded.mobileprovision').read_bytes(), b'SYNTHETIC profile')

    def test_refusals(self):
        (self.app / 'link').symlink_to('data.bin')
        with self.assertRaisesRegex(sg.Refused, 'a link in the app'):
            sg.sign(self.app, '-', run=lambda *a, **k: None)
        (self.app / 'link').unlink()
        plist(self.app / 'Info.plist', CFBundleExecutable='Missing')
        with self.assertRaisesRegex(sg.Refused, 'names no executable'):
            sg.sign(self.app, '-', run=lambda *a, **k: None)
        with self.assertRaisesRegex(sg.Refused, 'no signing identity'):
            sg.sign(self.app, '', run=lambda *a, **k: None)
        failing = lambda argv, **kw: subprocess.CompletedProcess(argv, 1, '', 'errSecInternalComponent')
        plist(self.app / 'Info.plist', CFBundleExecutable='FivePocket')
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(subprocess.CalledProcessError):
                sg.sign(self.app, '-', run=failing)


@unittest.skipUnless(sys.platform == 'darwin' and SDK.is_dir(), 'needs Xcode with the iPhoneOS SDK')
class RealSignature(unittest.TestCase):
    def clang(self, *args):
        subprocess.run([str(Path(XCODE) / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'), '-target', 'arm64-apple-ios17.4',
                        '-isysroot', str(SDK), *args], check=True, capture_output=True)

    def test_ad_hoc_signature_of_a_small_ios_app_verifies(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        root = Path(tmp.name); app = root / 'Tiny.app'; fw = app / 'Frameworks/Game.framework'; fw.mkdir(parents=True)
        (root / 'g.c').write_text('int game(void) { return 7; }\n', encoding='utf-8')
        (root / 'm.c').write_text('int main(void) { return 0; }\n', encoding='utf-8')
        self.clang('-dynamiclib', '-install_name', '@rpath/Game.framework/Game', '-o', str(fw / 'Game'), str(root / 'g.c'))
        plist(fw / 'Info.plist', CFBundleExecutable='Game', CFBundleIdentifier='example.tiny.game', CFBundlePackageType='FMWK')
        self.clang('-o', str(app / 'Tiny'), str(root / 'm.c'))
        plist(app / 'Info.plist', CFBundleExecutable='Tiny', CFBundleIdentifier='example.tiny', CFBundlePackageType='APPL')
        with contextlib.redirect_stdout(io.StringIO()) as said:
            self.assertEqual(sg.main(['--app', str(app), '--identity', '-', '--xcode', XCODE]), 0)
        self.assertIn('SIGNED Tiny.app: 2 items', said.getvalue())
        for target in (fw, app):
            info = subprocess.run(['/usr/bin/codesign', '-dv', str(target)], capture_output=True, text=True).stderr
            self.assertIn('Signature=adhoc', info)
        (fw / 'Game').write_bytes((fw / 'Game').read_bytes() + b'\0')               # changed after signing
        r = subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(app)], capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0, 'a change to nested code breaks the seal')


if __name__ == '__main__':
    unittest.main()
