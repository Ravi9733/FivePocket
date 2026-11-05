#!/usr/bin/env python3
"""Mutant check for build_code_headers.py: each mutant is a copy of the tool with one rule removed or changed;
test_build_code_headers.py (beside this file) must fail on it. First a control on an unchanged copy. Exits 1 if the
control fails, a mutant survives, or a mutant no longer applies. The tool itself is not touched.

  mutants_code_headers.py [--only TEXT]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'build_code_headers.py'
TESTS = HERE / 'test_build_code_headers.py'

MUTANTS = [
    ('a function runs to the end of the chunk', '    end = next((i for i in range(starts[0] + 1, len(lines)) if lines[i].startswith(NEXT)), len(lines))', '    end = len(lines)'),
    ('only another generated function ends a function', "NEXT = ('PPC_FUNC_IMPL(', 'PPC_WEAK_FUNC(', 'PPC_FUNC(')", "NEXT = ('PPC_FUNC_IMPL(',)"),
    ('a function that is there twice is taken', '    if len(starts) != 1:', '    if len(starts) < 1:'),
    ('a function that is missing is not reported as such', '    if len(starts) != 1:', '    if len(starts) > 1:'),
    ('the first line stays as generated', "    out, at = [part['first_line']], 1", '    out, at = [generated[0]], 1'),
    ('the removed stretch is not checked', "        if sha('\\n'.join(raw[edit['at']:edit['at'] + edit['remove']]).encode('utf-8')) != edit['removed_sha256']:", '        if False:'),
    ('the removed stretch is hashed as renamed', "        if sha('\\n'.join(raw[edit['at']:edit['at'] + edit['remove']]).encode('utf-8')) != edit['removed_sha256']:", "        if sha('\\n'.join(generated[edit['at']:edit['at'] + edit['remove']]).encode('utf-8')) != edit['removed_sha256']:"),
    ('an edit beyond the function is applied', '        if edit[\'at\'] + edit[\'remove\'] > len(raw):\n            raise ValueError(f\'function {part["function"]}: an edit lies beyond the function\')\n', ''),
    ('removed lines stay in', "        at = edit['at'] + edit['remove']\n", "        at = edit['at']\n"),
    ('inserted lines are left out', "        out += generated[at:edit['at']] + edit['insert']", "        out += generated[at:edit['at']]"),
    ('the lines behind the last edit are dropped', '    return out + generated[at:]', '    return out'),
    ('calls keep their generated names', "    generated = [IMPORT_CALL.sub(r'\\1', line) for line in raw] if part['direct_calls'] else raw", '    generated = raw'),
    ('calls are renamed whatever the recipe says', "    generated = [IMPORT_CALL.sub(r'\\1', line) for line in raw] if part['direct_calls'] else raw", "    generated = [IMPORT_CALL.sub(r'\\1', line) for line in raw]"),
    ('the header is not compared with its hash', " or sha(data) != header['sha256']:", ':'),
    ('the header is not compared with its size', "    if len(data) != header['bytes'] or ", '    if '),
    ('an absolute header path is accepted', "or path.startswith('/') or '\\\\' in path", "or '\\\\' in path"),
    ('a header path with a backslash is accepted', "or path.startswith('/') or '\\\\' in path", "or path.startswith('/')"),
    ('a header path may climb out with ..', " or '..' in path.split('/') \\\n", ' \\\n'),
    ('any file name is accepted for a header', "                or not path.endswith('.h') or path in seen:", '                or path in seen:'),
    ('two recipes may write one path', "or not path.endswith('.h') or path in seen:", "or not path.endswith('.h'):"),
    ('the schema is not checked', "or table.get('schema') != SCHEMA ", ''),
    ('the expected hash may be missing', " or not is_hash(header.get('sha256')):", ':'),
    ('the expected size may be zero', " or header['bytes'] <= 0", ''),
    ('a header without parts is accepted', "        if not isinstance(parts, list) or not parts:      # a header of text only is one that passes on to a hooked copy", "        if not isinstance(parts, list):"),
    ('the parts need not be a list', "        if not isinstance(parts, list) or not parts:      # a header of text only is one that passes on to a hooked copy", "        if not parts:"),
    ('any text is accepted as a function address', " or not ADDRESS.fullmatch(part['function']) \\\n", ' \\\n'),
    ('a lower-case address is accepted', "ADDRESS = re.compile(r'[0-9A-F]{8}')", "ADDRESS = re.compile(r'[0-9A-Fa-f]{8}')"),
    ('a negative chunk number is accepted', " or part['chunk'] < 0", ''),
    ('the chunk number may be text', "type(part.get('chunk')) is not int or ", ''),
    ('the first line may hold line breaks', "                    or '\\n' in part['first_line'] or", '                    or'),
    ('the renaming switch may be any value', "type(part.get('direct_calls')) is not bool or ", ''),
    ('an edit of the first line is accepted', "edit['at'] < max(last, 1)", "edit['at'] < last"),
    ('edits may overlap or come out of order', "edit['at'] < max(last, 1)", "edit['at'] < 1"),
    ('a negative removal is accepted', " or edit['remove'] < 0", ''),
    ('the hash of the removed lines may be missing', " or not is_hash(edit.get('removed_sha256')) \\\n", ' \\\n'),
    ('an inserted line may hold line breaks', "not all(isinstance(l, str) and '\\n' not in l for l in edit['insert'])", "not all(isinstance(l, str) for l in edit['insert'])"),
    ('an edit that does nothing is accepted', "                        or (edit['remove'] == 0 and not edit['insert']):", '                        or False:'),
    ('the inserted lines need not be a list', "                        or not isinstance(edit.get('insert'), list) or ", '                        or '),
    ('a refused header does not fail the build', "            report['status'] = 'differs'\n", ''),
    ('a refused header is written all the same', "            report['headers'][header['path']] = {'status': 'refused', 'why': str(error)}\n            continue\n", "            report['headers'][header['path']] = {'status': 'refused', 'why': str(error)}\n            data = b''\n"),
    ('a missing chunk ends the whole build', "        except (ValueError, OSError) as error:\n            report['status'] = 'differs'", "        except ValueError as error:\n            report['status'] = 'differs'"),
    ('an existing output folder is written into', '    output.mkdir(parents=True)\n', '    output.mkdir(parents=True, exist_ok=True)\n'),
    ('the exit code is 0 whatever happened', "    return 0 if report['status'] == 'complete' else 1", '    return 0'),
    ('a missing chunk folder is not reported', "    if not a.chunks.is_dir():\n        raise ValueError('the chunk folder is not there')\n", ''),
]


def run(tool: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, '-B', str(TESTS), '-v', '--failfast'], env=dict(os.environ, BUILD_CODE_HEADERS=str(tool)),
                          capture_output=True, text=True, timeout=300)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--only')
    a = ap.parse_args()
    source = TOOL.read_text()
    chosen = [(i, m) for i, m in enumerate(MUTANTS, 1) if not a.only or a.only in m[0]]
    killed = 0
    with tempfile.TemporaryDirectory() as tmp:
        copy = Path(tmp) / 'build_code_headers.py'
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
    print(f'{killed} of {len(chosen)} mutants killed')
    return 0 if killed == len(chosen) else 1


if __name__ == '__main__':
    sys.exit(main())
