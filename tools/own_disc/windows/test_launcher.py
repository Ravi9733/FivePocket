#!/usr/bin/env python3
"""builder.exe (builder_launcher.c) on Windows: what it starts, with which arguments and environment, and what it
answers when the package is not whole.

BUILDER_LAUNCHER names the compiled program (the mutant runner beside this file names its copies the same way). Each
case builds a small package in a new temporary folder whose name holds a space and an accented letter: the program,
a python\\ folder (this Python's own program and library files, pointed at this installation's standard library by a
._pth file, so nothing is installed and nothing large is copied) and a stand-in front end that writes down what
reached it. Skipped on other hosts: the program is a Windows program.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

LAUNCHER = os.environ.get('BUILDER_LAUNCHER')
STAND_IN = r'''
import json, os, sys
names = ('BUILDER_PACKAGE_ROOT', 'BUILDER_PYTHON', 'BUILDER_HOST_TOOLS', 'PYTHONUTF8', 'PYTHONHOME', 'PYTHONPATH', 'PYTHONNOUSERSITE', 'PATH')
seen = {'argv': sys.argv[1:], 'env': {n: os.environ.get(n) for n in names}, 'utf8_mode': sys.flags.utf8_mode,
        'bytecode': sys.dont_write_bytecode, 'program': sys.executable}
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'seen.json'), 'w', encoding='utf-8') as out:
    json.dump(seen, out)
print('front end reached')
code = 0
if '--exit' in sys.argv:
    code = int(sys.argv[sys.argv.index('--exit') + 1])
sys.exit(code)
'''


@unittest.skipUnless(sys.platform == 'win32' and LAUNCHER, 'a Windows program: set BUILDER_LAUNCHER on Windows')
class Launcher(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='launcher-')
        self.addCleanup(tmp.cleanup)
        self.package = Path(tmp.name) / 'paquet é 1'
        (self.package / 'python').mkdir(parents=True)
        (self.package / 'tools' / 'own_disc').mkdir(parents=True)
        self.exe = self.package / 'builder.exe'
        shutil.copyfile(LAUNCHER, self.exe)
        home = Path(sys.base_prefix)
        for item in home.iterdir():                      # the program and the library files beside it; not Lib, not DLLs
            if item.is_file() and item.suffix.lower() in ('.exe', '.dll') and item.name.lower() != 'pythonw.exe':
                shutil.copyfile(item, self.package / 'python' / item.name)
        version = f'python{sys.version_info.major}{sys.version_info.minor}'
        (self.package / 'python' / f'{version}._pth').write_text(f'{home}\\{version}.zip\n{home}\\Lib\n{home}\\DLLs\n', encoding='utf-8')
        self.front = self.package / 'tools' / 'own_disc' / 'builder.py'
        self.front.write_text(STAND_IN, encoding='utf-8', newline='\n')

    def run_launcher(self, arguments, env=None, feed=b''):
        return subprocess.run([str(self.exe), *arguments], capture_output=True, input=feed, env=env)

    def seen(self):
        return json.loads((self.front.parent / 'seen.json').read_text(encoding='utf-8'))

    def test_arguments_arrive_as_given(self):
        given = ['build', '--work', r'C:\b w\x', 'with "quotes" inside', 'ends with backslash\\', r'slash before quote \" here',
                 'é and ü', '', r'two \\ inside', '--exit', '0']
        done = self.run_launcher(given)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(self.seen()['argv'], given)

    def test_the_package_s_own_python_starts_the_front_end_in_utf8_mode_without_bytecode(self):
        self.assertEqual(self.run_launcher(['x']).returncode, 0)
        seen = self.seen()
        self.assertEqual(Path(seen['program']), self.package / 'python' / 'python.exe')
        self.assertEqual((seen['utf8_mode'], seen['bytecode']), (1, True))      # (PYTHONUTF8=1 from the launcher, and -B)

    def test_environment_names_the_package_s_folders(self):
        env = dict(os.environ, PYTHONHOME=r'C:\somewhere', PYTHONPATH=r'C:\elsewhere', PATH=r'C:\first;C:\second')
        self.assertEqual(self.run_launcher(['x'], env=env).returncode, 0)
        got = self.seen()['env']
        self.assertEqual(Path(got['BUILDER_PACKAGE_ROOT']), self.package)
        self.assertEqual(Path(got['BUILDER_PYTHON']), self.package / 'python' / 'python.exe')
        self.assertEqual(Path(got['BUILDER_HOST_TOOLS']), self.package / 'host-tools')
        self.assertEqual(got['PYTHONUTF8'], '1')
        self.assertEqual((got['PYTHONHOME'], got['PYTHONPATH']), (None, None))
        self.assertEqual(got['PYTHONNOUSERSITE'], '1')              # no package folder of the player's profile, also for the tools started later
        self.assertEqual(got['PATH'], r'C:\first;C:\second')       # the caller's PATH as it was

    def test_the_front_end_s_exit_code_is_handed_through(self):
        for code in (0, 2, 3, 77):
            self.assertEqual(self.run_launcher(['--exit', str(code)]).returncode, code)

    def test_a_package_without_its_python_says_so(self):
        shutil.rmtree(self.package / 'python')
        done = self.run_launcher(['build'])
        self.assertEqual(done.returncode, 69)
        self.assertIn('the package is not complete', done.stderr.decode('utf-8', 'replace'))
        self.assertFalse((self.front.parent / 'seen.json').exists())

    def test_a_package_without_its_front_end_says_so(self):
        self.front.unlink()
        done = self.run_launcher(['build'])
        self.assertEqual(done.returncode, 69)
        self.assertIn('the package is not complete', done.stderr.decode('utf-8', 'replace'))

    def test_started_without_arguments_it_waits_for_a_key(self):
        done = self.run_launcher([], feed=b'\r\n')
        self.assertEqual(done.returncode, 0)
        self.assertIn('Press Enter', done.stderr.decode('utf-8', 'replace'))
        self.assertEqual(self.seen()['argv'], [])
        # with arguments it does not ask
        self.assertNotIn('Press Enter', self.run_launcher(['x']).stderr.decode('utf-8', 'replace'))


if __name__ == '__main__':
    unittest.main()
