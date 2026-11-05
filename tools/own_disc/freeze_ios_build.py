#!/usr/bin/env python3
"""Freeze one finished iOS build (claude_build.py) into a manifest another Mac can replay: frozen-build-apple/1.

claude_build.py links one static Mach-O from objects of four kinds of earlier builds: the units it compiled itself
(its receipt), the generated chunks (a chunk-object folder with a receipt: a command template per chunk), the host
objects of a V3 build plan, and objects of a focused bot build. This tool writes down, for such a build:

  - every object the link reads, with the command that made it (word for word), its source and its SHA-256 now;
  - the link command, with its file lists written out, and the unsigned executable's SHA-256;
  - the closure of files each compile reads, asked from clang itself (the same command, preprocessing only, with
    -MD: the compiler's own dependency output, VFS overlays applied), each file with its SHA-256 and size, sorted into
    the toolchain's (below Xcode's Developer folder: checked by the Xcode version, never copied) and the rest;
  - the static libraries and frameworks the link names, with their SHA-256.

Nothing is compiled and nothing is changed: it reads the receipts, the file lists and the files. An object whose
command cannot be found, or whose recorded hash differs from the file now, stops the run (exit 1), so the manifest
never pairs an object with a command that did not make it.

    freeze_ios_build.py --build DIR --chunks DIR --plan FILE --extra-receipt FILE [...] --xcode DIR --out FILE [--jobs N]
                        [--root NAME=PATH ...] [--snapshot NEW_DIR]

--build is the claude_build.py output folder (receipt.json, runtime.LinkFileList, shell.LinkFileList); --chunks the
chunk-object folder whose receipt.json gives the chunk commands; --plan a V3 build-plan.json; --extra-receipt a focused
build's receipt.json (ios-bot-focused-build/1).

--snapshot copies (APFS clones where it can) every input outside Xcode, the profile data a command names, the static
libraries of the link and the reference executable into NEW_DIR/<root>/..., one folder per --root (NAME=PATH; a path may
be given in several spellings, e.g. a checkout and a link to it), and writes each VFS overlay rewritten to point there.
The manifest then carries the snapshot's place and the map a replay uses (replay_ios_build.py --from-snapshot): later
edits of the sources the build read no longer move the replay. An input outside every root is refused. The manifest names private files (game-derived sources among them):
it stays under local-analysis/.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def output_of(argv: list[str]) -> str | None:
    for i, w in enumerate(argv[:-1]):
        if w == '-o':
            return argv[i + 1]
    return None


def source_of(argv: list[str]) -> str | None:
    """the one compiled file: the last word ending like a source file that is not an option's value"""
    takes_value = {'-o', '-include', '-iquote', '-I', '-isystem', '-isysroot', '-ivfsoverlay', '-MF', '-MT', '-MQ', '-target',
                   '-x', '-arch', '-F', '-Xclang', '--serialize-diagnostics', '-index-store-path', '-fmodules-cache-path'}
    found = None
    skip = False
    for w in argv[1:]:
        if skip:
            skip = False
            continue
        if w in takes_value:
            skip = True
            continue
        if not w.startswith('-') and re.search(r'\.(c|cc|cpp|cxx|m|mm)$', w):
            found = w
    return found


def collect_commands(build: Path, chunks: Path, plan: Path, extras: list[Path]) -> dict[str, dict]:
    """object path -> {command, source, origin, recorded}"""
    out: dict[str, dict] = {}

    def add(obj: str, command: list[str], source: str | None, origin: str, recorded: str | None = None):
        out.setdefault(os.path.realpath(obj), {'command': command, 'source': source, 'origin': origin, 'recorded': recorded})

    receipt = json.loads((build / 'receipt.json').read_text(encoding='utf-8'))
    for c in receipt['commands']:
        if c['label'].startswith('compile-'):
            add(output_of(c['argv']), c['argv'], source_of(c['argv']), f'build {receipt.get("version")}')
    # pgo_chunks.py: the plan's command of chunk 100 with its source replaced, then the extra flags, the profile and -o
    cr = json.loads((chunks / 'receipt.json').read_text(encoding='utf-8'))
    base = cr['command_template']
    placeholder = next(w for w in base if w.endswith('ppc_recomp.100.cpp'))
    tail = list(cr.get('extra_flags', []))
    if cr.get('profile'):
        tail += [f'-fprofile-instr-use={cr["profile"]}', '-Wno-profile-instr-unprofiled', '-Wno-backend-plugin']
    for n, c in cr['chunks'].items():
        obj = chunks / f'{Path(c["source"]).name}.o'
        add(str(obj), [c['source'] if w == placeholder else w for w in base] + tail + ['-o', str(obj)], c['source'], 'chunks')
    for u in json.loads(plan.read_text(encoding='utf-8'))['compile_units']:
        add(u['object'], u['command'], u['source'], 'plan', None)
    for e in extras:
        r = json.loads(e.read_text(encoding='utf-8'))
        objects = r.get('new_objects', {})
        for c in r.get('commands', []):
            argv = c.get('argv') or c.get('command')
            if isinstance(argv, list) and output_of(argv):
                add(output_of(argv), argv, source_of(argv), f'receipt {e.parent.name}', objects.get(output_of(argv)))
    return out


def file_lists(link: list[str]) -> dict[str, list[str]]:
    """the link's file lists as the command names them (-filelist PATH, -Wl,-filelist,PATH) -> their lines: the replay builds
    its own lists from this record (a list is no input of any compile, so no snapshot holds it)"""
    out = {}
    for i, w in enumerate(link):
        path = link[i + 1] if w == '-filelist' and i + 1 < len(link) else (w.split(',', 2)[2] if w.startswith('-Wl,-filelist,') else None)
        if path:
            out[path] = [line.strip() for line in Path(path).read_text(encoding='utf-8').splitlines() if line.strip()]
    return out


def link_objects(build: Path) -> tuple[list[str], list[str]]:
    receipt = json.loads((build / 'receipt.json').read_text(encoding='utf-8'))
    link = next(c['argv'] for c in receipt['commands'] if c['label'] == 'link')
    objects: list[str] = []
    for name in ('shell.LinkFileList', 'runtime.LinkFileList'):
        objects += [line.strip() for line in (build / name).read_text(encoding='utf-8').splitlines() if line.strip()]
    return link, objects


def overlay_map(command: list[str]) -> dict[str, str]:
    """virtual path -> the file it stands for, from the command's -ivfsoverlay files (use-external-names false: clang
    reports the virtual name, so the closure is read through this map)"""
    out: dict[str, str] = {}

    def walk(entry: dict, parent: str) -> None:
        name = entry['name'] if entry['name'].startswith('/') else f'{parent}/{entry["name"]}'
        if entry.get('type') == 'file':
            out[os.path.normpath(name)] = entry['external-contents']
        for child in entry.get('contents', []):
            walk(child, name)

    for i, w in enumerate(command[:-1]):
        if w == '-ivfsoverlay':
            for root in json.loads(Path(command[i + 1]).read_text(encoding='utf-8'))['roots']:
                walk(root, '')
    return out


def depends(command: list[str], workdir: Path) -> list[str]:
    """the files the compile reads, by clang's own -MD output for the same command run as preprocessing only"""
    argv, skip = [], False
    for w in command:
        if skip:
            skip = False
            continue
        if w in ('-o', '-MF', '-MT', '-MQ', '--serialize-diagnostics'):
            skip = True
            continue
        if w in ('-c', '-MD', '-MMD') or w.startswith('-fprofile-instr-use=') or w.startswith('-fprofile-use='):
            continue
        argv.append(w)
    with tempfile.TemporaryDirectory(dir=workdir) as tmp:
        dep = Path(tmp) / 'unit.d'
        r = subprocess.run(argv + ['-E', '-MD', '-MF', str(dep), '-o', os.devnull], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(f'preprocessing failed: {r.stderr[-600:]}')
        text = dep.read_text(encoding='utf-8').replace('\\\n', ' ')
    body = text.split(':', 1)[1]
    vfs = overlay_map(command)
    files = [p.replace('\\ ', ' ') for p in re.findall(r'(?:\\ |[^ \n])+', body)]
    return [vfs.get(os.path.normpath(f), f) for f in files]


def take_snapshot(snap: Path, roots: list[tuple[str, str]], inputs: dict, libraries: dict, overlays: list[str],
                  reference: Path, words: set[str]) -> dict:
    """copies the build's own files into snap/<root>/...; returns {dir, map: [[old, new], ...]}"""
    if not roots:
        raise SystemExit('--snapshot needs --root NAME=PATH for every place the build read files from')
    pairs = sorted(((path.rstrip('/'), str(snap.resolve() / name)) for name, path in roots), key=lambda p: -len(p[0]))

    def place(path: str) -> Path:
        for old, new in pairs:
            if path == old or path.startswith(old + '/'):
                return Path(new + path[len(old):])
        raise SystemExit(f'outside every --root: {path}')

    pattern = re.compile('|'.join(re.escape(old + '/') for old, _ in pairs))
    table = {old + '/': new + '/' for old, new in pairs}

    def remap_text(text: str) -> str:
        return pattern.sub(lambda m: table[m.group(0)], text)     # one pass: a new path is never rewritten again

    snap.mkdir(parents=True)
    files = list(inputs) + [p for p in libraries if p.startswith('/')] + [str(reference)]
    for name in files:
        dst = place(name)
        dst.parent.mkdir(parents=True, exist_ok=True)
        if not dst.exists():
            subprocess.run(['/bin/cp', '-c', '-p', name, str(dst)], check=True)
    # sources that include another file by its absolute path (wrapper headers of the iOS stages): the path rewritten to
    # the snapshot, so the replay reads the copy; the new hash is recorded for the replay's input check
    rewritten = {}
    for name in inputs:
        if Path(name).suffix not in ('.h', '.hpp', '.inc', '.c', '.cc', '.cpp', '.m', '.mm'):
            continue
        dst = place(name)
        text = dst.read_text(encoding='utf-8', errors='surrogateescape')
        new = remap_text(text)
        if new != text:
            dst.unlink()                                  # a clone: replace the snapshot's copy, never the original
            dst.write_text(new, encoding='utf-8', errors='surrogateescape', newline='')
            rewritten[name] = sha256(dst)
    # the commands name files through links of the checkout (local-analysis/attempts -> .attempts): the same relative
    # links in the snapshot, so every spelling the commands use reaches the copied file
    links = 0
    for word in words:
        for old, new in pairs:
            at = word.find(old + '/')
            if at < 0 or (at > 0 and word[at - 1] not in '=,' and not word.startswith('-')):
                continue
            rel = Path(word[at + len(old) + 1:])
            here = Path(old)
            for part in rel.parts:
                here = here / part
                if here.is_symlink() and not os.path.isabs(os.readlink(here)):
                    mirror = Path(new) / here.relative_to(old)
                    if not mirror.is_symlink():
                        mirror.parent.mkdir(parents=True, exist_ok=True)
                        mirror.symlink_to(os.readlink(here))
                        links += 1
                if not here.exists():
                    break
            break
    # include and framework folders the commands name: present (empty if no file of theirs was read), so the search
    # order a replay sees is the build's
    for word in words:
        path = word[2:] if word[:2] in ('-I', '-F') and len(word) > 2 else word
        if path.startswith('/') and Path(path).is_dir():
            try:
                place(os.path.realpath(path)).mkdir(parents=True, exist_ok=True)
            except SystemExit:
                pass                                     # a folder outside the roots (Xcode's): not the snapshot's
    # every file an overlay stands in for, also as a real file at its virtual place: the compiler driver looks for its
    # input on the disk before the overlay applies, and the build's tree had (older) files there
    virtual = 0
    for overlay in overlays:
        for name, external in overlay_map(['-ivfsoverlay', overlay]).items():
            try:
                dst = place(name)
            except SystemExit:
                continue
            if not dst.exists():
                dst.parent.mkdir(parents=True, exist_ok=True)
                subprocess.run(['/bin/cp', '-c', '-p', os.path.realpath(external), str(dst)], check=True)
                virtual += 1
    # the overlays, rewritten: the build's overlays name one file in two spellings (the checkout and its link), which the
    # snapshot folds into one place; an entry whose file the build never read is not in the snapshot and would hide the
    # real file there, so only entries whose file is in the snapshot stay, the first one per name
    dropped = 0
    for overlay in overlays:
        dst = place(os.path.realpath(overlay))
        dst.parent.mkdir(parents=True, exist_ok=True)
        data = json.loads(remap_text(Path(overlay).read_text(encoding='utf-8')))
        kept, seen = [], set()
        for root in data['roots']:
            if root.get('type') != 'file':
                kept.append(root)
                continue
            if root['name'] in seen or not Path(root['external-contents']).is_file():
                dropped += 1
                continue
            seen.add(root['name'])
            kept.append(root)
        data['roots'] = kept
        dst.write_text(json.dumps(data, indent=1) + '\n', encoding='utf-8', newline='\n')
    return {'dir': str(snap.resolve()), 'map': [list(p) for p in pairs], 'files': len(files), 'overlays': len(overlays), 'links': links, 'virtual_files': virtual, 'overlay_entries_dropped': dropped, 'rewritten': rewritten}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--chunks', type=Path, required=True)
    ap.add_argument('--plan', type=Path, required=True)
    ap.add_argument('--extra-receipt', type=Path, action='append', default=[])
    ap.add_argument('--xcode', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--root', action='append', default=[], metavar='NAME=PATH')
    ap.add_argument('--snapshot', type=Path)
    a = ap.parse_args(argv)
    if a.snapshot and a.snapshot.exists():
        print(f'refusing: {a.snapshot} exists', file=sys.stderr)
        return 2
    if a.out.exists():
        print(f'refusing: {a.out} exists', file=sys.stderr)
        return 2
    xcode = a.xcode.resolve()
    commands = collect_commands(a.build, a.chunks, a.plan, a.extra_receipt)
    link, objects = link_objects(a.build)
    units, problems = [], []
    for obj in objects:
        c = commands.get(os.path.realpath(obj))
        if c is None:
            problems.append(f'no command for {obj}')
            continue
        now = sha256(Path(obj))
        if c['recorded'] and c['recorded'] != now:
            problems.append(f'{obj}: recorded {c["recorded"][:12]}, now {now[:12]}')
        units.append({'object': obj, 'object_sha256': now, 'source': c['source'], 'origin': c['origin'], 'command': c['command']})
    if problems:
        print('\n'.join(problems[:40]), file=sys.stderr)
        return 1
    work = a.out.parent
    with ThreadPoolExecutor(a.jobs) as pool:
        closures = list(pool.map(lambda u: depends(u['command'], work), units))
    toolchain, inputs = {}, {}
    for u, files in zip(units, closures):
        names = []
        for f in files:
            p = Path(f).resolve()
            names.append(str(p))
            table = toolchain if p.is_relative_to(xcode) else inputs
            key = str(p.relative_to(xcode)) if table is toolchain else str(p)
            if key not in table:
                table[key] = {'sha256': sha256(p), 'size': p.stat().st_size}
        u['reads'] = sorted(set(names))
    libraries = {}
    for w in link:
        if w.endswith('.a') and Path(w).is_file():
            p = Path(w).resolve()
            libraries[str(p.relative_to(xcode)) if p.is_relative_to(xcode) else str(p)] = {'sha256': sha256(p), 'size': p.stat().st_size}
    receipt = json.loads((a.build / 'receipt.json').read_text(encoding='utf-8'))
    output = link[link.index('-o') + 1]
    reference = (a.build / Path(output).parent.name / Path(output).name).resolve()   # the unsigned app beside the receipt
    if receipt.get('unsigned_sha256') and (not reference.is_file() or sha256(reference) != receipt['unsigned_sha256']):
        print(f'the unsigned executable {reference} is not the receipt\'s', file=sys.stderr)
        return 1
    profiles = sorted({w.split('=', 1)[1] for u in units for w in u['command'] if w.startswith('-fprofile-instr-use=')})
    for prof in profiles:
        inputs[str(Path(prof).resolve())] = {'sha256': sha256(Path(prof)), 'size': Path(prof).stat().st_size}
    overlays = sorted({u['command'][i + 1] for u in units for i, w in enumerate(u['command'][:-1]) if w == '-ivfsoverlay'})
    snapshot = None
    if a.snapshot:
        words = {w for u in units for w in u['command']} | set(link)
        snapshot = take_snapshot(a.snapshot, [tuple(r.split('=', 1)) for r in a.root], inputs, libraries, overlays, reference, words)
    manifest = {
        'schema': 'frozen-build-apple/1', 'build': str(a.build), 'version': receipt.get('version'),
        'xcode': str(xcode), 'xcode_version': subprocess.run(['/usr/bin/xcodebuild', '-version'], capture_output=True, text=True,
                                                            env=dict(os.environ, DEVELOPER_DIR=str(xcode))).stdout.split('\n')[:2],
        'units': units,
        'link': {'command': link, 'objects': objects, 'file_lists': file_lists(link), 'unsigned_sha256': receipt.get('unsigned_sha256')},
        'inputs': dict(sorted(inputs.items())), 'toolchain_inputs': dict(sorted(toolchain.items())),
        'libraries': libraries, 'overlays': overlays, 'profiles': profiles,
    }
    manifest['link']['reference'] = str(reference)
    if snapshot:
        manifest['snapshot'] = snapshot
    with a.out.open('x', encoding='utf-8', newline='\n') as f:
        f.write(json.dumps(manifest, indent=1) + '\n')
    origins: dict[str, int] = {}
    for u in units:
        origins[u['origin']] = origins.get(u['origin'], 0) + 1
    print(f'FROZEN {len(units)} units ({", ".join(f"{k} {v}" for k, v in sorted(origins.items()))}), {len(inputs)} inputs, '
          f'{len(toolchain)} toolchain files, {len(libraries)} libraries -> {a.out}', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
