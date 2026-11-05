#!/usr/bin/env python3
"""The shader toolchain of a run: which programs translate, compile and validate, named from the run's own folders.

build_shader_pack.py and build_shader_extras.py take the toolchain as a JSON file of paths (--toolchain FILE) or,
with --host-tools DIR and --ndk DIR, from here: every path is one of the two folders plus a fixed name. A builder
unpacked anywhere, with its compiler in that machine's cache folder, so needs no file that holds absolute paths.

Also here: the environment in which those programs find the compiler library (dxcompiler) on each host. macOS and
Linux have a variable for it; Windows has none, there the folder goes in front of PATH (a library beside the program
and the system's own folders still come first, by Windows' rule).

Nothing here reads the game.
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

BASE = ('translator', 'preflight', 'common_header', 'dxc', 'spirv_validator', 'compiler_library_directory')
EXTRAS = ('gradient_translator', 'gradient_header', 'reglod_translator', 'reglod_header')
# build_shader_fixes.py: the plain v41 translator (the base tree, the fix lanes' control), v41 with the co-issue order,
# and the register-LOD state with the co-issue order; it also uses reglod_translator / reglod_header and the four BASE
# programs it compiles and validates with
FIXES = ('v41_translator', 'v41_header', 'coissue_translator', 'coissue_header', 'coissue_reglod_translator',
         'coissue_reglod_header', 'reglod_translator', 'reglod_header', 'dxc', 'spirv_validator', 'compiler_library_directory')
NDK_HOST_TAGS = {'darwin': 'darwin-x86_64', 'win32': 'windows-x86_64', 'linux': 'linux-x86_64'}


def host_key(platform: str) -> str:
    key = 'linux' if platform.startswith('linux') else platform
    if key not in NDK_HOST_TAGS:
        raise ValueError(f'no shader toolchain is named for the host {platform}')
    return key


def names(host_tools, ndk, extras: bool = False, platform: str = sys.platform, fixes: bool = False) -> dict:
    """The toolchain's fields from a host-tools folder and an NDK: the six of the pack, with `extras` the four of the
    override tables as well, with `fixes` the eleven of the later fix tables only. Programs carry .exe on Windows; the
    validator is the NDK's own for this host."""
    key = host_key(platform)
    exe = '.exe' if key == 'win32' else ''
    xenos = Path(host_tools) / 'xenos'
    found = {
        'translator': xenos / 'main' / ('XenosRecomp' + exe),
        'preflight': xenos / 'main' / ('XenosShaderPreflight' + exe),
        'common_header': xenos / 'main' / 'shader_common.h',
        'dxc': Path(host_tools) / 'dxc' / 'bin' / ('dxc-3.7' + exe),
        'spirv_validator': Path(ndk) / 'shader-tools' / NDK_HOST_TAGS[key] / ('spirv-val' + exe),
        'compiler_library_directory': Path(host_tools) / 'dxc' / 'lib',
    }
    if extras:
        found.update(gradient_translator=xenos / 'gradient' / ('XenosRecomp' + exe), gradient_header=xenos / 'gradient' / 'shader_common.h',
                     reglod_translator=xenos / 'reglod' / ('XenosRecomp' + exe), reglod_header=xenos / 'reglod' / 'shader_common.h')
    if fixes:
        for state, folder in (('v41', 'v41'), ('coissue', 'coissue'), ('coissue_reglod', 'coissue-reglod'), ('reglod', 'reglod')):
            found[state + '_translator'] = xenos / folder / ('XenosRecomp' + exe)
            found[state + '_header'] = xenos / folder / 'shader_common.h'
        found = {name: found[name] for name in FIXES}
    return {name: str(path) for name, path in found.items()}


def load(toolchain, host_tools, ndk, extras: bool = False, platform: str = sys.platform, fixes: bool = False) -> dict:
    """The toolchain a tool was given, one way or the other: the file as it is, or the names made from the two
    folders. Both ways at once, or neither, or one folder without the other, is refused."""
    if toolchain is not None and host_tools is None and ndk is None:
        return json.loads(Path(toolchain).read_text(encoding='utf-8'))
    if toolchain is None and host_tools is not None and ndk is not None:
        return names(host_tools, ndk, extras, platform, fixes)
    raise ValueError('toolchain refused: give --toolchain FILE, or --host-tools DIR and --ndk DIR')


def library_environment(directory, base=None, platform: str = sys.platform) -> dict:
    """A copy of the environment (this process's, or `base`) in which a started program finds the compiler library
    in `directory`."""
    env = dict(os.environ if base is None else base)
    key = host_key(platform)
    if key == 'darwin':
        env['DYLD_LIBRARY_PATH'] = str(directory)
    elif key == 'win32':
        env['PATH'] = str(directory) + ';' + env['PATH'] if env.get('PATH') else str(directory)
    else:
        env['LD_LIBRARY_PATH'] = str(directory)
    return env
