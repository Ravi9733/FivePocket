#!/usr/bin/env python3
"""Mutant check for how build_shader_extras.py takes its toolchain and starts the compiler: copies of the tool with one
rule changed; the Toolchain cases of test_shader_extras.py (beside this file) must fail on each. First a control.
Exits 1 if the control fails, a mutant survives or no longer applies. The tool itself is not touched.

Not reached by these cases: the second place a program is started (the gradient and level-register translators,
inside a whole override run). It uses the same call; the stage run on real data is its check on each host."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'build_shader_extras.py'
TESTS = HERE / 'test_shader_extras.py'
LOAD = "    config=shader_toolchain.load(args.toolchain,args.host_tools,args.ndk,extras=True)\n"
FIELDS = "    if set(config)!=set(shader_toolchain.BASE+shader_toolchain.EXTRAS):raise ValueError('toolchain fields refused')\n"
MUTANTS = [
    ('the override step asks for the pack\'s six fields only', LOAD, LOAD.replace('extras=True', 'extras=False')),
    ('the folders are not handed on', LOAD, LOAD.replace('args.host_tools,args.ndk', 'None,None')),
    ('the file is not handed on', LOAD, LOAD.replace('load(args.toolchain,', 'load(None,')),
    ('the NDK is taken for the host tools', LOAD, LOAD.replace('args.host_tools,args.ndk', 'args.ndk,args.host_tools')),
    ('any set of fields is taken', FIELDS, ''),
    ('the pack\'s six fields are enough', FIELDS, FIELDS.replace('shader_toolchain.BASE+shader_toolchain.EXTRAS', 'shader_toolchain.BASE')),
    ('the compiler is started without its library named', "    env=shader_toolchain.library_environment(config['compiler_library_directory'])\n    run_tool([config['dxc']", "    env=os.environ.copy()\n    run_tool([config['dxc']"),
    ('the compiler is told another folder', "    env=shader_toolchain.library_environment(config['compiler_library_directory'])\n    run_tool([config['dxc']", "    env=shader_toolchain.library_environment(folder)\n    run_tool([config['dxc']"),
    ('the command line knows the file only', "    for n in ('toolchain','host-tools','ndk'):p.add_argument('--'+n,type=Path)", "    for n in ('toolchain',):p.add_argument('--'+n,type=Path,default=None);p.set_defaults(host_tools=None,ndk=None)"),
    ('the command line requires the file', "    for n in ('toolchain','host-tools','ndk'):p.add_argument('--'+n,type=Path)", "    for n in ('toolchain','host-tools','ndk'):p.add_argument('--'+n,type=Path,required=n=='toolchain')"),
]


def run(tool):
    return subprocess.run([sys.executable, '-B', str(TESTS), 'Toolchain'], capture_output=True, text=True, env=dict(os.environ, EXTRAS_SOURCE=str(tool)))


def main() -> int:
    source = TOOL.read_text(encoding='utf-8')
    if run(TOOL).returncode != 0:
        print('CONTROL FAILED: the tests do not pass on the tool as it is')
        return 1
    print('control: the tests pass on the tool as it is', flush=True)
    killed = 0
    with tempfile.TemporaryDirectory() as folder:
        # (the tool hashes the pack tool beside itself: a copy of that lies beside every copy of the tool)
        (Path(folder) / 'build_shader_pack.py').write_bytes(TOOL.with_name('build_shader_pack.py').read_bytes())
        same = Path(folder) / 'unchanged.py'
        same.write_text(source, encoding='utf-8', newline='\n')
        if run(same).returncode != 0:
            print('CONTROL FAILED: an unchanged copy of the tool does not pass (a mutant would be killed by being a copy)')
            return 1
        print('control: an unchanged copy passes too', flush=True)
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
