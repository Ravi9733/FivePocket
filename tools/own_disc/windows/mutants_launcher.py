#!/usr/bin/env python3
"""Mutant check for builder_launcher.c on Windows: the source with one rule changed is compiled to a new program in a
temporary folder, and test_launcher.py (beside this file) must fail on it. First a control (the source as it is).

    mutants_launcher.py --compiler PATH\\x86_64-w64-mingw32-clang.exe

Exits 1 if the control fails, a mutant survives, does not compile or no longer applies. The source is not touched.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE / 'builder_launcher.c'
TESTS = HERE / 'test_launcher.py'
MUTANTS = [
    ('backslashes before a quote are not doubled', 'for (size_t k = 0; k < slashes * 2 + 1; k++) PUT(L\'\\\\\');', 'for (size_t k = 0; k < slashes + 1; k++) PUT(L\'\\\\\');'),
    ('a quote inside an argument is not escaped', 'for (size_t k = 0; k < slashes * 2 + 1; k++) PUT(L\'\\\\\');', 'for (size_t k = 0; k < slashes * 2; k++) PUT(L\'\\\\\');'),
    ('backslashes at the end of an argument are not doubled', '            for (size_t k = 0; k < slashes * 2; k++) PUT(L\'\\\\\');\n            break;', '            for (size_t k = 0; k < slashes; k++) PUT(L\'\\\\\');\n            break;'),
    ('backslashes inside an argument are doubled', '            for (size_t k = 0; k < slashes; k++) PUT(L\'\\\\\');\n        }\n        PUT(*p);', '            for (size_t k = 0; k < slashes * 2; k++) PUT(L\'\\\\\');\n        }\n        PUT(*p);'),
    ('arguments are not quoted', '    if (i) PUT(L\' \');\n    PUT(L\'"\');', '    if (i) PUT(L\' \');'),
    ('the first argument given is dropped', '    for (int i = 1; i < argc; i++)\n        if (!append_argument(command, MAX_COMMAND, &at, argv[i])) goto too_long;', '    for (int i = 2; i < argc; i++)\n        if (!append_argument(command, MAX_COMMAND, &at, argv[i])) goto too_long;'),
    ('the front end may write bytecode', 'const wchar_t *fixed[] = {python, L"-B", script};\n    for (int i = 0; i < 3; i++)', 'const wchar_t *fixed[] = {python, script};\n    for (int i = 0; i < 2; i++)'),
    ('the package root is not named', '!SetEnvironmentVariableW(L"BUILDER_PACKAGE_ROOT", folder) || ', ''),
    ('the package\'s Python is not named', ' || !SetEnvironmentVariableW(L"BUILDER_PYTHON", python) ||', ' ||'),
    ('the host tools are named in another folder', 'L"%ls\\\\host-tools", folder', 'L"%ls\\\\tools", folder'),
    ('UTF-8 is not asked of the tools the front end starts', '!SetEnvironmentVariableW(L"PYTHONUTF8", L"1") ||', ''),
    ('another Python\'s home stays in the environment', '!SetEnvironmentVariableW(L"PYTHONHOME", NULL) || ', ''),
    ('another Python\'s path stays in the environment', ' || !SetEnvironmentVariableW(L"PYTHONPATH", NULL) ||', ' ||'),
    ('the player\'s own package folder may be put on the path', ' ||\n        !SetEnvironmentVariableW(L"PYTHONNOUSERSITE", L"1")) {', ') {'),
    ('the switch against the player\'s package folder is set to off', 'L"PYTHONNOUSERSITE", L"1")', 'L"PYTHONNOUSERSITE", L"")'),
    ('the caller\'s PATH is changed', '    size_t at = 0;\n', '    SetEnvironmentVariableW(L"PATH", tools);\n    size_t at = 0;\n'),
    ('the exit code is not handed through', '    return (int)code;\ntoo_long:', '    return 0;\ntoo_long:'),
    ('a package that is not whole is started anyway', 'if (GetFileAttributesW(python) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {', 'if (0) {'),
    ('a missing front end is not noticed', ' || GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES) {', ') {'),
    ('a missing Python is not noticed', 'if (GetFileAttributesW(python) == INVALID_FILE_ATTRIBUTES || ', 'if ('),
    ('a double click does not wait', '    if (argc <= 1) wait_for_a_key();     /* a double click: keep the usage on the screen */', ''),
    ('every start waits for a key', '    if (argc <= 1) wait_for_a_key();     /* a double click: keep the usage on the screen */', '    wait_for_a_key();'),
]


def compile_to(compiler: str, source: Path, exe: Path) -> bool:
    return subprocess.run([compiler, '-O2', '-municode', '-o', str(exe), str(source)], capture_output=True).returncode == 0


def tests_pass(exe: Path) -> bool:
    return subprocess.run([sys.executable, '-X', 'utf8', '-B', str(TESTS)], capture_output=True, input=b'',
                          env=dict(os.environ, BUILDER_LAUNCHER=str(exe))).returncode == 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--compiler', required=True)
    a = ap.parse_args()
    text = SOURCE.read_text(encoding='utf-8')
    killed = 0
    with tempfile.TemporaryDirectory(prefix='launcher-mutants-') as folder:
        folder = Path(folder)
        control = folder / 'control.exe'
        if not compile_to(a.compiler, SOURCE, control) or not tests_pass(control):
            print('CONTROL FAILED: the source as it is does not compile or does not pass its tests')
            return 1
        print('control: the source as it is compiles and passes', flush=True)
        for index, (what, old, new) in enumerate(MUTANTS, 1):
            if text.count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({text.count(old)} matches): {what}')
                continue
            source, exe = folder / f'mutant-{index}.c', folder / f'mutant-{index}.exe'
            source.write_text(text.replace(old, new), encoding='utf-8', newline='\n')
            if not compile_to(a.compiler, source, exe):
                print(f'mutant {index:3d} DOES NOT COMPILE: {what}')
                continue
            if tests_pass(exe):
                print(f'mutant {index:3d} SURVIVED: {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed: {what}')
    print(f'{killed} of {len(MUTANTS)} mutants killed')
    return 0 if killed == len(MUTANTS) else 1


if __name__ == '__main__':
    sys.exit(main())
