#!/usr/bin/env python3
"""Replay a frozen build (freeze_build.py) on another machine and say, unit by unit, whether it made the same code.

Runs on Windows, macOS or Linux with nothing but Python 3.9+ and the same compiler release unpacked. It

  1. checks every input file and every compiler file the build read against the manifest's SHA-256s,
  2. runs every compile command with the paths of this machine in place of the first machine's,
  3. links, and
  4. writes results.json: for each object and for the library, its SHA-256 and its code identity
     (elf_sections.code_identity: everything except the compiler's name and debug paths).

It never starts more compiles than memory allows. Two may always run; a further one starts only when the machine still
has --reserve-gb plus --per-job-gb to hand out (physical memory and commit room, whichever is smaller) and at least
--spacing seconds after the previous start, so that the previous one's appetite is visible first. (The first version
looked only at the moment of starting and launched twelve large units at once on a 16 GB PC with 2.7 GB available;
the PC began to swap. 2026-10-07.) Finished objects are kept; a second run compiles only what is missing or changed.

    replay_build.py --manifest manifest.json --root DIR --compiler DIR --jobs N [--only NAME ...] [--no-link]
                    [--map REL=DIR ...] [--pending-input REL ...] [--input-identity REL=HEX ...] [--state-dir DIR]

--map puts a folder that lay below the first machine's root somewhere else on this machine (the first run's work
folder -> this run's): REL is its path below that root, with forward slashes. --pending-input names an input that a
later step makes from this replay's own objects; it may be missing while --no-link is given and is checked like every
other input when the link runs. --state-dir takes the results, the state and the link's response file (default: the
root).
"""
from __future__ import annotations

import argparse
import concurrent.futures
import ctypes
import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import elf_sections  # noqa: E402

WINDOWS = os.name == 'nt'


def available_gb() -> float:
    """Memory the system can still hand out now: physical memory, or on Windows room under the commit limit if smaller."""
    if WINDOWS:
        class Status(ctypes.Structure):
            _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong),
                        ('avail', ctypes.c_ulonglong), ('total_page', ctypes.c_ulonglong), ('avail_page', ctypes.c_ulonglong),
                        ('total_virtual', ctypes.c_ulonglong), ('avail_virtual', ctypes.c_ulonglong),
                        ('avail_extended', ctypes.c_ulonglong)]
        s = Status()
        s.length = ctypes.sizeof(s)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(s))
        return min(s.avail, s.avail_page) / 2**30
    if sys.platform == 'darwin':
        out = subprocess.run(['vm_stat'], capture_output=True, text=True).stdout
        page = int(out.split('page size of ')[1].split()[0])
        count = lambda key: int(out.split(key + ':')[1].split('.')[0])
        return (count('Pages free') + count('Pages inactive') + count('Pages purgeable')) * page / 2**30
    for line in open('/proc/meminfo'):
        if line.startswith('MemAvailable:'):
            return int(line.split()[1]) / 2**20
    return 0.0


def may_start(running: int, available: float, since_last_start: float, jobs: int, reserve: float, per_job: float,
              spacing: float) -> bool:
    """The admission rule for one more compile (see the module text)."""
    if running >= jobs:
        return False
    if running < 2:
        return True
    return available >= reserve + per_job and since_last_start >= spacing


class Translator:
    """First machine's paths -> this machine's, in command words."""

    def __init__(self, manifest: dict, root: str, compiler: str, maps=()):
        clean = lambda path: path.replace('\\', '/').rstrip('/')
        self.compiler, self.root = clean(compiler), clean(root)
        # (a folder moved by --map: its path below the first machine's root, and where it is here; the longest first)
        self.maps = sorted(((rel.replace('\\', '/').strip('/'), clean(new)) for rel, new in maps), key=lambda m: -len(m[0]))
        if any(not rel or '..' in rel.split('/') for rel, _ in self.maps) or len({rel for rel, _ in self.maps}) != len(self.maps):
            raise ValueError('--map wants REL=DIR with a path below the root, each once')
        self.pairs = [(manifest['compiler_root'], self.compiler)] \
            + [(manifest['repo_root'] + '/' + rel, new) for rel, new in self.maps] + [(manifest['repo_root'], self.root)]
        # one pass, the longest match first: a replaced path is never looked at again
        table = dict(self.pairs)
        pattern = re.compile('|'.join(re.escape(old) for old in sorted(table, key=len, reverse=True)))
        self._replace = lambda w: pattern.sub(lambda m: table[m.group(0)], w)

    def word(self, w: str) -> str:
        return self._replace(w)

    def local(self, rel: str) -> str:
        """Where an input lies here: a path below the first machine's root, as the manifest names it."""
        for moved, new in self.maps:
            if rel == moved or rel.startswith(moved + '/'):
                return new + rel[len(moved):]
        return self.root + '/' + rel

    def command(self, words: list[str]) -> list[str]:
        out = [self.word(w) for w in words]
        if WINDOWS and not out[0].lower().endswith('.exe'):
            out[0] += '.exe'
        return out

    def reproducible_flags(self, cwd_there: str) -> list[str]:
        """Make paths written into objects read as on the first machine (macros and debug records)."""
        # the root first and a moved folder after it: where two apply, the compiler takes the later (and the longer)
        ordered = [self.pairs[0], self.pairs[-1]] + self.pairs[-2:0:-1]
        flags = [f'-ffile-prefix-map={new}={old}' for old, new in ordered]
        return flags + ['-ffile-reproducible', f'-fdebug-compilation-dir={cwd_there}']


def check_inputs(table: dict, base, what: str, pending=(), identities=None) -> list[str]:
    """base: the folder the names are below, or a function from a name to its place here. pending: names left out.
    identities: name -> archive identity; such an input is a library this run built on this host, and it may differ
    from the first machine's bytes in the compiler's name section of its objects and in nothing else."""
    wrong = []
    for rel, want in table.items():
        if rel in pending:
            continue
        path = base(rel) if callable(base) else os.path.join(base, rel)
        if not os.path.isfile(path):
            wrong.append(f'{what} missing: {rel}')
        elif os.path.getsize(path) != want['size'] or elf_sections.file_sha256(path) != want['sha256']:
            found = same_objects(path) if rel in (identities or {}) else ''
            if not found or found != identities[rel]:          # (a file that is no library of objects has no identity)
                wrong.append(f'{what} differs: {rel}')
    return wrong


def same_objects(path) -> str:
    """The archive identity of a file, or '' when it is no archive of objects."""
    try:
        return elf_sections.archive_identity(path)[0]
    except (ValueError, OSError):
        return ''


def state_folder(named, root: str) -> Path:
    """Where results, state and the response file go: absolute, because the link runs in another folder."""
    return Path(named or root).absolute()


def response_file(path: Path, words: list[str]) -> None:
    def quote(w: str) -> str:
        return '"' + w.replace('\\', '\\\\').replace('"', '\\"') + '"' if (not w or any(c in w for c in ' \t"\\')) else w
    path.write_text('\n'.join(quote(w) for w in words) + '\n', encoding='utf-8', newline='\n')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--manifest', type=Path, required=True)
    ap.add_argument('--root', required=True, help="this machine's copy of the first machine's repository root")
    ap.add_argument('--compiler', required=True, help="this machine's compiler host folder (…/prebuilt/<host>)")
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--reserve-gb', type=float, default=1.5)
    ap.add_argument('--per-job-gb', type=float, default=1.0)
    ap.add_argument('--spacing', type=float, default=4.0, help='seconds between starts beyond the first two')
    ap.add_argument('--low-priority', action='store_true',
                    help='Windows: run compiles below normal priority (on hybrid Intel chips this moves them to the slow cores)')
    ap.add_argument('--only', nargs='*', help='unit names to build (default: all)')
    ap.add_argument('--no-link', action='store_true')
    ap.add_argument('--link-extra', action='append', default=[],
                    help='one word added to the link command, repeatable; write --link-extra=-Wl,... (with the equals sign)')
    ap.add_argument('--variant', default='', help='suffix for the library and the results file of a link variant (--variant=-name)')
    ap.add_argument('--skip-input-check', action='store_true')
    ap.add_argument('--plain', action='store_true', help='do not add the path-reproducibility flags')
    ap.add_argument('--map', action='append', default=[], metavar='REL=DIR',
                    help="a folder below the first machine's root that lies elsewhere here (repeatable)")
    ap.add_argument('--pending-input', action='append', default=[], metavar='REL',
                    help='an input made later from this replay\'s objects: may be missing with --no-link (repeatable)')
    ap.add_argument('--input-identity', action='append', default=[], metavar='REL=HEX',
                    help="an input that is a library this run built on this host: accepted with the first machine's bytes, or with this "
                         "archive identity (elf_sections.py --archive: the same objects, the compiler's name section left out)")
    ap.add_argument('--state-dir', help='folder for the results, the state and the response file (default: the root)')
    args = ap.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
    if any('=' not in m for m in args.map):
        ap.error('--map wants REL=DIR')
    if args.pending_input and not args.no_link:
        ap.error('--pending-input needs --no-link: the link reads every input')
    if any(rel not in manifest['inputs'] for rel in args.pending_input):
        ap.error('--pending-input names no input of the manifest')
    identities = dict(item.split('=', 1) for item in args.input_identity if '=' in item)
    if len(identities) != len(args.input_identity) or any(rel not in manifest['inputs'] or not re.fullmatch('[0-9a-f]{64}', value) for rel, value in identities.items()):
        ap.error('--input-identity wants REL=HEX for an input of the manifest')
    tr = Translator(manifest, args.root, args.compiler, [m.split('=', 1) for m in args.map])
    root = tr.root
    state_dir = state_folder(args.state_dir, root)
    state_dir.mkdir(parents=True, exist_ok=True)
    results_path = state_dir / f'replay-results{args.variant}.json'
    state_path = state_dir / 'replay-state.json'
    state = json.loads(state_path.read_text()) if state_path.exists() else {}
    started = time.time()

    wrong = []
    if not args.skip_input_check:
        wrong = check_inputs(manifest['inputs'], tr.local, 'input', set(args.pending_input), identities) \
            + check_inputs(manifest['compiler_inputs'], tr.compiler, 'compiler file')
        print(f'inputs checked: {len(manifest["inputs"])} files, {len(manifest["compiler_inputs"])} compiler files, '
              f'{len(wrong)} wrong ({time.time() - started:.0f} s)', flush=True)
        for line in wrong[:40]:
            print('  ' + line, flush=True)
        if any(w.startswith('input') for w in wrong):
            return 2

    cwd = tr.word(manifest['cwd'])
    os.makedirs(cwd, exist_ok=True)
    extra = [] if args.plain else tr.reproducible_flags(manifest['cwd'])
    units = [u for u in manifest['units'] if not args.only or u['name'] in args.only]
    units.sort(key=lambda u: -(u.get('seconds_here') or 0))
    lock = threading.Lock()
    running = [0]
    low = [available_gb()]
    start_available = low[0]
    stop = threading.Event()

    def watch():
        while not stop.wait(0.5):
            low[0] = min(low[0], available_gb())
    threading.Thread(target=watch, daemon=True).start()

    last_start = [0.0]
    most = [0]

    def gate():
        while True:
            with lock:
                now = time.time()
                if may_start(running[0], available_gb(), now - last_start[0], args.jobs, args.reserve_gb, args.per_job_gb,
                             args.spacing):
                    running[0] += 1
                    most[0] = max(most[0], running[0])
                    last_start[0] = now
                    return
            time.sleep(0.5)

    def build(u: dict) -> dict:
        command = tr.command(u['command']) + extra
        obj = tr.word(u['object'])
        key = elf_sections.hashlib.sha256(json.dumps(command).encode()).hexdigest()
        old = state.get(u['name'])
        if old and old.get('key') == key and os.path.isfile(obj) and elf_sections.file_sha256(obj) == old['object_sha256']:
            return dict(old, reused=True)
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        gate()
        t0 = time.time()
        try:
            flags = 0x00004000 if WINDOWS and args.low_priority else 0  # BELOW_NORMAL_PRIORITY_CLASS
            run = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=flags)
        finally:
            with lock:
                running[0] -= 1
        seconds = round(time.time() - t0, 1)
        Path(obj + '.log').write_bytes(run.stdout)
        if run.returncode:
            tail = run.stdout.decode(errors='replace').strip().splitlines()[-8:]
            return dict(name=u['name'], ok=False, seconds=seconds, error='\n'.join(tail))
        identity, left = elf_sections.code_identity(obj)
        record = dict(name=u['name'], ok=True, key=key, seconds=seconds, object_sha256=elf_sections.file_sha256(obj),
                      code_identity=identity, left_out=left, same_bytes=None)
        record['same_bytes'] = record['object_sha256'] == u['object_sha256']
        with lock:
            state[u['name']] = record
            state_path.write_text(json.dumps(state), encoding='utf-8', newline='\n')
        return record

    done = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(2, args.jobs)) as pool:
        for n, record in enumerate(pool.map(build, units), 1):
            done.append(record)
            if not record.get('ok'):
                print(f'FAILED {record["name"]}:\n{record.get("error")}', flush=True)
            elif n % 25 == 0 or n == len(units):
                print(f'{n}/{len(units)} units, {time.time() - started:.0f} s, lowest available memory {low[0]:.1f} GB, '
                      f'running {running[0]}', flush=True)
    compile_seconds = round(time.time() - started, 1)
    failed = [r['name'] for r in done if not r.get('ok')]

    link = None
    if not failed and not args.no_link and not args.only:
        words = tr.command(manifest['link']['command'])
        library = tr.word(manifest['link']['library'])
        if args.variant:
            library = library[:-3] + args.variant + '.so'
            words[words.index('-o') + 1] = library
        words[words.index('-o'):words.index('-o')] = args.link_extra
        rsp = state_dir / f'link{args.variant}.rsp'
        response_file(rsp, words[1:])
        t0 = time.time()
        run = subprocess.run([words[0], '@' + str(rsp).replace('\\', '/')], cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        link = dict(ok=run.returncode == 0, seconds=round(time.time() - t0, 1), library=library,
                    output=run.stdout.decode(errors='replace')[-2000:])
        if link['ok']:
            identity, left = elf_sections.code_identity(library)
            link.update(library_sha256=elf_sections.file_sha256(library), code_identity=identity, left_out=left,
                        size=os.path.getsize(library), sections=elf_sections.sections(library),
                        same_bytes=elf_sections.file_sha256(library) == manifest['link']['library_sha256'])
    stop.set()
    summary = dict(schema='replay-results/1', host=sys.platform, jobs=args.jobs, reserve_gb=args.reserve_gb,
                   plain=args.plain, wrong_inputs=wrong, units=done, failed=failed, link=link,
                   compile_seconds=compile_seconds, total_seconds=round(time.time() - started, 1),
                   available_gb_at_start=round(start_available, 2), lowest_available_gb=round(low[0], 2),
                   most_at_once=most[0], per_job_gb=args.per_job_gb, spacing=args.spacing,
                   same_bytes=sum(1 for r in done if r.get('same_bytes')), units_total=len(done))
    results_path.write_text(json.dumps(summary, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(f'units {len(done)}  failed {len(failed)}  byte-identical to the first machine {summary["same_bytes"]}  '
          f'compile {compile_seconds} s  link {link and link.get("seconds")} s ok={link and link.get("ok")}  '
          f'lowest available memory {low[0]:.1f} GB  most at once {most[0]}', flush=True)
    return 1 if failed or (link and not link['ok']) else 0


if __name__ == '__main__':
    sys.exit(main())
