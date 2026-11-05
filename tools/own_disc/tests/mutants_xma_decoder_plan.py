#!/usr/bin/env python3
"""Mutant check for build_xma_decoder.py: copies of the tool with one rule changed; test_xma_decoder_plan.py (beside
this file) must fail on each. First a control. Exits 1 if the control fails, a mutant survives or no longer applies.
The tool itself is not touched."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'build_xma_decoder.py'
TESTS = HERE / 'test_xma_decoder_plan.py'
MUTANTS = [
    ("a host's own compiler name makes a library another one", "        if plan.get('identity'):         # the same objects, whatever the compiler calls itself on this host\n", '        if False:\n'),
    ("a library of other objects is the plan's", "                                                     as_the_plan_expects=found == plan['identity'][entry['path']])", '                                                     as_the_plan_expects=True)'),
    ("the bytes must be the first machine's as well", "                                                     as_the_plan_expects=found == plan['identity'][entry['path']])", "                                                     as_the_plan_expects=same and found == plan['identity'][entry['path']])"),
    ("every library is said to have the first machine's bytes", 'same_bytes_as_the_first_machine=same,', 'same_bytes_as_the_first_machine=True,'),
    ('the member count is not reported', "            report['archives'][entry['path']].update(identity=found, members=members,", "            report['archives'][entry['path']].update(identity=found, members=0,"),
    ('the identities found are not written down', "    if plan.get('identity'):\n        found = {path: entry['identity'] for path, entry in sorted(report['archives'].items())}\n", '    if False:\n        found = {}\n'),
    ('the record holds the identities hoped for', "        found = {path: entry['identity'] for path, entry in sorted(report['archives'].items())}\n", "        found = dict(plan['identity'])\n"),
    ("the record holds this host's file hashes too", "json.dumps({'schema': 'xma-decoder-identity/1', 'archives': found}, indent=1, sort_keys=True)", "json.dumps({'schema': 'xma-decoder-identity/1', 'archives': found, 'files': {p: e['sha256'] for p, e in report['archives'].items()}}, indent=1, sort_keys=True)"),
    ("the record is written with the host's line ends", "indent=1, sort_keys=True) + '\\n',\n                                              encoding='utf-8', newline='\\n')", "indent=1, sort_keys=True) + '\\n',\n                                              encoding='utf-8', newline='\\r\\n')"),
    ('identities for some libraries are enough', 'set(identity) != set(expect) or not all(is_hash(v) for v in identity.values())', 'not all(is_hash(v) for v in identity.values())'),
    ('any text is an identity', 'set(identity) != set(expect) or not all(is_hash(v) for v in identity.values())', 'set(identity) != set(expect)'),
    ('identities of any shape are taken', '    if identity is not None and (not isinstance(identity, dict) or ', '    if identity is not None and isinstance(identity, dict) and ('),
    ('the command line calls a recognised library a different one', "        verdict = 'DIFFERS' if not entry['as_the_plan_expects'] else 'as expected' if entry.get('same_bytes_as_the_first_machine', True) \\", "        verdict = 'DIFFERS' if not entry.get('same_bytes_as_the_first_machine', entry['as_the_plan_expects']) else 'as expected' if entry.get('same_bytes_as_the_first_machine', True) \\"),
    ("the command line does not say when the bytes are another host's", "else 'as expected' if entry.get('same_bytes_as_the_first_machine', True) \\", "else 'as expected' if True \\"),
    ('any source archive is built', "    if sha_file(archive) != plan['source_archive_sha256']:\n        raise ValueError('the source archive is not the one the plan names')\n", ""),
    ('a changed configured file is used', "        if not path.is_file() or sha_file(path) != expected:", "        if not path.is_file():"),
    ('a missing configured file is passed over', "        if not path.is_file() or sha_file(path) != expected:", "        if path.is_file() and sha_file(path) != expected:"),
    ('an existing output folder is written into', "    if output.exists():\n        raise ValueError('the output folder exists already')\n", ""),
    ('the archive is unpacked before the checks', "    tool = tools(ndk, platform)\n    version = subprocess.run([str(tool['clang']), '--version'], capture_output=True, text=True)\n    if version.returncode != 0:\n        raise ValueError('the NDK compiler does not start')\n    files = unpack(archive, output)\n",
     "    files = unpack(archive, output)\n    tool = tools(ndk, platform)\n    version = subprocess.run([str(tool['clang']), '--version'], capture_output=True, text=True)\n"),
    ('links in the archive are accepted', "            if not (member.isfile() or member.isdir()) or not inside(member.name.rstrip('/')):", "            if not inside(member.name.rstrip('/')):"),
    ('member names may leave the folder', "            if not (member.isfile() or member.isdir()) or not inside(member.name.rstrip('/')):", "            if not (member.isfile() or member.isdir()):"),
    ('a name may climb above the folder', "        depth += -1 if part == '..' else 1\n        if depth < 0:\n            return False\n", "        depth += 1\n"),
    ('a name may start at the root', " or name.startswith('/'):\n        return False\n    depth = 0", ":\n        return False\n    depth = 0"),
    ('a name may hold a backslash or a drive', "not name or '\\\\' in name or ':' in name or name.startswith('/')", "not name or name.startswith('/')"),
    ('the revision\'s own files are compiled, not the configured ones', "        target.write_bytes(configured.joinpath(*PurePosixPath(name).parts).read_bytes())\n", "        pass\n"),
    ('the target is left to the compiler', "    return [str(clang), '--target=' + plan['target'], *plan['flag_sets'][unit['flags']],", "    return [str(clang), *plan['flag_sets'][unit['flags']],"),
    ('every unit gets the first flag set', "*plan['flag_sets'][unit['flags']],", "*plan['flag_sets'][0],"),
    ('no dependency file is asked for', "            '-MMD', '-MF', obj[:-2] + '.d', '-MT', obj, '-c', '-o', obj, unit['source']]", "            '-c', '-o', obj, unit['source']]"),
    ('units are compiled in another folder', "        r = subprocess.run(unit_command(plan, unit, tool['clang']), cwd=output, capture_output=True, text=True)\n        if r.returncode != 0:\n            raise ValueError(f'{unit[\"source\"]} did not compile",
     "        r = subprocess.run(unit_command(plan, unit, tool['clang']), cwd=output.parent, capture_output=True, text=True)\n        if r.returncode != 0:\n            raise ValueError(f'{unit[\"source\"]} did not compile"),
    ('a unit that does not compile is passed over', "        if r.returncode != 0:\n            raise ValueError(f'{unit[\"source\"]} did not compile: ' + (r.stderr or r.stdout).strip()[-600:])\n", ""),
    ('a missing source is left to the compiler', "            raise ValueError(f'the source of a unit is not in the archive: {unit[\"source\"]}')\n", "            pass\n"),
    ('members go in in name order', "[str(tool['llvm-ar']), 'rcD', entry['path'], *entry['members']]", "[str(tool['llvm-ar']), 'rcD', entry['path'], *sorted(entry['members'])]"),
    ('the archive is not indexed', "        for command in ([str(tool['llvm-ar']), 'rcD', entry['path'], *entry['members']], [str(tool['llvm-ranlib']), entry['path']]):", "        for command in ([str(tool['llvm-ar']), 'rcD', entry['path'], *entry['members']],):"),
    ('a library that differs passes', "        if not report['archives'][entry['path']]['as_the_plan_expects']:\n            report['status'] = 'differs'\n", ""),
    ('every library is reported as expected', "        same = got == plan['expect'][entry['path']]\n", "        same = True\n"),
    ('a differing library exits 0', "    return 0 if report['status'] == 'complete' else 1\n", "    return 0\n"),
    ('no report is written', "    (output / 'report.json').write_text(json.dumps(report, indent=1, sort_keys=True) + '\\n', encoding='utf-8', newline='\\n')\n", ""),
    ('Windows tools are looked for without .exe', "    suffix = '.exe' if platform == 'win32' else ''\n", "    suffix = ''\n"),
    ('every host uses the Mac folder of the NDK', "    tag = HOST_TAGS.get('linux' if platform.startswith('linux') else platform)\n", "    tag = HOST_TAGS['darwin']\n"),
    ('a missing NDK tool is found out later', "        if not path.is_file():\n            raise ValueError(f'the NDK tool is missing: {path}')\n", ""),
    ('the plan schema is not checked', "    if not isinstance(plan, dict) or plan.get('schema') != SCHEMA:", "    if not isinstance(plan, dict):"),
    ('any target is accepted', " or not plan['target'].startswith('aarch64-linux-android'):", ":"),
    ('configured names are not checked', "not all(inside(n) and is_hash(h) for n, h in configured.items())", "not all(is_hash(h) for n, h in configured.items())"),
    ('a flag may name an output or a file', "        if any(f in ('-o', '-c', '-MF', '-MT') or f.startswith(('/', '@')) or '..' in f for f in flags):\n            raise ValueError('plan refused: a flag set names a file or an output')\n", ""),
    ('a flag may climb', "f.startswith(('/', '@')) or '..' in f for f in flags", "f.startswith(('/', '@')) for f in flags"),
    ('a flag may read a response file', "f.startswith(('/', '@'))", "f.startswith('/')"),
    ('unit sources are not checked', "not inside(unit.get('source')) or not inside(unit.get('object'))", "not inside(unit.get('object'))"),
    ('unit objects are not checked', "not inside(unit.get('source')) or not inside(unit.get('object'))", "not inside(unit.get('source'))"),
    ('any kind of source is compiled', "                or not unit['source'].endswith(('.c', '.S')) or not unit['object'].endswith('.o') \\\n", "                or not unit['object'].endswith('.o') \\\n"),
    ('two units may write one object', " or unit['object'] in objects:\n", ":\n"),
    ('a flag set index out of range is accepted', "or type(unit.get('flags')) is not int or not 0 <= unit['flags'] < len(sets)", "or type(unit.get('flags')) is not int"),
    ('True counts as a flag set index', "or type(unit.get('flags')) is not int or", "or not isinstance(unit.get('flags'), int) or"),
    ('an object may be in two archives', "                or listed & set(archive['members']) or len(set(archive['members'])) != len(archive['members']):", "                or len(set(archive['members'])) != len(archive['members']):"),
    ('an archive may hold an unknown object', "    if listed != objects or set(expect) != {a['path'] for a in archives}:", "    if not objects <= listed or set(expect) != {a['path'] for a in archives}:"),
    ('an object may be in an archive twice', " or len(set(archive['members'])) != len(archive['members']):", ":"),
    ('an object may be left out of every archive', "    if listed != objects or set(expect) != {a['path'] for a in archives}:", "    if set(expect) != {a['path'] for a in archives}:"),
    ('an expected library that is never made is accepted', "    if listed != objects or set(expect) != {a['path'] for a in archives}:", "    if listed != objects:"),
    ('an archive without an expected hash is accepted', "        if not is_hash(expect.get(archive['path'])):\n            raise ValueError('plan refused: an archive without an expected hash')\n", ""),
    ('archive paths are not checked', "not inside(archive.get('path')) or not archive['path'].endswith('.a')", "not archive.get('path', '').endswith('.a')"),
]


def run(tool):
    return subprocess.run([sys.executable, '-B', str(TESTS)], capture_output=True, text=True, env=dict(os.environ, XMA_DECODER_TOOL=str(tool)))


def main() -> int:
    source = TOOL.read_text(encoding='utf-8')
    killed = 0
    with tempfile.TemporaryDirectory() as tmp:
        copy = Path(tmp) / 'build_xma_decoder.py'
        copy.write_text(source, encoding='utf-8')
        (Path(tmp) / 'elf_sections.py').write_bytes(TOOL.with_name('elf_sections.py').read_bytes())     # the helper the tool imports from beside itself
        if run(copy).returncode != 0:
            print('CONTROL FAILED: the tests do not pass on an unchanged copy'); return 1
        print('control: the tests pass on an unchanged copy of the tool', flush=True)
        for index, (what, old, new) in enumerate(MUTANTS, 1):
            if source.count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({source.count(old)} matches): {what}'); continue
            copy.write_text(source.replace(old, new), encoding='utf-8')
            if run(copy).returncode == 0: print(f'mutant {index:3d} SURVIVED: {what}')
            else: killed += 1; print(f'mutant {index:3d} killed: {what}')
    print(f'{killed} of {len(MUTANTS)} mutants killed')
    return 0 if killed == len(MUTANTS) else 1


if __name__ == '__main__':
    sys.exit(main())
