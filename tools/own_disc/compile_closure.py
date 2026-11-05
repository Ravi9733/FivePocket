#!/usr/bin/env python3
"""What did the compile stage really read, and is all of it declared?

A stage of the builder declares its inputs in the profile. The compiler reads more: every include folder on its
command lines. On 2026-10-08 that difference hid 57 earlier shader pack headers with game shader bytes from the list
of supplied inputs. This tool compares the two. It takes the manifest freeze_build.py writes for a finished build
(its "inputs" are the files named by the compiler's own dependency files, as paths relative to the repository) and
puts every file into exactly one group:

  work        under the run's work folder: made in this run
  tracked     a file the repository tracks
  declared    named by the declaration file, as a file or as a folder (an entry ending in "/")
  undeclared  none of the above

    compile_closure.py --manifest MANIFEST --work FOLDER --declared FILE [--tracked FILE] [--report OUT.json]

--work is the work folder relative to the repository. --tracked is a file with one tracked path per line; without
it `git ls-files` of the manifest's repository is asked. Exit 0 when nothing is undeclared, 1 when something is,
2 when an argument or a file is refused. Declared entries that the compiler did not read are reported as unused; they
do not change the exit code.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

DECLARED_SCHEMA = 'declared-inputs/1'
GROUPS = ('work', 'tracked', 'declared', 'undeclared')


def under(path: str, folder: str) -> bool:
    """True when path lies inside folder (a path of whole components; 'a/b' is not under 'a/bc')."""
    folder = folder.rstrip('/')
    return path == folder or path.startswith(folder + '/')


def load_declared(path: Path) -> list[dict]:
    table = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(table, dict) or table.get('schema') != DECLARED_SCHEMA or not isinstance(table.get('entries'), list):
        raise ValueError('declaration file refused: it needs the schema ' + DECLARED_SCHEMA + ' and a list of entries')
    seen = set()
    for entry in table['entries']:
        if not isinstance(entry, dict) or not isinstance(entry.get('path'), str) or not entry['path'].strip('/') \
                or entry['path'].startswith('/') or '..' in entry['path'].split('/') \
                or type(entry.get('game_derived')) is not bool or not isinstance(entry.get('what'), str) or not entry['what']:
            raise ValueError('declaration entry refused: it needs a relative path, what it is, and game_derived true or false')
        if entry['path'] in seen:
            raise ValueError('declared twice: ' + entry['path'])
        seen.add(entry['path'])
    return table['entries']


def classify(inputs: dict, work: str, tracked: set[str], declared: list[dict]) -> dict:
    """inputs: path -> {'size': n, ...}. Returns the groups, the bytes per group, and per declared entry what it covered."""
    if not work.strip('/') or work.startswith('/'):
        raise ValueError('the work folder must be a path relative to the repository')
    groups = {name: [] for name in GROUPS}
    covered = {entry['path']: [] for entry in declared}
    for path in sorted(inputs):
        if under(path, work):
            groups['work'].append(path)
            continue
        if path in tracked:
            groups['tracked'].append(path)
            continue
        # the most specific declaration wins, so a file named by itself is not swallowed by its folder's entry
        owners = [e['path'] for e in declared if (under(path, e['path']) if e['path'].endswith('/') else path == e['path'])]
        if owners:
            covered[max(owners, key=len)].append(path)
            groups['declared'].append(path)
        else:
            groups['undeclared'].append(path)
    size = lambda paths: sum(int(inputs[p].get('size', 0)) for p in paths)
    return {'groups': groups, 'bytes': {name: size(paths) for name, paths in groups.items()},
            'declared': [{'path': e['path'], 'what': e['what'], 'game_derived': e['game_derived'],
                          'files': len(covered[e['path']]), 'bytes': size(covered[e['path']])} for e in declared],
            'unused': [e['path'] for e in declared if not covered[e['path']]]}


def tracked_files(repo: str) -> set[str]:
    r = subprocess.run(['git', '-C', repo, 'ls-files', '-z'], capture_output=True)
    if r.returncode != 0:
        raise ValueError('git could not list the tracked files; pass --tracked FILE')
    return {p for p in r.stdout.decode('utf-8', 'surrogateescape').split('\0') if p}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--manifest', type=Path, required=True)
    ap.add_argument('--work', required=True)
    ap.add_argument('--declared', type=Path, required=True)
    ap.add_argument('--tracked', type=Path)
    ap.add_argument('--report', type=Path)
    a = ap.parse_args()
    manifest = json.loads(a.manifest.read_text(encoding='utf-8'))
    if not isinstance(manifest, dict) or not isinstance(manifest.get('inputs'), dict) or not manifest['inputs']:
        raise ValueError('manifest refused: it has no inputs')
    tracked = {line for line in a.tracked.read_text(encoding='utf-8').split('\n') if line} if a.tracked \
        else tracked_files(manifest['repo_root'])
    result = classify(manifest['inputs'], a.work.replace('\\', '/'), tracked, load_declared(a.declared))
    for name in GROUPS:
        print(f'{len(result["groups"][name]):6d} files {result["bytes"][name] / 1e6:10.1f} MB  {name}')
    for entry in result['declared']:
        print(f'  declared {"GAME-DERIVED" if entry["game_derived"] else "            "} {entry["files"]:5d} files '
              f'{entry["bytes"] / 1e6:8.1f} MB  {entry["path"]}  ({entry["what"]})')
    for path in result['unused']:
        print('  declared but not read:', path)
    for path in result['groups']['undeclared']:
        print(f'  UNDECLARED {int(manifest["inputs"][path].get("size", 0)):>10}  {path}')
    if a.report:
        a.report.write_text(json.dumps(result, indent=1, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
    print('COMPILE_CLOSURE', 'complete' if not result['groups']['undeclared'] else 'UNDECLARED_INPUTS', flush=True)
    return 0 if not result['groups']['undeclared'] else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError) as error:
        print('COMPILE_CLOSURE_REFUSED:', error, file=sys.stderr)
        sys.exit(2)
