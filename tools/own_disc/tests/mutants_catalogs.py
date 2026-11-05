#!/usr/bin/env python3
"""Mutant check for build_catalogs.py: each mutant is a copy with one rule removed or one value changed;
test_build_catalogs.py (beside this file) must fail on it. First a control on an unchanged copy.

  mutants_catalogs.py [--only TEXT]"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'build_catalogs.py'
TEST = HERE / 'test_build_catalogs.py'

MUTANTS = [
    ('files are given no allocation', "        allocation = 0 if folder else (size + 2047) // 2048 * 2048", "        allocation = 0"),
    ('allocation is the bare size', "(size + 2047) // 2048 * 2048", "size"),
    ('allocation rounds to 4096', "(size + 2047) // 2048 * 2048", "(size + 4095) // 4096 * 4096"),
    ('folders are marked as files', "DIRECTORY if folder else FILE))", "FILE))"),
    ('records keep the order they were found in', "    for (guest, (size, folder)), stamp in zip(sorted(rows.items()), times):", "    for (guest, (size, folder)), stamp in zip(rows.items(), times):"),
    ('a table of another length is accepted', "    if len(times) != len(rows):", "    if False:"),
    ('a shorter table is accepted', "    if len(times) != len(rows):", "    if len(times) > len(rows):"),
    ('timestamp rows are not checked', "        if not isinstance(stamp, list) or len(stamp) != 4 or not all(type(t) is int and 0 <= t < 1 << 63 for t in stamp):", "        if False:"),
    ('negative timestamps are accepted', "type(t) is int and 0 <= t < 1 << 63", "type(t) is int and t < 1 << 63"),
    ('fractional timestamps are accepted', "type(t) is int and 0 <= t < 1 << 63", "0 <= t < 1 << 63"),
    ('two entries with one path are accepted', "        if guest in rows:", "        if False:"),
    ("a folder's size is written", "        rows[guest] = (0 if folder else size, folder)", "        rows[guest] = (size, folder)"),
    ('the extra files of the table are left out', "list(play_entries) + extras, table['play']['times'])", "list(play_entries), table['play']['times'])"),
    ('package names keep their case', "((name.upper(), display, entries) for name, display, entries in packages)", "((name, display, entries) for name, display, entries in packages)"),
    ('packages keep the order they were found in', "for name, display, entries in packages), key=lambda p: p[0])", "for name, display, entries in packages), key=lambda p: 0)"),
    ('packages are numbered from zero', "in enumerate(zip(ordered, table['packages']), 1):", "in enumerate(zip(ordered, table['packages']), 0):"),
    ('another number of packages is accepted', "    if len(ordered) != len(table['packages']):", "    if False:"),
    ('content records in any order are accepted', "    if names != sorted(names):", "    if False:"),
    ("the content catalog is not checked by its reader", "    if content_catalog.parse_catalog(data) != (title_id, len(packages)):", "    if False:"),
    ('the content catalog names another device', "struct.pack('>II', 1, 2) + display", "struct.pack('>II', 2, 2) + display"),
    ('the disc listing leaves folders out', "                result.append((prefix + name, length, folder))\n", "                if not folder:\n                    result.append((prefix + name, length, folder))\n"),
    ('the disc listing does not enter folders', "                if folder:\n                    pending.append(", "                if False:\n                    pending.append("),
    ('any table schema is accepted', "table.get('schema') != TIMES_SCHEMA or ", ""),
    ('an extra file may name a path', " \\\n                or '/' in extra['name'] or '\\\\' in extra['name']:", ":"),
    ('an extra file needs no size', "or type(extra.get('bytes')) is not int ", ""),
    ('an unexpected hash is not reported', "        if name in expected and expected[name] != sha(data):", "        if False:"),
    ('a missing expected catalog is not reported', "    if set(expected) - set(outputs):", "    if False:"),
    ('the output folder may exist', "    output.mkdir(parents=True)\n    report = {", "    output.mkdir(parents=True, exist_ok=True)\n    report = {"),
    ('a differing result still exits 0', "    return 0 if report['status'] == 'complete' else 1", "    return 0"),
    ('the package name is taken from the copy, not the disc', "item['origin'].rsplit('/', 1)[-1]", "item['file'].rsplit('/', 1)[-1]"),
]


def run(tool: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, '-B', str(TEST), '-v'], env=dict(os.environ, BUILD_CATALOGS=str(tool)),
                          capture_output=True, text=True, timeout=300)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--only')
    a = ap.parse_args()
    source = TOOL.read_text()
    chosen = [(i, m) for i, m in enumerate(MUTANTS, 1) if not a.only or a.only in m[0]]
    killed = 0
    # A copy has to lie in the tool's own folder: the tool finds the repository from where it is.
    copy = TOOL.with_name(f'_mutant_catalogs_{os.getpid()}.py')
    try:
        copy.write_text(source)
        r = run(copy)
        if r.returncode != 0:
            print('CONTROL FAILED: the tests do not pass on an unchanged copy\n' + r.stderr[-2000:])
            return 1
        print('control: the tests pass on an unchanged copy of the tool', flush=True)
        for index, (what, old, new) in chosen:
            if source.count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({source.count(old)} matches): {what}')
                continue
            copy.write_text(source.replace(old, new))
            r = run(copy)
            failed = [line.split(' ... ')[0].strip() for line in r.stderr.splitlines() if line.rstrip().endswith(('... FAIL', '... ERROR'))]
            if r.returncode == 0:
                print(f'mutant {index:3d} SURVIVED: {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed by {failed[0].split(" (")[0] if failed else "the test run (it could not start)"}: {what}')
    finally:
        copy.unlink(missing_ok=True)
    print(f'{killed} of {len(chosen)} mutants killed')
    return 0 if killed == len(chosen) else 1


if __name__ == '__main__':
    sys.exit(main())
