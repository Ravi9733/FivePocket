#!/usr/bin/env python3
"""Freeze one attempt build into a manifest another machine can replay.

The attempt builder (tools/iter/iterate.py, cmd_build) derives every compile command at build time from this Mac's
private build tree. A second machine (the owner's Windows PC, later a player's) has none of that. This tool writes
down, for a build that already ran here:

  - every compile command, word for word, with its source, its object and the object's SHA-256 here;
  - the link command and the library's SHA-256 here;
  - the closure of files those commands read (from the compiler's own dependency files), each with its SHA-256;
  - the compiler's files they read, listed apart, so the other machine can check its own copy of the compiler.

Nothing is compiled and nothing is changed: it reads the attempt, the plans and the dependency files of the finished
build, and asks ninja for command text exactly as the builder does.

    freeze_build.py --attempts DIR --attempt v507 --base-build DIR [--extra-compile mapping-proof.json] --out DIR
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PATH_OPTIONS = ('-fprofile-use=', '-fprofile-instr-use=', '-fprofile-sample-use=', '-Wl,--version-script=',
                '-Wl,--symbol-ordering-file=', '--sysroot=', '--dependency-file=')


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def load_iterate():
    sys.path.insert(0, str(ROOT / 'tools/iter'))
    spec = importlib.util.spec_from_file_location('frozen_iterate', ROOT / 'tools/iter/iterate.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def compiler_root(command: list[str]) -> str:
    """The compiler's host folder (.../prebuilt/<host>), from the driver's own path."""
    driver = Path(command[0])
    if driver.name != 'clang++' or driver.parent.name != 'bin':
        raise SystemExit(f'unexpected compiler driver: {command[0]}')
    return str(driver.parent.parent)


def compile_command(it, cfg: dict, spec: dict, plan: dict, a: Path, out: Path, f: dict) -> tuple[list[str], Path, Path, Path]:
    """The command cmd_build's compile_one runs for this file (same statements, nothing executed but ninja -t)."""
    src = ROOT / f['source'] if f.get('source') else a / 'src' / f['name']
    obj, dep = out / 'obj' / (f['name'] + '.o'), out / 'obj' / (f['name'] + '.d')
    if 'ppc_chunk' in f:
        command, _ = it.ppc_chunk_command(cfg, int(f['ppc_chunk']), src, obj, dep)
        command += spec.get('ppc_defines', [])
        extra = spec.get('ppc_chunk_extra')
        if extra and int(f['ppc_chunk']) in extra.get('chunks', []):
            command += extra.get('flags', [])
    else:
        template = f.get('template', f['name'])
        items = [i for i in plan['plans'] if Path(i['source']).name == template]
        if len(items) != 1:
            raise SystemExit(f'{template} is not a single base build plan entry')
        command = list(json.loads((ROOT / items[0]['spec']).read_text())['command'])
        command[command.index('-c') + 1] = str(src)
        command[command.index('-o') + 1] = str(obj)
        command[1:1] = ['-iquote', str((ROOT / f['origin']).parent), *f.get('defines', [])]
        command += ['-MD', '-MF', str(dep)]
        command += f.get('extra_flags', [])
    return command, src, obj, dep


def make_dependency_words(text: str) -> list[str]:
    """The prerequisites of a compiler's Make dependency file ("target: dep dep ..."), as Make reads them: a backslash
    before a newline continues the line, "\\ " is a space inside a path (and "\\\\" a backslash), "$$" is "$"; the target
    ends at the first ':' followed by whitespace or the end (so a Windows drive letter such as "C:\\" stays in a path).
    2026-10-16, after Codex's v544 audit: splitting on any whitespace cut "headers\\ with\\ spaces/config.h" into three
    fragments that do not exist, and the caller then dropped the header silently."""
    text = text.replace('\\\r\n', ' ').replace('\\\n', ' ')
    i = 0
    while i < len(text):
        if text[i] == '\\': i += 2; continue
        if text[i] == ':' and (i + 1 == len(text) or text[i + 1] in ' \t\r\n'): break
        i += 1
    else:
        raise SystemExit('dependency file has no "target:" separator')
    words, cur, j, rest = [], [], 0, text[i + 1:]
    while j < len(rest):
        c = rest[j]
        if c == '\\' and j + 1 < len(rest) and rest[j + 1] in ' \t#\\':
            cur.append(rest[j + 1]); j += 2; continue
        if c == '$' and j + 1 < len(rest) and rest[j + 1] == '$':
            cur.append('$'); j += 2; continue
        if c in ' \t\r\n':
            if cur: words.append(''.join(cur)); cur = []
            j += 1; continue
        cur.append(c); j += 1
    if cur: words.append(''.join(cur))
    return words


def dependency_paths(depfile: Path, cwd: Path) -> list[str]:
    """Paths the compiler read, as it named them (made absolute, '..' folded, links not followed)."""
    return [os.path.normpath(p if os.path.isabs(p) else os.path.join(cwd, p))
            for p in make_dependency_words(depfile.read_text())]


def path_words(command: list[str]) -> list[str]:
    """Absolute paths named in a command, bare or behind an option that takes a path."""
    found = []
    for w in command:
        for prefix in PATH_OPTIONS:
            if w.startswith(prefix):
                w = w[len(prefix):]
                break
        if w.startswith('/'):
            found.append(w)
    return found


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('--attempts', type=Path, required=True, help='folder holding the built attempt (…/attempts)')
    ap.add_argument('--attempt', required=True)
    ap.add_argument('--base-build', type=Path, required=True, help='folder with plan.json and link-plan.json used by that build')
    ap.add_argument('--extra-compile', type=Path, action='append', default=[],
                    help='a JSON file {"command": [...], "sha256": ...} of one more compile the link consumes')
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()

    it = load_iterate()
    it.ATTEMPTS = args.attempts.resolve()
    cfg = json.loads((ROOT / 'local-analysis/attempts/config.json').read_text())
    a = it.ATTEMPTS / args.attempt
    out = a / 'build'
    spec = json.loads((a / 'attempt.json').read_text())
    base = args.base_build.resolve()
    plan = json.loads((base / 'plan.json').read_text())
    link = json.loads((base / 'link-plan.json').read_text())
    cwd = ROOT / cfg['build_cwd']
    receipt = json.loads((out / 'build.json').read_text())
    built = {r['name']: r for r in receipt['records']}

    files = list(spec['files'])
    if spec.get('ppc_chunks') == 'all':
        listed = {int(f['ppc_chunk']) for f in files if 'ppc_chunk' in f}
        corpus = cwd / 'validated-ppc'
        for path in sorted(corpus.glob('ppc_recomp.*.cpp'), key=lambda q: int(q.name.split('.')[1])):
            n = int(path.name.split('.')[1])
            if re.fullmatch(r'ppc_recomp\.\d+\.cpp', path.name) and n not in listed:
                files.append(dict(name=path.name, ppc_chunk=n, source=str(path.relative_to(ROOT))))
    if [f['name'] for f in files] != [r['name'] for r in receipt['records']]:
        raise SystemExit('the attempt names other files than the finished build recorded')

    units, inputs, compiler_inputs = [], {}, {}
    toolroot = None

    def note_input(path: str) -> None:
        if toolroot and (path == toolroot or path.startswith(toolroot + '/')):
            if os.path.isfile(path):
                compiler_inputs.setdefault(path, None)
            return
        if os.path.isfile(path):
            inputs.setdefault(path, None)

    def add_unit(name, command, src, obj, dep, expected, seconds, kind):
        nonlocal toolroot
        root = compiler_root(command)
        if toolroot not in (None, root):
            raise SystemExit(f'two compilers in one build: {toolroot} and {root}')
        toolroot = root
        if not dep.exists():
            raise SystemExit(f'no dependency file for {name}: {dep} (was this object built or taken from a cache?)')
        got = sha256(obj)
        if got != expected:
            raise SystemExit(f'{name}: the object on disk is not the one the build recorded')
        note_input(str(src))
        for p in dependency_paths(dep, cwd):
            if not os.path.isfile(p):   # (2026-10-16) a dependency the compiler named and that is not a file: a parse error or a lost input, never silently dropped
                raise SystemExit(f'{name}: the dependency file names {p}, which is not a file')
            note_input(p)
        for p in path_words(command):
            if p not in (str(obj), str(dep), command[0]):  # what the command writes, and the driver itself, are not inputs
                note_input(p)
        units.append(dict(name=name, kind=kind, command=command, source=str(src), object=str(obj), depfile=str(dep),
                          object_sha256=got, seconds_here=seconds))

    for f in files:
        command, src, obj, dep = compile_command(it, cfg, spec, plan, a, out, f)
        r = built[f['name']]
        if r['object'] != str(obj):
            raise SystemExit(f'{f["name"]}: object path differs from the finished build')
        add_unit(f['name'], command, src, obj, dep, r['object_sha256'], r['seconds'], 'chunk' if 'ppc_chunk' in f else 'runtime')
    for extra in args.extra_compile:
        e = json.loads(extra.read_text())
        command = list(e['command'])
        src, obj = Path(command[command.index('-c') + 1]), Path(command[command.index('-o') + 1])
        dep = Path(command[command.index('-MF') + 1])
        add_unit(obj.name[:-2], command, src, obj, dep, e['sha256'], None, 'extra')

    # The link command, as cmd_build assembles it.
    records = receipt['records']
    replaced = {r['replaces']: r['object'] for r in records if r['replaces']}
    added = [r['object'] for r in records if not r['replaces']]
    library = out / f'libhost_guest_trace-attempt-{args.attempt}.so'
    lc = [replaced.get(x, x) for x in link['command']]
    lc[lc.index('-o'):lc.index('-o')] = added + spec.get('link_extra', [])
    lc = ['--dependency-file=' + str(out / 'link.d') if x.startswith('--dependency-file=') else x for x in lc]
    if spec.get('version_script'):
        vs = [i for i, x in enumerate(lc) if x.startswith('-Wl,--version-script=')]
        if len(vs) != 1:
            raise SystemExit('version_script: the base link command has no single --version-script')
        lc[vs[0]] = '-Wl,--version-script=' + str(a / spec['version_script'])
    if sum(lc.count(v) for v in replaced.values()) != len(replaced):
        raise SystemExit('link command did not take every replaced object exactly once')
    lc[lc.index('-o') + 1] = str(library)
    if sha256(library) != receipt['library_sha256']:
        raise SystemExit('the library on disk is not the one the build recorded')
    produced = {u['object'] for u in units}
    link_inputs = []
    for p in path_words(lc):
        if p in produced or p == str(library) or p.endswith('/link.d'):
            continue
        if toolroot and p.startswith(toolroot):
            continue
        if not os.path.isfile(p):
            raise SystemExit(f'link input is not a file here: {p}')
        note_input(p)
        link_inputs.append(p)
    for p, expected in link['inputs'].items():
        if p in produced or p in replaced:
            continue
        if os.path.isfile(p) and not (toolroot and p.startswith(toolroot)) and sha256(Path(p)) != expected:
            raise SystemExit(f'base link input changed since the base build: {p}')

    outside = sorted(p for p in inputs if not p.startswith(str(ROOT) + '/'))
    if outside:
        raise SystemExit('inputs outside the repository and the compiler:\n  ' + '\n  '.join(outside[:20]))
    for table in (inputs, compiler_inputs):
        for p in table:
            table[p] = dict(sha256=sha256(Path(p)), size=os.path.getsize(p))
    lowered = {}
    for p in inputs:
        lowered.setdefault(p.lower(), []).append(p)
    clashes = [v for v in lowered.values() if len(v) > 1]
    if clashes:
        raise SystemExit(f'paths that differ only by letter case (one folder on Windows): {clashes[:5]}')

    args.out.mkdir(parents=True, exist_ok=True)
    manifest = dict(
        schema='frozen-build/1', attempt=args.attempt, repo_root=str(ROOT), compiler_root=toolroot, cwd=str(cwd),
        build_jobs_here=spec.get('build_jobs'), units=units,
        link=dict(command=lc, library=str(library), library_sha256=receipt['library_sha256'],
                  seconds_here=receipt['link_seconds'], inputs=sorted(set(link_inputs))),
        inputs={os.path.relpath(p, ROOT): v for p, v in sorted(inputs.items())},
        compiler_inputs={os.path.relpath(p, toolroot): v for p, v in sorted(compiler_inputs.items())})
    (args.out / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n', encoding='utf-8', newline='\n')
    (args.out / 'inputs.txt').write_text(''.join(p + '\n' for p in manifest['inputs']), encoding='utf-8', newline='\n')
    total = sum(v['size'] for v in manifest['inputs'].values())
    print(f'units {len(units)}  inputs {len(inputs)} ({total / 1e9:.2f} GB)  compiler files read {len(compiler_inputs)}  '
          f'link words {len(lc)}  compiler {toolroot}')


if __name__ == '__main__':
    main()
