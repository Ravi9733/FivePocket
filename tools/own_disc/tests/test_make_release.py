#!/usr/bin/env python3
"""make_release.py: the inventory of a package folder, written and read back by the front end's own reader; what is
listed, what is left out, what refuses the run. Synthetic packages, no game data. MAKE_RELEASE names another copy of
the tool (the mutant runner beside this file)."""
import contextlib
import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
TOOL = Path(os.environ.get('MAKE_RELEASE', HERE.parent / 'make_release.py'))
sys.path.insert(0, str(HERE.parent))
spec = importlib.util.spec_from_file_location('make_release_under_test', TOOL)
mr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mr)
import builder                     # the front end the inventory is for
import disc_builder as db
import shader_toolchain            # where the shader stages look for the compiler the builder fetches

PINS = Path(os.environ.get('DXC_PINS', HERE.parent / 'windows' / 'dxc-v1.9.2607.json'))      # the tracked pins of Microsoft's release
DXC_FILES = (('bin/x64/dxc.exe', 'host-tools/dxc/bin/dxc-3.7.exe', b'SYNTHETIC shader compiler'), ('bin/x64/dxcompiler.dll', 'host-tools/dxc/lib/dxcompiler.dll', b'SYNTHETIC library'))

NDK = {'url': 'https://dl.google.com/android/repository/android-ndk-r27c-windows.zip', 'sha256': 'ab' * 32, 'size': 10,
       'directory': 'android-ndk-r27c', 'unpacked_bytes': 100}
FILES = {'builder.exe': b'launcher', 'python/python.exe': b'python', 'python/Lib/os.py': b'# os', 'tools/own_disc/builder.py': b'# front end',
         'profile/profile.json': b'{}', 'host-tools/xenos/main/XenosRecomp.exe': b'translator', 'host-tools/notes.txt': b'notes',
         'bin/run': b'#!/bin/sh\n'}


class Release(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.package = Path(tmp.name).resolve() / 'package'
        for name, data in FILES.items():
            path = self.package / name
            path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
        (self.package / 'bin/run').chmod(0o755)
        (self.package / 'tools/own_disc/__pycache__').mkdir()
        (self.package / 'tools/own_disc/__pycache__/builder.cpython-312.pyc').write_bytes(b'cache')
        self.out = self.package / 'release.json'

    def make(self, host=None, **changed):
        given = dict(package=self.package, host=host or builder.host_id(), profile='profile/profile.json', python='python/python.exe',
                     ndk=dict(NDK), min_free_bytes=1 << 30, build_time='about half an hour', output=self.out)
        given.update(changed)
        return mr.make(**given)

    def test_the_inventory_is_what_the_front_end_reads(self):
        release = self.make()
        on_disk = json.loads(self.out.read_text(encoding='utf-8'))
        self.assertEqual(on_disk, release)
        self.assertEqual([r['path'] for r in release['files']], sorted(FILES))            # every file, by name; no cache, not itself
        self.assertEqual({r['path']: r['sha256'] for r in release['files']}, {n: db.sha256_file(self.package / n) for n in FILES})
        self.assertEqual((release['schema'], release['host'], release['profile'], release['python']),
                         ('builder-release/1', builder.host_id(), 'profile/profile.json', 'python/python.exe'))
        self.assertEqual(release['ndk'], dict(NDK, license_url='https://developer.android.com/ndk/downloads'))
        self.assertEqual(release['estimates'], {'min_free_bytes': 1 << 30, 'build_time': 'about half an hour'})
        loaded, root = builder.load_release(self.out)                                      # the consumer's own reader
        self.assertEqual((loaded, root), (release, self.package))
        self.assertEqual(builder.ndk_spec(loaded), release['ndk'])
        (self.package / 'host-tools/notes.txt').write_bytes(b'changed')
        with self.assertRaisesRegex(builder.Refusal, 'missing or changed'):
            builder.load_release(self.out)

    def test_a_package_without_its_own_python(self):
        release = self.make(python=None)
        self.assertNotIn('python', release)
        self.assertNotIn('python', builder.load_release(self.out)[0])

    def test_target_profiles_are_written_and_must_be_files_of_the_package(self):
        release = self.make(targets={'ios': 'profile/profile.json'})
        self.assertEqual(release['profiles'], {'ios': 'profile/profile.json'})
        self.assertEqual(builder.load_release(self.out)[0]['profiles'], {'ios': 'profile/profile.json'})
        self.out.unlink()
        with self.assertRaisesRegex(ValueError, 'duo profile is not a file'):
            self.make(targets={'duo': 'profile/duo.json'})
        with self.assertRaisesRegex(ValueError, 'unknown target'):
            self.make(targets={'tv': 'profile/profile.json'})
        self.assertFalse(self.out.exists())

    def test_programs_by_mode_here_and_by_suffix_in_a_windows_package(self):
        here = {r['path'] for r in self.make()['files'] if r.get('executable')}
        self.assertEqual(here, {'bin/run'})
        self.out.unlink()
        windows = self.make(host='win32-x86_64')
        self.assertEqual({r['path'] for r in windows['files'] if r.get('executable')},
                         {'builder.exe', 'python/python.exe', 'host-tools/xenos/main/XenosRecomp.exe'})
        self.assertEqual(json.loads(self.out.read_text(encoding='utf-8'))['host'], 'win32-x86_64')
        self.assertTrue(all(set(r) <= {'path', 'sha256', 'executable'} for r in windows['files']))

    def test_no_time_estimate_no_field(self):
        self.assertEqual(self.make(build_time=None)['estimates'], {'min_free_bytes': 1 << 30})

    def refused(self, message, **changed):
        with self.assertRaisesRegex(ValueError, message):
            self.make(**changed)
        self.assertFalse(self.out.exists())

    def test_what_refuses_the_run_and_writes_nothing(self):
        self.refused('profile is not a file of the package', profile='profile/other.json')
        self.refused('Python is not a file of the package', python='python/python3.exe')
        self.refused('not pinned as the front end wants it', ndk=dict(NDK, url='https://example.org/android-ndk.zip'))
        self.refused('not pinned as the front end wants it', ndk=dict(NDK, sha256='zz'))
        for count in (0, -5, 1.5, None):
            self.refused('positive number of bytes', min_free_bytes=count)
        self.refused('package folder is not there', package=self.package / 'absent', output=self.package / 'absent' / 'release.json')
        self.refused('lies at the package root', output=self.package / 'tools' / 'release.json')
        (self.package / 'host-tools' / 'link.txt').symlink_to(self.package / 'host-tools' / 'notes.txt')
        self.refused('a link is in the package: host-tools/link.txt')
        (self.package / 'host-tools' / 'link.txt').unlink()
        (self.package / 'host-tools' / 'folder-link').symlink_to(self.package / 'bin', target_is_directory=True)
        self.refused('a link is in the package: host-tools/folder-link')
        (self.package / 'host-tools' / 'folder-link').unlink()
        (self.package / 'host-tools' / 'bad:name.txt').write_bytes(b'x')
        self.refused('front end would refuse this name: host-tools/bad:name.txt')

    def test_an_empty_package_is_refused(self):
        empty = self.package.parent / 'empty'; empty.mkdir()
        with self.assertRaisesRegex(ValueError, 'package is empty'):
            self.make(package=empty, output=empty / 'release.json')

    def test_an_existing_inventory_is_never_written_over(self):
        self.out.write_text('kept', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'exists already'):
            self.make()
        self.assertEqual(self.out.read_text(encoding='utf-8'), 'kept')

    def test_an_inventory_the_front_end_refuses_does_not_stay(self):
        with mock.patch.object(mr.builder, 'load_release', side_effect=builder.Refusal('not for this reader')):
            with self.assertRaisesRegex(ValueError, 'front end refuses the inventory just written: not for this reader'):
                self.make()
        self.assertFalse(self.out.exists())
        # for another host the reader cannot run here: it is not asked
        with mock.patch.object(mr.builder, 'load_release', side_effect=AssertionError('not this host')):
            self.make(host='win32-x86_64' if builder.host_id() != 'win32-x86_64' else 'linux-x86_64')
        self.assertTrue(self.out.exists())

    def dxc(self):
        """A synthetic archive of a shader compiler release beside the package, and its pins."""
        import hashlib, zipfile
        archive = self.package.parent / 'dxc-fixture.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            for member, _, data in DXC_FILES:
                z.writestr(zipfile.ZipInfo(member.replace('/', '\\')), data)
        return archive, dict(url='https://github.com/microsoft/DirectXShaderCompiler/releases/download/v0.0.fixture/dxc_fixture.zip',
                             sha256=db.sha256_file(archive), size=archive.stat().st_size,
                             license_url='https://github.com/microsoft/DirectXShaderCompiler/blob/v0.0.fixture/LICENSE.TXT',
                             files=[dict(member=member, path=place, size=len(data), sha256=hashlib.sha256(data).hexdigest()) for member, place, data in DXC_FILES])

    def test_the_fetched_shader_compiler_is_pinned_and_not_in_the_package(self):
        archive, pins = self.dxc()
        release = self.make(dxc=pins)
        self.assertEqual(release['dxc'], pins)
        self.assertEqual([r['path'] for r in release['files']], sorted(FILES))            # none of its files is listed
        loaded, root = builder.load_release(self.out)
        self.assertEqual(builder.dxc_spec(loaded), pins)
        with self.assertRaisesRegex(builder.Refusal, 'shader compiler missing'):
            builder.existing_dxc(loaded, root)
        # the consumer: the front end's fetch puts the files where the pins say, and the inventory still reads
        with contextlib.redirect_stdout(io.StringIO()):
            builder.fetch_dxc(loaded, root, self.package.parent / 'cache', True, archive)
        builder.existing_dxc(loaded, root)
        self.assertEqual([(self.package / place).read_bytes() for _, place, _ in DXC_FILES], [data for _, _, data in DXC_FILES])
        self.assertEqual(builder.load_release(self.out)[0], release)
        self.assertNotIn('dxc', self.make(output=self.package / 'second.json'))            # without pins the field is not written

    def test_a_shader_compiler_the_front_end_would_refuse_or_one_in_the_package_is_refused(self):
        _, pins = self.dxc()
        self.refused('shader compiler is not pinned as the front end wants it: shader compiler URL', dxc=dict(pins, url='https://example.org/dxc.zip'))
        self.refused('shader compiler is not pinned as the front end wants it: shader compiler files not listed', dxc=dict(pins, files=[]))
        self.refused('shader compiler is not pinned as the front end wants it', dxc={})
        held = self.package / 'host-tools/dxc/lib/DXCompiler.DLL'                           # another spelling of a pinned place
        held.parent.mkdir(parents=True); held.write_bytes(b'copied in by hand')
        self.refused('package holds a file the builder is to fetch from its publisher: host-tools/dxc/lib/dxcompiler.dll', dxc=pins)
        self.make()                                                                         # without pins the same package is written: the file is payload then

    def test_the_tracked_pins_are_what_the_front_end_and_the_shader_stages_expect(self):
        pins = json.loads(PINS.read_text(encoding='utf-8'))
        self.assertIs(builder.dxc_spec({'dxc': pins}), pins)
        self.assertEqual((pins['url'], pins['size'], pins['sha256']),
                         ('https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/dxc_2026_07_29.zip', 41625275,
                          'a1dfb116ba3eeae6a1582291b53a8e7bf65ad760676bd3194685c8f7367cd241'))
        self.assertTrue(pins['license_url'].startswith('https://github.com/microsoft/DirectXShaderCompiler/blob/v1.9.2607/'))
        places = {record['member']: record['path'] for record in pins['files']}
        names = shader_toolchain.names('host-tools', 'ndk', platform='win32')               # a Windows package's shader stages
        self.assertEqual(places['bin/x64/dxc.exe'], Path(names['dxc']).as_posix())
        libraries = Path(names['compiler_library_directory']).as_posix()
        self.assertEqual((places['bin/x64/dxcompiler.dll'], places['bin/x64/dxil.dll']), (libraries + '/dxcompiler.dll', libraries + '/dxil.dll'))
        self.assertEqual(len(places), len(pins['files']))                                   # no member taken twice
        terms = sorted(member for member in places if '/' not in member)
        self.assertEqual(terms, ['LICENCE-MIT.txt', 'LICENSE-LLVM.txt', 'LICENSE-MS.txt'])   # the publisher's terms travel with its files
        for member in terms:
            self.assertEqual(places[member], 'host-tools/dxc/' + member)
        self.assertEqual(sorted(places), sorted(terms + ['bin/x64/dxc.exe', 'bin/x64/dxcompiler.dll', 'bin/x64/dxil.dll']))

    def test_a_macos_release_names_the_ndk_folder_inside_the_disk_image(self):
        dmg = dict(NDK, url='https://dl.google.com/android/repository/android-ndk-r27c-darwin.dmg')
        with self.assertRaisesRegex(ValueError, 'image root not declared'):
            self.make(host='darwin-arm64', ndk=dmg)
        self.assertFalse(self.out.exists())
        release = self.make(host='darwin-arm64', ndk=dict(dmg, image_root='AndroidNDK12479018.app/Contents/NDK'))
        self.assertEqual(release['ndk']['image_root'], 'AndroidNDK12479018.app/Contents/NDK')
        self.out.unlink()
        argv = ['--package', str(self.package), '--host', 'darwin-arm64', '--profile', 'profile/profile.json', '--python', 'python/python.exe',
                '--ndk-url', dmg['url'], '--ndk-sha256', NDK['sha256'], '--ndk-size', '10', '--ndk-directory', NDK['directory'],
                '--ndk-unpacked-bytes', '100', '--min-free-bytes', str(1 << 30), '--ndk-image-root', 'AndroidNDK12479018.app/Contents/NDK']
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(mr.main(argv), 0)
        self.assertEqual(json.loads(self.out.read_text(encoding='utf-8'))['ndk']['image_root'], 'AndroidNDK12479018.app/Contents/NDK')

    def test_the_command_line(self):
        argv = ['--package', str(self.package), '--host', builder.host_id(), '--profile', 'profile/profile.json', '--python', 'python/python.exe',
                '--ndk-url', NDK['url'], '--ndk-sha256', NDK['sha256'], '--ndk-size', '10', '--ndk-directory', NDK['directory'],
                '--ndk-unpacked-bytes', '100', '--min-free-bytes', str(1 << 30)]
        with contextlib.redirect_stdout(io.StringIO()) as said:
            self.assertEqual(mr.main(argv), 0)
        self.assertIn(f'RELEASE {len(FILES)} files, 1 programs', said.getvalue())
        release = json.loads(self.out.read_text(encoding='utf-8'))                         # the default output: the package root
        self.assertEqual((release['ndk']['size'], release['ndk']['unpacked_bytes'], release['estimates']), (10, 100, {'min_free_bytes': 1 << 30}))
        with contextlib.redirect_stderr(io.StringIO()) as said:
            self.assertEqual(mr.main(argv), 2)                                             # it is there now
        self.assertIn('RELEASE_REFUSED: the inventory exists already', said.getvalue())
        self.out.unlink()
        elsewhere = self.package / 'named.json'
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(mr.main(argv + ['--output', str(elsewhere), '--build-time', 'an hour']), 0)
        self.assertEqual(json.loads(elsewhere.read_text(encoding='utf-8'))['estimates']['build_time'], 'an hour')
        self.assertNotIn('dxc', json.loads(elsewhere.read_text(encoding='utf-8')))
        # the pins of a fetched shader compiler come from a file
        _, pins = self.dxc()
        named = self.package.parent / 'pins.json'; named.write_text(json.dumps(pins), encoding='utf-8')
        third = self.package / 'with-pins.json'
        with contextlib.redirect_stdout(io.StringIO()) as said:
            self.assertEqual(mr.main(argv + ['--output', str(third), '--dxc', str(named)]), 0)
        self.assertIn(f'{len(DXC_FILES)} shader compiler files fetched by the builder', said.getvalue())
        self.assertEqual(json.loads(third.read_text(encoding='utf-8'))['dxc'], pins)
        for content, message in (('[1, 2]', 'pins file holds no record'), ('not json', 'RELEASE_REFUSED'), (None, 'RELEASE_REFUSED')):
            fourth = self.package / 'refused.json'
            if content is None: named.unlink()
            else: named.write_text(content, encoding='utf-8')
            with contextlib.redirect_stderr(io.StringIO()) as said:
                self.assertEqual(mr.main(argv + ['--output', str(fourth), '--dxc', str(named)]), 2)
            self.assertIn(message, said.getvalue()); self.assertFalse(fourth.exists())


if __name__ == '__main__':
    unittest.main()
