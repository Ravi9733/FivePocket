#!/usr/bin/env python3
"""Mutant check for portable_compile.py: copies of the tools folder with one rule of the tool changed;
test_portable_compile.py (beside this file) must fail on each. First a control. Exits 1 if the control fails, a
mutant survives or no longer applies. The tools are not touched."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
FILES = ('portable_compile.py', 'replay_build.py', 'compile_rehearsal.py', 'elf_sections.py', 'host_support.py')
TESTS = HERE / 'test_portable_compile.py'
MUTANTS = [
    ('an input found nowhere is passed over', "            if label_path is None:\n                missing.append(rel)\n                continue\n", "            if label_path is None:\n                continue\n"),
    ('missing inputs are found out by the compiler', "    if missing:\n        raise ValueError(", "    if False:\n        raise ValueError("),
    ('a supplied file is named before a stage output', "from_stage.get(want['sha256']) or from_supplied.get(want['sha256'])", "from_supplied.get(want['sha256']) or from_stage.get(want['sha256'])"),
    ('supplied folders are not looked into', "from_stage.get(want['sha256']) or from_supplied.get(want['sha256'])", "from_stage.get(want['sha256'])"),
    ('stage outputs are not looked into', "from_stage.get(want['sha256']) or from_supplied.get(want['sha256'])", "from_supplied.get(want['sha256'])"),
    ('this run\'s own file is not compared', "            if here.stat().st_size != want['size'] or sha_file(here) != want['sha256']:\n                found =", "            if False:\n                found ="),
    ('a host-built library is taken whatever its objects', "                if not found or found != host_built[rel]:", "                if rel not in host_built:"),
    ('a named library needs no objects at all', "                if not found or found != host_built[rel]:", "                if found != host_built[rel]:"),
    ('any file is looked at as a library', "                found = archive_identity(here) if rel in host_built else ''", "                found = archive_identity(here)\n                host_built = dict(host_built, **{rel: host_built.get(rel, found)})"),
    ('a host-built library is recorded as the first machine\'s bytes', "                origins[rel] = 'this run, in place (the first machine\\'s objects with this host\\'s compiler name in them)'\n", "                origins[rel] = 'this run, in place'\n"),
    ('an unknown host-built name is accepted', "    unknown = set(host_built) - set(wanted)\n    if unknown:\n        raise ValueError(", "    unknown = set(host_built) - set(wanted)\n    if False:\n        raise ValueError("),
    ('the compile is not told the identity', "['--no-link', *pending, *recognised])", "['--no-link', *pending])"),
    ('the link is not told the identity', "    link = replay(manifest_path, repo, host, frozen, work, jobs, recognised)['link']", "    link = replay(manifest_path, repo, host, frozen, work, jobs, [])['link']"),
    ('the identity is told for a name of this run', "('--input-identity', f'{frozen}/{rel}={identity}')", "('--input-identity', f'{rel}={identity}')"),
    ('any text is taken as an identity on the command line', "        if len(host_built) != len(a.host_built) or any(len(v) != 64 or set(v) - set('0123456789abcdef') for v in host_built.values()):\n            raise ValueError(", "        if False:\n            raise ValueError("),
    ('an identity of another length is taken', "any(len(v) != 64 or set(v) - set('0123456789abcdef') for v in host_built.values())", "any(set(v) - set('0123456789abcdef') for v in host_built.values())"),
    ('the command line does not hand the libraries on', "named(a.supplied, '--supplied'), set(a.made_later), a.jobs, host_built=host_built)", "named(a.supplied, '--supplied'), set(a.made_later), a.jobs)"),
    ('a file of the run is replaced by a supplied one of the same name', "        if here.is_file() and not rel.startswith('compile/'):", "        if False:"),
    ('a different file already there is used', "            if here.exists() and (here.stat().st_size != want['size'] or sha_file(here) != want['sha256']):\n                raise ValueError(f'{rel}: a different file lies there already')\n", ""),
    ('nothing is laid out', "            shutil.copyfile(source, here)\n", "            pass\n"),
    ('the origin recorded is always the stage', "            origins[rel] = label\n", "            origins[rel] = 'stage:recipe'\n"),
    ('inputs in place are recorded as laid out', "            origins[rel] = 'this run, in place'\n", "            origins[rel] = 'stage:recipe'\n"),
    ('an unknown made-later name is accepted', "    if unknown:\n        raise ValueError('--made-later names no input of the manifest: ' + ', '.join(sorted(unknown)))\n", ""),
    ('an input outside the work folder is accepted', "        if not below(rel):\n            raise ValueError('the manifest names an input outside the work folder: ' + rel)\n", ""),
    ('a manifest without inputs in the work folder is accepted', "    if not wanted:\n        raise ValueError('the manifest names no input in ' + frozen)\n", ""),
    ('a missing stage folder is an empty one', "        if files is None:\n            raise ValueError(f'{label}: {base} is not there')\n", "        if files is None:\n            continue\n"),
    ('the script need not be named as made later', "        raise ValueError(f'{REDIRECT} is made from this run\\'s objects: name it with --made-later')\n", "        pass\n"),
    ('a climbing frozen folder is accepted', "    if not below(frozen):\n        raise ValueError('--frozen-work wants a path below the first run\\'s root')\n", ""),
    ('a missing NDK tool is found out later', "        if not tool.is_file():\n            raise ValueError(f'the NDK tool is missing: {tool}')\n", ""),
    ('a script that differs is linked', "        if report['redirect']['sha256'] != want['sha256']:\n            raise RuntimeError(", "        if False:\n            raise RuntimeError("),
    ('the script is written with the host\'s line ends', "        script.write_text(text, encoding='utf-8', newline='\\n')\n", "        script.write_text(text.replace('\\n', '\\r\\n'), encoding='utf-8', newline='')\n"),
    ('no script is made', "        script.write_text(text, encoding='utf-8', newline='\\n')\n", "        pass\n"),
    ('the script is made from no objects', "        objects = sorted((linked.parent / 'obj').glob('*.o'))\n", "        objects = []\n"),
    ('a failed replay is not noticed', "    if done.returncode != 0 or not results.is_file():\n        raise RuntimeError(", "    if not results.is_file():\n        raise RuntimeError("),
    ('the log of a failed step is not named', "        raise RuntimeError(f'the replay ended with {done.returncode}; log: {log}')", "        raise RuntimeError(f'the replay ended with {done.returncode}')"),
    ('the compile and the link write one log', "('replay-compile.log' if '--no-link' in extra else 'replay-link.log')", "'replay-compile.log'"),
    ('the link may lie outside the work folder', "    if not link_rel.startswith(frozen + '/'):\n        raise ValueError('the manifest links outside the first run\\'s work folder')\n", ""),
    ('the portable library is the linked one', "    rehearsal.portable_library(objcopy, linked, out)\n", "    shutil.copyfile(linked, out)\n"),
    ('no report is written', "    (work / 'compile' / 'portable-compile.json').write_text(", "    (work / 'compile' / 'unwritten.json').write_text("),
    ('the supplied inputs are not listed', "'supplied_inputs': sorted(r for r, o in origins.items() if o.startswith('supplied:'))}", "'supplied_inputs': []}"),
    ('the replay writes its state into the tree', "'--map', f'{frozen}={work}', '--state-dir', str(state), *extra]", "'--map', f'{frozen}={work}', *extra]"),
    ('the work folder is not mapped', "'--jobs', str(jobs), '--map', f'{frozen}={work}', '--state-dir', str(state), *extra]", "'--jobs', str(jobs), '--state-dir', str(state), *extra]"),
    ('the script is not left pending for the compile', "    pending = [arg for rel in sorted(later) for arg in ('--pending-input', f'{frozen}/{rel}')]\n", "    pending = []\n"),
    ('a refusal exits like a failure', "        print('PORTABLE_COMPILE_REFUSED:', error, file=sys.stderr)\n        return 2\n", "        print('PORTABLE_COMPILE_REFUSED:', error, file=sys.stderr)\n        return 1\n"),
    ('a failure exits 0', "        print('PORTABLE_COMPILE_FAILED:', error, file=sys.stderr)\n        return 1\n", "        print('PORTABLE_COMPILE_FAILED:', error, file=sys.stderr)\n        return 0\n"),
    ('a stage without a name is accepted', "        if '=' not in item:\n            raise ValueError(f'{what} wants NAME=PATH, got {item!r}')\n", "        if '=' not in item:\n            continue\n"),
]


def run(folder):
    return subprocess.run([sys.executable, '-B', str(TESTS)], capture_output=True, text=True, env=dict(os.environ, PORTABLE_COMPILE_DIR=str(folder)))


def main() -> int:
    source = (TOOLS / FILES[0]).read_text(encoding='utf-8')
    killed = 0
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp).resolve() / 'own_disc'
        folder.mkdir()
        for name in FILES:
            shutil.copy(TOOLS / name, folder / name)
        if run(folder).returncode != 0:
            print('CONTROL FAILED: the tests do not pass on unchanged copies'); return 1
        print('control: the tests pass on unchanged copies of the tools', flush=True)
        for index, (what, old, new) in enumerate(MUTANTS, 1):
            if source.count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({source.count(old)} matches): {what}'); continue
            (folder / FILES[0]).write_text(source.replace(old, new), encoding='utf-8')
            if run(folder).returncode == 0: print(f'mutant {index:3d} SURVIVED: {what}')
            else: killed += 1; print(f'mutant {index:3d} killed: {what}')
    print(f'{killed} of {len(MUTANTS)} mutants killed')
    return 0 if killed == len(MUTANTS) else 1


if __name__ == '__main__':
    sys.exit(main())
