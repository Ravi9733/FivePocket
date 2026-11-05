#!/usr/bin/env python3
"""Mutant check for shader_toolchain.py: copies of the module with one rule changed; test_shader_toolchain.py (beside
this file) must fail on each. First a control. Exits 1 if the control fails, a mutant survives or no longer applies.
The module itself is not touched."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'shader_toolchain.py'
TESTS = HERE / 'test_shader_toolchain.py'
MUTANTS = [
    ('Windows programs carry no .exe', "    exe = '.exe' if key == 'win32' else ''\n", "    exe = ''\n"),
    ('every host\'s programs carry .exe', "    exe = '.exe' if key == 'win32' else ''\n", "    exe = '.exe'\n"),
    ('the validator is the Mac\'s on every host', "Path(ndk) / 'shader-tools' / NDK_HOST_TAGS[key] / ('spirv-val' + exe)", "Path(ndk) / 'shader-tools' / 'darwin-x86_64' / ('spirv-val' + exe)"),
    ('the validator is looked for in the host tools', "Path(ndk) / 'shader-tools' / NDK_HOST_TAGS[key] / ('spirv-val' + exe)", "Path(host_tools) / 'shader-tools' / NDK_HOST_TAGS[key] / ('spirv-val' + exe)"),
    ('the validator carries no .exe on Windows', "/ ('spirv-val' + exe),", "/ 'spirv-val',"),
    ('the checker is the translator', "'preflight': xenos / 'main' / ('XenosShaderPreflight' + exe),", "'preflight': xenos / 'main' / ('XenosRecomp' + exe),"),
    ('the compiler is named without its version', "Path(host_tools) / 'dxc' / 'bin' / ('dxc-3.7' + exe)", "Path(host_tools) / 'dxc' / 'bin' / ('dxc' + exe)"),
    ('the library folder is the compiler\'s program folder', "'compiler_library_directory': Path(host_tools) / 'dxc' / 'lib',", "'compiler_library_directory': Path(host_tools) / 'dxc' / 'bin',"),
    ('the pack\'s header is another state\'s', "'common_header': xenos / 'main' / 'shader_common.h',", "'common_header': xenos / 'reglod' / 'shader_common.h',"),
    ('the gradient translator is the main one', "gradient_translator=xenos / 'gradient' / ('XenosRecomp' + exe)", "gradient_translator=xenos / 'main' / ('XenosRecomp' + exe)"),
    ('the level-register translator is the gradient one', "reglod_translator=xenos / 'reglod' / ('XenosRecomp' + exe)", "reglod_translator=xenos / 'gradient' / ('XenosRecomp' + exe)"),
    ('the level-register header is the main one', "reglod_header=xenos / 'reglod' / 'shader_common.h'", "reglod_header=xenos / 'main' / 'shader_common.h'"),
    ('the gradient header is the level-register one', "gradient_header=xenos / 'gradient' / 'shader_common.h'", "gradient_header=xenos / 'reglod' / 'shader_common.h'"),
    ('the four further fields are always there', "    if extras:\n        found.update(", "    if True:\n        found.update("),
    ('the four further fields are never there', "    if extras:\n        found.update(", "    if False:\n        found.update("),
    ('an unknown host gets the Mac\'s names', "    if key not in NDK_HOST_TAGS:\n        raise ValueError(f'no shader toolchain is named for the host {platform}')\n    return key", "    return key if key in NDK_HOST_TAGS else 'darwin'"),
    ('only the first Linux name is known', "    key = 'linux' if platform.startswith('linux') else platform\n", "    key = platform\n"),
    ('a file beside both folders is taken', "    if toolchain is not None and host_tools is None and ndk is None:\n", "    if toolchain is not None:\n"),
    ('one folder is enough beside nothing', "    if toolchain is None and host_tools is not None and ndk is not None:\n", "    if toolchain is None and (host_tools is not None or ndk is not None):\n"),
    ('the folders win over a file', "    if toolchain is None and host_tools is not None and ndk is not None:\n", "    if host_tools is not None and ndk is not None:\n"),
    ('the further fields are not handed on by load', "        return names(host_tools, ndk, extras, platform)\n", "        return names(host_tools, ndk, False, platform)\n"),
    ('load names this host whatever is asked', "        return names(host_tools, ndk, extras, platform)\n", "        return names(host_tools, ndk, extras)\n"),
    ('the given environment is changed in place', "    env = dict(os.environ if base is None else base)\n", "    env = os.environ if base is None else base\n"),
    ('the process\'s environment is used whatever is given', "    env = dict(os.environ if base is None else base)\n", "    env = dict(os.environ)\n"),
    ('macOS gets Linux\'s variable', "        env['DYLD_LIBRARY_PATH'] = str(directory)\n", "        env['LD_LIBRARY_PATH'] = str(directory)\n"),
    ('Linux gets macOS\'s variable', "    else:\n        env['LD_LIBRARY_PATH'] = str(directory)\n", "    else:\n        env['DYLD_LIBRARY_PATH'] = str(directory)\n"),
    ('Windows gets a loader variable instead of the PATH', "    elif key == 'win32':\n", "    elif key == 'never':\n"),
    ('on Windows the folder comes after the PATH', "        env['PATH'] = str(directory) + ';' + env['PATH'] if env.get('PATH') else str(directory)\n", "        env['PATH'] = env['PATH'] + ';' + str(directory) if env.get('PATH') else str(directory)\n"),
    ('on Windows the PATH is replaced', "        env['PATH'] = str(directory) + ';' + env['PATH'] if env.get('PATH') else str(directory)\n", "        env['PATH'] = str(directory)\n"),
    ('an empty PATH leaves a stray separator', "        env['PATH'] = str(directory) + ';' + env['PATH'] if env.get('PATH') else str(directory)\n", "        env['PATH'] = str(directory) + ';' + env.get('PATH', '')\n"),
    ('the Windows separator is the Mac\'s', "        env['PATH'] = str(directory) + ';' + env['PATH'] if env.get('PATH') else str(directory)\n", "        env['PATH'] = str(directory) + ':' + env['PATH'] if env.get('PATH') else str(directory)\n"),
]


def run(tool):
    return subprocess.run([sys.executable, '-B', str(TESTS)], capture_output=True, text=True, env=dict(os.environ, SHADER_TOOLCHAIN=str(tool)))


def main() -> int:
    source = TOOL.read_text(encoding='utf-8')
    if run(TOOL).returncode != 0:
        print('CONTROL FAILED: the tests do not pass on the module as it is')
        return 1
    print('control: the tests pass on the module as it is', flush=True)
    killed = 0
    with tempfile.TemporaryDirectory() as folder:
        for index, (what, old, new) in enumerate(MUTANTS, 1):
            if source.count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({source.count(old)} matches): {what}')
                continue
            copy = Path(folder) / f'mutant_{index}.py'
            copy.write_text(source.replace(old, new), encoding='utf-8', newline='\n')
            if run(copy).returncode == 0:
                print(f'mutant {index:3d} SURVIVED: {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed: {what}')
    print(f'{killed} of {len(MUTANTS)} mutants killed')
    return 0 if killed == len(MUTANTS) else 1


if __name__ == '__main__':
    sys.exit(main())
