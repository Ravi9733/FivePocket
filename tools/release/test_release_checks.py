"""Tests of the release checks: each check must catch the thing it exists for, and pass a clean tree."""
import atexit
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def run(script, *args):
    return subprocess.run([sys.executable, str(HERE / script), *map(str, args)], capture_output=True, text=True)


class Tree:
    def __init__(self, files):
        self.root = Path(tempfile.mkdtemp(prefix='release-check-'))
        atexit.register(shutil.rmtree, self.root, True)
        for rel, data in files.items():
            p = self.root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data if isinstance(data, bytes) else data.encode())


class Names(unittest.TestCase):
    def test_internal_source_names_pass_and_product_download_and_top_level_names_fail(self):
        clean = Tree({'src/pocket_renderer.cpp': 'int x;\n', 'runtime/G-T-A_5_driver_constants.h': 'int x;\n',
                      'tools/G-T-A_5_codegen/main.py': 'x = 1\n',          # a source folder deeper inside
                      'builder/host-tools/dxc/lib/libLLVMDxcBindingTable.a': b'!<arch>'})
        self.assertEqual(run('scan_names.py', clean.root).returncode, 0, run('scan_names.py', clean.root).stdout)
        dirty = Tree({'FivePocket-Grand-Theft-1.0-android/README.txt': 'FivePocket\n',  # a download's own name
                      'builder/out/fivepocket-G-T-A.bundle': b'x',                       # a handed-out file type
                      'builder/FivePocket-G-T-A-Builder.zip': b'PK',
                      'G-T-A_notes.txt': 'x\n',                                          # what a player sees on opening
                      'G-T-A_tools/main.py': 'x = 1\n'})
        out = run('scan_names.py', dirty.root)
        self.assertEqual(out.returncode, 1)
        for name in ('fivepocket-G-T-A.bundle', 'FivePocket-G-T-A-Builder.zip', 'G-T-A_notes.txt', 'G-T-A_tools',
                     'FivePocket-Grand-Theft-1.0-android'):
            self.assertIn(name, out.stdout)
        out = run('scan_names.py', dirty.root / 'FivePocket-Grand-Theft-1.0-android')
        self.assertIn('the download or folder name', out.stdout)

    def test_player_visible_text_and_placeholders_fail(self):
        t = Tree({'app/src/CaptureExport.java': 'String DIR = "Pictures/G-T-A 5 Captures";\n',
                  'ios/Info.plist': '<key>CFBundleDisplayName</key><string>G-T-A Duo</string>\n',
                  'packaging/README-ios.txt': 'FivePocket G-T-A for iPad\n',
                  'SECURITY.md': 'Discord: DISCORD-INVITE-' + 'TO-BE-SET\n',
                  'android/app/build.gradle.kts': 'applicationId = "dev.pocket.g' + 'ta5"\n',
                  'ios/project.pbxproj': 'PRODUCT_BUNDLE_IDENTIFIER = dev.pocket.G' + 'TAV;\n'})
        out = run('scan_names.py', t.root).stdout
        for what in ('FOLDER app/src/CaptureExport.java:1', 'LABEL ios/Info.plist', 'TITLE packaging/README-ios.txt',
                     'PLACEHOLDER SECURITY.md:1', 'ID android/app/build.gradle.kts:1', 'ID ios/project.pbxproj:1'):
            self.assertIn(what, out)

    def test_app_label_is_checked(self):
        t = Tree({'android/app/src/main/res/values/strings.xml':
                  '<resources><string name="app_name">Grand Theft Pocket</string></resources>'})
        self.assertIn('LABEL', run('scan_names.py', t.root).stdout)


class Secrets(unittest.TestCase):
    KEY = bytes(range(0x30, 0x40))  # a made-up key, forbidden through --key-file

    def test_private_key_block_fails(self):
        t = Tree({'a.txt': '-----BEGIN RSA ' + 'PRIVATE KEY-----\nabc\n'})  # split so this file passes the scan
        self.assertIn('private key block', run('scan_secrets.py', t.root).stdout)

    def test_known_key_in_c_array_hex_string_and_binary_fails(self):
        keyfile = Tree({'k.bin': self.KEY})
        array = ', '.join(f'0x{b:02X}' for b in self.KEY)
        t = Tree({'src/a.cpp': f'static const unsigned char blob[] = {{ 0x00, {array}, 0x00 }};\n',
                  'src/b.py': f'X = "{self.KEY.hex()}"\n',
                  'res/c.dat': b'\0' * 9 + self.KEY + b'\1' * 9})
        out = run('scan_secrets.py', t.root, '--key-file', keyfile.root / 'k.bin')
        self.assertEqual(out.returncode, 1)
        for rel in ('src/a.cpp', 'src/b.py', 'res/c.dat'):
            self.assertIn(rel, out.stdout)

    def test_unknown_key_shaped_array_fails_and_a_table_passes(self):
        values = ', '.join(f'0x{b:02x}' for b in hashlib.sha256(b'x').digest()[:16])
        t = Tree({'k.cpp': f'const uint8_t kAesKey[16] = {{ {values} }};\n'})
        self.assertIn('KEYLIKE', run('scan_secrets.py', t.root).stdout)
        t = Tree({'k.cpp': f'const uint8_t kGammaTable[16] = {{ {values} }};\n'})
        self.assertEqual(run('scan_secrets.py', t.root).returncode, 0)

    def test_upstream_allowance_waives_only_key_findings_in_the_exact_bytes(self):
        keyfile = Tree({'k.bin': self.KEY})
        binary = b'\0' * 9 + self.KEY + b'\1' * 9
        header = 'const uint8_t RetailKey[16] = { %s };\nconst char *t = "ghp_%s";\n' % (
            ', '.join(f'0x{b:02X}' for b in self.KEY), 'A' * 36)
        allow = Tree({'allow.txt': f'{hashlib.sha256(binary).hexdigest()} host-tools/XenonRecomp upstream\n'
                                   f'{hashlib.sha256(header.encode()).hexdigest()} sources/xex.h upstream\n'})
        t = Tree({'host-tools/XenonRecomp': binary, 'sources/xex.h': header,
                  'host-tools/xex_image': binary[:-1] + b'\2'})  # one byte changed: not the allowed file
        out = run('scan_secrets.py', t.root, '--key-file', keyfile.root / 'k.bin', '--allow', allow.root / 'allow.txt')
        self.assertEqual(out.returncode, 1)
        self.assertIn('ALLOWED host-tools/XenonRecomp', out.stdout)
        self.assertIn('KEY host-tools/xex_image', out.stdout)
        self.assertIn('SECRET sources/xex.h:2: GitHub token', out.stdout)  # never waived
        self.assertNotIn('KEY sources/xex.h', out.stdout)

    def test_signing_files_fail(self):
        t = Tree({'release.jks': b'\0\1'})
        self.assertIn('FILETYPE', run('scan_secrets.py', t.root).stdout)


class Owner(unittest.TestCase):
    def test_home_folder_lan_address_and_email_fail(self):
        # Built from pieces so that this test file itself passes the scan.
        t = Tree({'a.py': "ROOT = '/Users/" + "jdoe/src'\nHOST = '192." + "168.1.20'\nMAIL = 'someone" + "@mail.test'\n"})
        out = run('scan_owner.py', t.root)
        self.assertEqual(out.returncode, 1)
        for what in ('home folder', 'private network address', 'EMAIL'):
            self.assertIn(what, out.stdout)

    def test_third_party_folder_waives_only_generic_details(self):
        body = "ROOT = '/Users/" + "jdoe/src'\nHOST = '192." + "168.1.20'\nMAIL = 'someone" + "@mail.test'\n"
        t = Tree({'sources/thirdparty/lib/a.py': body})
        out = run('scan_owner.py', t.root)
        self.assertEqual(out.returncode, 0, out.stdout)
        self.assertIn('NOTE 3 ', out.stdout)
        extra = Tree({'x.txt': 'jdoe\n'})  # our own people stay forbidden everywhere
        self.assertIn('PRIVATE sources/thirdparty/lib/a.py:1', run('scan_owner.py', t.root, '--extra', extra.root / 'x.txt').stdout)
        t = Tree({'sources/thirdparty/lib/a.sh': 'ssh ' + 'jdoe@build-host true\n'})
        self.assertIn('ssh user' + '@host', run('scan_owner.py', t.root).stdout)  # split: this file must pass

    def test_a_download_carries_only_the_listed_dylibs(self):
        lib = b'\xcf\xfa\xed\xfe' + os.urandom(4000)
        allow = Tree({'ft.txt': f'{hashlib.sha256(lib).hexdigest()} host-tools/dxc/lib/libx.dylib a listed library (decision 21)\n'})
        ok = Tree({'host-tools/dxc/lib/libx.dylib': lib})
        out = run('scan_secrets.py', ok.root, '--download', '--filetype-allow', allow.root / 'ft.txt')
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn('ALLOWED host-tools/dxc/lib/libx.dylib: listed library', out.stdout)
        for name, data in (('host-tools/dxc/lib/libx.dylib', lib[:-1] + bytes([lib[-1] ^ 1])),  # one byte changed
                           ('host-tools/other/liby.dylib', lib),                     # the same bytes elsewhere
                           ('host-tools/dxc/lib/libz.dylib', os.urandom(100))):      # not listed at all
            bad = Tree({name: data})
            with self.subTest(name):
                out = run('scan_secrets.py', bad.root, '--download', '--filetype-allow', allow.root / 'ft.txt')
                self.assertEqual(out.returncode, 1, out.stdout + out.stderr)
                self.assertIn('FILETYPE', out.stdout)

    def test_exact_file_allowance_covers_only_that_file_and_those_bytes(self):
        manifest = '{"repo_root": "/Users/' + 'jdoe/Developer/repo"}\n'
        allow = Tree({'allow.txt': hashlib.sha256(manifest.encode()).hexdigest() +
                      ' local-analysis/builder/frozen-manifest.json provenance of the reference build\n'})
        ok = Tree({'builder/local-analysis/builder/frozen-manifest.json': manifest})
        out = run('scan_owner.py', ok.root, '--exact-allow', allow.root / 'allow.txt')
        self.assertEqual(out.returncode, 0, out.stdout)
        self.assertIn('ALLOWED builder/local-analysis/builder/frozen-manifest.json', out.stdout)
        bad = Tree({'builder/local-analysis/builder/recipe.json': manifest,            # same path text, other file
                    'builder/local-analysis/builder/frozen-manifest.json': manifest + ' '})  # one byte more
        out = run('scan_owner.py', bad.root, '--exact-allow', allow.root / 'allow.txt')
        self.assertEqual(out.returncode, 1)
        self.assertIn('OWNER builder/local-analysis/builder/recipe.json:1: home folder', out.stdout)
        self.assertIn('OWNER builder/local-analysis/builder/frozen-manifest.json:1: home folder', out.stdout)

    def test_placeholders_pass(self):
        t = Tree({'a.py': "A = '/Users/a/repo'\nB = '/home/runner/x'\nC = 't@example.invalid'\nD = '127.0.0.1'\n"})
        self.assertEqual(run('scan_owner.py', t.root).returncode, 0, run('scan_owner.py', t.root).stdout)

    def test_extra_literals_are_reported_without_the_text(self):
        extra = Tree({'x.txt': 'secret-host-name\n'})
        t = Tree({'a.sh': 'ssh secret-host-name true\n'})
        out = run('scan_owner.py', t.root, '--extra', extra.root / 'x.txt')
        self.assertIn('private literal #1', out.stdout)
        self.assertNotIn('secret-host-name', out.stdout)


class Packaging(unittest.TestCase):
    def test_taint_parts_cover_every_file_once_within_the_limit(self):
        sys.path.insert(0, str(HERE))
        import package_release
        files = {'a/x.bin': b'1' * 40, 'a/y.bin': b'2' * 40, 'a/deep/z.bin': b'3' * 30, 'b/big.bin': b'4' * 150,
                 'top.txt': b'5' * 10, 'c/small.txt': b'6' * 5}
        t = Tree(files)
        parts = package_release.taint_parts(t.root / '.', t.root.parent / f'{t.root.name}-work', limit=100)
        seen = []
        for part in parts:
            sizes = [f.stat().st_size for f in part.rglob('*') if f.is_file()]
            self.assertTrue(sum(sizes) <= 100 or len(sizes) == 1, (part, sizes))
            seen += [f.relative_to(part).as_posix() for f in part.rglob('*') if f.is_file()]
        self.assertEqual(sorted(seen), sorted(files))
        shutil.rmtree(t.root.parent / f'{t.root.name}-work', ignore_errors=True)

    def test_taint_union_scans_every_distinct_content_of_a_path_once(self):
        # Two downloads share a path with different bytes, and the run from the game is only in the second one's copy;
        # the same bytes at the same path in both downloads are scanned once; an .apk member is scanned too.
        sys.path.insert(0, str(HERE))
        import io, zipfile
        import package_release
        retail_bytes = os.urandom(200_000)
        retail = Tree({'game.bin': retail_bytes})
        apk = io.BytesIO()
        with zipfile.ZipFile(apk, 'w') as z:
            z.writestr('classes.dex', os.urandom(3000))
        shared = os.urandom(4000)
        one = Tree({'builder/tool.py': os.urandom(3000), 'COPYING': shared, 'app.apk': apk.getvalue()})
        two = Tree({'builder/tool.py': os.urandom(1000) + retail_bytes[90_000:90_048] + os.urandom(1000), 'COPYING': shared})
        rows = package_release.staged_files([one.root, two.root])
        work = Path(tempfile.mkdtemp(prefix='release-union-'))
        atexit.register(shutil.rmtree, work, True)
        roots = package_release.taint_union(rows, work, work / 'map.tsv')
        scanned = sorted(f.relative_to(root).as_posix() for root in roots for f in root.rglob('*') if f.is_file())
        self.assertEqual(scanned, ['COPYING', 'app.apk', 'app.apk.unpacked/classes.dex', 'builder/tool.py', 'builder/tool.py'])
        out = run('taint_scan.py', '--published', *roots, '--retail', retail.root / 'game.bin')
        self.assertEqual(out.returncode, 1, out.stdout + out.stderr)
        self.assertIn('taint-union-2/builder/tool.py', out.stdout)
        self.assertIn(f'taint-union-2/builder/tool.py\t{two.root.name}', (work / 'map.tsv').read_text())


class Taint(unittest.TestCase):
    def test_planted_run_is_found_and_reported_without_its_bytes(self):
        retail_bytes = os.urandom(200_000)
        retail = Tree({'game.bin': retail_bytes})
        pub = Tree({'clean.txt': os.urandom(5000), 'copied.cpp': b'// x\n' + retail_bytes[70_000:70_040] + b'\n'})
        out = run('taint_scan.py', '--published', pub.root, '--retail', retail.root / 'game.bin')
        self.assertEqual(out.returncode, 1, out.stdout + out.stderr)
        self.assertIn('copied.cpp', out.stdout)
        self.assertNotIn('clean.txt', out.stdout)
        self.assertIn('length 40', out.stdout)
        self.assertIn('controls=1/1', out.stdout)

    def test_clean_tree_passes_with_controls_found(self):
        retail = Tree({'game.bin': os.urandom(100_000)})
        pub = Tree({'a.txt': os.urandom(10_000)})
        out = run('taint_scan.py', '--published', pub.root, '--retail', retail.root / 'game.bin')
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn('TAINT PASS', out.stdout)

    def test_generated_code_fails_only_on_long_runs(self):
        line = b'ctx.r3.u64 = PPC_LOAD_U32(ctx.r31.u32 + 8);\n'
        body = b''.join(b'// fn %d\n' % i + os.urandom(64).hex().encode() + b'\n' + line for i in range(300))
        gen = Tree({'ppc_recomp.0.cpp': body})
        listing = Tree({'gen.txt': f'{gen.root / "ppc_recomp.0.cpp"}\n'})
        retail = Tree({'game.bin': os.urandom(50_000)})
        short = Tree({'tool.py': b'PATTERN = "' + line.strip() + b'"\n'})
        out = run('taint_scan.py', '--published', short.root, '--retail', retail.root / 'game.bin',
                  '--generated-list', listing.root / 'gen.txt')
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn('NOTE', out.stdout)
        copied = Tree({'copied.cpp': body[1000:1300]})
        out = run('taint_scan.py', '--published', copied.root, '--retail', retail.root / 'game.bin',
                  '--generated-list', listing.root / 'gen.txt')
        self.assertEqual(out.returncode, 1, out.stdout + out.stderr)
        self.assertIn('FINDING', out.stdout)

    def test_counting_runs_are_notes_not_findings(self):
        table = b''.join((600 + i).to_bytes(2, 'big') for i in range(40))  # a font's glyph-index table
        retail = Tree({'game.bin': os.urandom(20_000) + table + os.urandom(20_000)})
        pub = Tree({'font.ttf': os.urandom(3000) + table + os.urandom(3000)})
        out = run('taint_scan.py', '--published', pub.root, '--retail', retail.root / 'game.bin')
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn('counting run', out.stdout)

    def test_a_path_scoped_name_allowance_covers_only_that_name_in_that_file(self):
        name = b'TASK_DO_SOMETHING_WITH_A_LONG_NAME_XY'                      # 37 bytes, as a game function name
        retail = Tree({'game.bin': os.urandom(20_000) + b'\0' + name + b'\0' + os.urandom(20_000)})
        digest = hashlib.sha256(name).hexdigest()
        allow = Tree({'allow.txt': f'{digest} path=runtime/autopilot.h,name a game function name (decision 20)\n'})
        quoted = b'// calls ' + name + b' here\n' + os.urandom(500).hex().encode()
        ok = Tree({'runtime/autopilot.h': quoted})
        out = run('taint_scan.py', '--published', ok.root, '--retail', retail.root / 'game.bin', '--allow', allow.root / 'allow.txt')
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        other = Tree({'runtime/other.h': quoted})                           # the same name in another file: a finding
        out = run('taint_scan.py', '--published', other.root, '--retail', retail.root / 'game.bin', '--allow', allow.root / 'allow.txt')
        self.assertEqual(out.returncode, 1, out.stdout + out.stderr)
        longer = Tree({'runtime/autopilot.h': b'// x\n\0' + name + b'\0\n'})    # more than the name: another run, a finding
        out = run('taint_scan.py', '--published', longer.root, '--retail', retail.root / 'game.bin', '--allow', allow.root / 'allow.txt')
        self.assertEqual(out.returncode, 1, out.stdout + out.stderr)

    def test_a_file_level_allowance_covers_only_that_files_exact_bytes_at_that_path(self):
        retail_bytes = os.urandom(200_000)
        retail = Tree({'game.bin': retail_bytes})
        dll = os.urandom(3000) + retail_bytes[50_000:50_064] + os.urandom(3000)     # an upstream file sharing a table
        allow = Tree({'allow.txt': f'file={hashlib.sha256(dll).hexdigest()} path=python/lib.dll byte-identical to an archive member\n'})
        ok = Tree({'builder/python/lib.dll': dll})
        out = run('taint_scan.py', '--published', ok.root, '--retail', retail.root / 'game.bin', '--allow', allow.root / 'allow.txt')
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        changed = dll[:10] + bytes([dll[10] ^ 1]) + dll[11:]
        for name, files in (('another path', {'builder/other/lib.dll': dll}), ('one byte changed', {'builder/python/lib.dll': changed})):
            with self.subTest(name):
                out = run('taint_scan.py', '--published', Tree(files).root, '--retail', retail.root / 'game.bin',
                          '--allow', allow.root / 'allow.txt')
                self.assertEqual(out.returncode, 1, out.stdout + out.stderr)

    def test_missing_retail_input_refuses(self):
        pub = Tree({'a.txt': 'x'})
        self.assertEqual(run('taint_scan.py', '--published', pub.root, '--retail', pub.root / 'nope.iso').returncode, 2)


if __name__ == '__main__':
    unittest.main()
