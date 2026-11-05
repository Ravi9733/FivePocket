#!/usr/bin/env python3
"""shader_toolchain.py: the toolchain's names from a host-tools folder and an NDK on each host, the two ways a tool
is given its toolchain, and the environment in which the programs find the compiler library. No game data.
SHADER_TOOLCHAIN names another copy of the module (the mutant runner beside this file)."""
import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path, PurePosixPath, PureWindowsPath

HERE = Path(__file__).resolve().parent
MODULE = Path(os.environ.get('SHADER_TOOLCHAIN', HERE.parent / 'shader_toolchain.py'))
spec = importlib.util.spec_from_file_location('shader_toolchain_under_test', MODULE)
st = importlib.util.module_from_spec(spec)
spec.loader.exec_module(st)


class Names(unittest.TestCase):
    def test_the_six_fields_of_the_pack_on_a_mac(self):
        self.assertEqual(st.names(PurePosixPath('/h'), PurePosixPath('/n'), platform='darwin'), {
            'translator': '/h/xenos/main/XenosRecomp', 'preflight': '/h/xenos/main/XenosShaderPreflight',
            'common_header': '/h/xenos/main/shader_common.h', 'dxc': '/h/dxc/bin/dxc-3.7',
            'spirv_validator': '/n/shader-tools/darwin-x86_64/spirv-val', 'compiler_library_directory': '/h/dxc/lib'})

    def test_programs_carry_exe_on_windows_and_the_validator_is_that_host_s(self):
        got = st.names(PurePosixPath('/h'), PurePosixPath('/n'), extras=True, platform='win32')
        self.assertEqual(got, {
            'translator': '/h/xenos/main/XenosRecomp.exe', 'preflight': '/h/xenos/main/XenosShaderPreflight.exe',
            'common_header': '/h/xenos/main/shader_common.h', 'dxc': '/h/dxc/bin/dxc-3.7.exe',
            'spirv_validator': '/n/shader-tools/windows-x86_64/spirv-val.exe', 'compiler_library_directory': '/h/dxc/lib',
            'gradient_translator': '/h/xenos/gradient/XenosRecomp.exe', 'gradient_header': '/h/xenos/gradient/shader_common.h',
            'reglod_translator': '/h/xenos/reglod/XenosRecomp.exe', 'reglod_header': '/h/xenos/reglod/shader_common.h'})

    def test_the_fix_tables_name_the_folders_build_host_tools_writes(self):
        # build_host_tools.ps1 writes xenos\v41 (the base tree's translator), xenos\coissue and xenos\coissue-reglod
        got = st.names(PurePosixPath('/h'), PurePosixPath('/n'), platform='win32', fixes=True)
        self.assertEqual(got, {
            'v41_translator': '/h/xenos/v41/XenosRecomp.exe', 'v41_header': '/h/xenos/v41/shader_common.h',
            'coissue_translator': '/h/xenos/coissue/XenosRecomp.exe', 'coissue_header': '/h/xenos/coissue/shader_common.h',
            'coissue_reglod_translator': '/h/xenos/coissue-reglod/XenosRecomp.exe',
            'coissue_reglod_header': '/h/xenos/coissue-reglod/shader_common.h',
            'reglod_translator': '/h/xenos/reglod/XenosRecomp.exe', 'reglod_header': '/h/xenos/reglod/shader_common.h',
            'dxc': '/h/dxc/bin/dxc-3.7.exe', 'spirv_validator': '/n/shader-tools/windows-x86_64/spirv-val.exe',
            'compiler_library_directory': '/h/dxc/lib'})

    def test_linux_by_any_of_its_names(self):
        for platform in ('linux', 'linux2'):
            got = st.names(PurePosixPath('/h'), PurePosixPath('/n'), platform=platform)
            self.assertEqual((got['spirv_validator'], got['dxc']), ('/n/shader-tools/linux-x86_64/spirv-val', '/h/dxc/bin/dxc-3.7'))

    def test_the_fields_are_the_declared_ones(self):
        self.assertEqual(tuple(st.names(PurePosixPath('/h'), PurePosixPath('/n'), platform='darwin')), st.BASE)
        self.assertEqual(tuple(st.names(PurePosixPath('/h'), PurePosixPath('/n'), extras=True, platform='darwin')), st.BASE + st.EXTRAS)

    def test_a_host_without_an_ndk_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'no shader toolchain'):
            st.names(PurePosixPath('/h'), PurePosixPath('/n'), platform='sunos5')


class Load(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.file = Path(tmp.name) / 'toolchain.json'
        self.file.write_text(json.dumps({'translator': 'from the file'}), encoding='utf-8')

    def test_the_file_as_it_is(self):
        self.assertEqual(st.load(self.file, None, None), {'translator': 'from the file'})

    def test_the_names_from_the_two_folders(self):
        self.assertEqual(st.load(None, PurePosixPath('/h'), PurePosixPath('/n'), platform='win32'), st.names(PurePosixPath('/h'), PurePosixPath('/n'), platform='win32'))
        self.assertEqual(st.load(None, PurePosixPath('/h'), PurePosixPath('/n'), extras=True, platform='darwin'),
                         st.names(PurePosixPath('/h'), PurePosixPath('/n'), extras=True, platform='darwin'))

    def test_both_ways_neither_or_half_of_one_is_refused(self):
        for given in ((None, None, None), (self.file, Path('/h'), Path('/n')), (None, Path('/h'), None), (None, None, Path('/n')),
                      (self.file, Path('/h'), None), (self.file, None, Path('/n'))):
            with self.assertRaisesRegex(ValueError, 'toolchain refused'):
                st.load(*given)


class LibraryEnvironment(unittest.TestCase):
    BASE = {'PATH': 'first;second', 'OTHER': '1'}

    def test_mac_names_the_loader_s_variable_and_leaves_the_rest(self):
        self.assertEqual(st.library_environment('/lib', self.BASE, 'darwin'), {'PATH': 'first;second', 'OTHER': '1', 'DYLD_LIBRARY_PATH': '/lib'})

    def test_linux_names_its_own(self):
        self.assertEqual(st.library_environment('/lib', self.BASE, 'linux'), {'PATH': 'first;second', 'OTHER': '1', 'LD_LIBRARY_PATH': '/lib'})

    def test_windows_puts_the_folder_in_front_of_the_path(self):
        self.assertEqual(st.library_environment(PureWindowsPath(r'D:\h\dxc\lib'), self.BASE, 'win32'), {'PATH': r'D:\h\dxc\lib;first;second', 'OTHER': '1'})
        self.assertEqual(st.library_environment(r'D:\lib', {'OTHER': '1'}, 'win32'), {'OTHER': '1', 'PATH': r'D:\lib'})
        self.assertEqual(st.library_environment(r'D:\lib', {'PATH': ''}, 'win32'), {'PATH': r'D:\lib'})

    def test_the_given_environment_is_not_changed_and_the_process_s_own_is_the_default(self):
        before = dict(self.BASE)
        st.library_environment('/lib', self.BASE, 'darwin')
        self.assertEqual(self.BASE, before)
        mine = st.library_environment('/lib', platform='linux')
        self.assertEqual({k: v for k, v in mine.items() if k != 'LD_LIBRARY_PATH'}, {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'})
        self.assertEqual(mine['LD_LIBRARY_PATH'], '/lib')
        self.assertIsNot(mine, os.environ)


if __name__ == '__main__':
    unittest.main()
