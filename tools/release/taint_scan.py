#!/usr/bin/env python3
"""Check that nothing we publish shares a byte run with the game.

    taint_scan.py --published PATH [PATH ...] (--retail FILE [FILE ...] | --retail-list LIST)
                  [--generated-list LIST] [--max-generated-run 128]
                  [--window 32] [--min-distinct 8] [--allow tools/release/taint-allow.txt] [--report FILE]

--published: the files about to go public (folders are walked; .git is skipped). Run it on the repository checkout,
on the unpacked source archive and on every unpacked download.
--retail: files that come from the game on your own machine, for example the two disc images, the decoded game image
and the shader pack the builder made from them, or the folders of code and shaders it generated (folders are walked;
files under 192 bytes are skipped) (--retail-list: a text file with one such path per line, which keeps
your own folder names out of shell history). They are only read. Nothing from them is printed: a finding names the
published file, the offset, the length and the SHA-256 of the shared bytes.

--generated-list: files of C++ that the translator generated from the game (ppc_recomp.*.cpp, ppc_func_mapping.cpp,
ppc_config.h). The translator writes every instruction with the same small template and names the console's system
calls the same way each time, so short shared runs with our own translator patches, tests and tools are expected.
A run longer than MAX-GENERATED-RUN bytes (more than a statement or two) is a finding; shorter ones are listed as
NOTE lines. Do not list the translator's own header (ppc_context.h): it is upstream MIT code, not the game's.

Every WINDOW-byte run that appears in both a published file and a --retail file is a finding, unless it is a
counting run (8/16/32-bit numbers stepping by a fixed amount, as in font and file tables; listed as a NOTE), or the window has fewer than MIN-DISTINCT different
byte values (padding, rows of spaces) or the exact run is listed with a reason in the allow file. An allow line may name
where the run is allowed: 'DIGEST path=SUFFIX REASON' allows it only in a published file whose path ends with SUFFIX, and
'DIGEST path=SUFFIX,name REASON' only when the run is also one upper-case name ([A-Z0-9_]: a game function's name quoted as a
fact, publish decision 20); the same run in any other file, or a run that is more than the name, is still a finding.
'file=SHA256 path=SUFFIX REASON' allows every run in one third-party file: the published file at that path whose whole
content has that SHA-256 (proven byte-identical to a member of a pinned upstream archive, named in the reason); one
changed byte, or the same bytes at another path, and its runs are findings again.

Control: before the scan, 96 bytes from the middle of every retail and generated file (where such a window is varied
enough) are planted in a temporary file on the published side. Every plant must be found; a missed plant means the
scan did not read what it was given, and it fails.

Exit 0: PASS. 1: FAIL (a finding or a missed control). 2: the scan could not run.
The matching itself is done by windows.c, compiled here with $CC (default cc).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONTROL_BYTES = 96


def is_progression(run: bytes) -> bool:
    """True for counting runs: 8-, 16- or 32-bit numbers (either byte order) that step by a fixed amount, like the
    glyph-index and offset tables every font and many file formats hold. They clear MIN-DISTINCT but say nothing."""
    for width in (1, 2, 4):
        for order in ('big', 'little'):
            for start in range(width):
                body = run[start:]
                body = body[:len(body) - len(body) % width]
                values = [int.from_bytes(body[i:i + width], order) for i in range(0, len(body), width)]
                if len(values) >= 8 and len({b - a for a, b in zip(values, values[1:])}) == 1:
                    return True
    return False


def walk(paths):
    files = []
    for p in map(Path, paths):
        if p.is_dir():
            for root, dirs, names in os.walk(p):
                dirs[:] = sorted(d for d in dirs if d != '.git')
                files.extend(Path(root) / n for n in sorted(names) if not (Path(root) / n).is_symlink())
        elif p.is_file():
            files.append(p)
        else:
            raise SystemExit(f'taint_scan: no such file or folder: {p}')
    return files


def load_allow(path):
    """digest -> list of (path suffix or None, name only, reason)"""
    allowed = {}
    if path and Path(path).exists():
        for line in Path(path).read_text().splitlines():
            line = line.strip()
            if line and not line.startswith('#'):
                digest, _, rest = line.partition(' ')
                if digest.startswith('file='):         # a whole third-party file: 'file:<sha256>' -> [(path suffix, reason)]
                    where, _, reason = rest.partition(' ')
                    allowed.setdefault('file:' + digest[5:].lower(), []).append(
                        (where[len('path='):] if where.startswith('path=') else None, False, reason.strip()))
                    continue
                suffix, name_only = None, False
                if rest.startswith('path='):
                    where, _, rest = rest.partition(' ')
                    suffix, _, flag = where[len('path='):].partition(',')
                    name_only = flag == 'name'
                allowed.setdefault(digest.lower(), []).append((suffix, name_only, rest.strip()))
    return allowed


def is_allowed(rules, published_path: str, run: bytes) -> bool:
    import re
    for suffix, name_only, _reason in rules or ():
        if suffix is not None and not published_path.replace(os.sep, '/').endswith('/' + suffix.lstrip('/')):
            continue
        if name_only and not re.fullmatch(rb'[A-Z0-9_]+', run):
            continue
        return True
    return False


def file_allowed(allowed, published_path: str, file_sha256: str) -> bool:
    """True when the whole published file (by SHA-256, at that path) is listed as an unchanged upstream file."""
    return any(suffix and published_path.replace(os.sep, '/').endswith('/' + suffix.lstrip('/'))
               for suffix, _n, _r in allowed.get('file:' + file_sha256, ()))


def control_plant(retail: Path, min_distinct: int, published_blob: bytes = b'', taken=None) -> bytes:
    """96 bytes from the middle of the file, moved forward until the first window is varied enough to be indexed;
    None for a file with no such window (it is still scanned, but cannot prove it was read). `taken` holds the windows
    of the plants already chosen: generated files repeat themselves, and a plant equal to an earlier one would have no
    window of its own in the index."""
    size = retail.stat().st_size
    if size < CONTROL_BYTES * 2:
        raise SystemExit(f'taint_scan: retail file too small for a control: {retail.name}')
    taken = set() if taken is None else taken
    with retail.open('rb') as f:
        for start in list(range(size // 2, size - CONTROL_BYTES, 4099)) + list(range(0, size // 2, 4099)):
            f.seek(start)
            chunk = f.read(CONTROL_BYTES)
            # A plant made of text the published files also hold would be credited to them, not to the control.
            windows = {chunk[i:i + 32] for i in range(CONTROL_BYTES - 31)}
            if len(set(chunk[:32])) >= max(min_distinct, 16) and not windows & taken and not any(
                    chunk[i:i + 32] in published_blob for i in (0, 32, 64)):
                taken |= windows
                return chunk
    return None


def build_helper(work: Path) -> Path:
    exe = work / 'windows'
    cc = os.environ.get('CC', 'cc')
    subprocess.run([cc, '-O2', '-o', str(exe), str(HERE / 'windows.c')], check=True)
    return exe


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--published', nargs='+', required=True)
    ap.add_argument('--retail', nargs='+', default=[])
    ap.add_argument('--retail-list')
    ap.add_argument('--generated-list')
    ap.add_argument('--max-generated-run', type=int, default=128)
    ap.add_argument('--window', type=int, default=32)
    ap.add_argument('--min-distinct', type=int, default=8)
    ap.add_argument('--allow', default=str(HERE / 'taint-allow.txt'))
    ap.add_argument('--report')
    args = ap.parse_args(argv)

    published = walk(args.published)
    named = list(args.retail)
    if args.retail_list:
        named += [l.strip() for l in Path(args.retail_list).read_text().splitlines() if l.strip() and not l.startswith('#')]
    missing = [Path(r).name for r in named if not Path(r).exists()]
    if missing:
        print(f'taint_scan: retail input missing: {", ".join(missing)}', file=sys.stderr)
        return 2
    retail = [r for r in walk(named) if r.stat().st_size >= CONTROL_BYTES * 2]
    generated_named = []
    if args.generated_list:
        generated_named = [l.strip() for l in Path(args.generated_list).read_text().splitlines()
                           if l.strip() and not l.startswith('#')]
        missing = [Path(r).name for r in generated_named if not Path(r).exists()]
        if missing:
            print(f'taint_scan: generated input missing: {", ".join(missing[:5])}', file=sys.stderr)
            return 2
    strict_count = len(retail)
    retail += [r for r in walk(generated_named) if r.stat().st_size >= CONTROL_BYTES * 2]
    if not retail:
        print('taint_scan: give --retail or --retail-list', file=sys.stderr)
        return 2
    allowed = load_allow(args.allow)
    lines = []

    with tempfile.TemporaryDirectory(prefix='taint-') as tmp:
        work = Path(tmp)
        exe = build_helper(work)
        controls, planted = [], []
        blob = b'\0'.join(p.read_bytes() for p in published)
        taken = set()
        for i, r in enumerate(retail):
            chunk = control_plant(r, args.min_distinct, blob, taken)
            if chunk is None:
                continue
            plant = work / f'control-{i}.bin'
            plant.write_bytes(chunk)
            controls.append(plant)
            planted.append(i)
        index = published + controls
        (work / 'index.txt').write_text(''.join(f'{p}\n' for p in index))
        (work / 'stream.txt').write_text(''.join(f'{p.resolve()}\n' for p in retail))
        out = subprocess.run([str(exe), str(args.window), str(args.min_distinct), str(work / 'index.txt'),
                              str(work / 'stream.txt'), str(len(published))], capture_output=True, text=True)
        if out.returncode != 0 or 'DONE' not in out.stdout:
            print(out.stderr, file=sys.stderr)
            return 2

        found_controls = set()
        findings, notes, file_sha = [], [], {}
        done = ''
        for row in out.stdout.splitlines():
            if row.startswith('DONE'):
                done = row
                continue
            _, sf, so, pf, po, length = row.split()
            sf, so, pf, po, length = map(int, (sf, so, pf, po, length))
            if pf >= len(published):
                found_controls.add(planted[pf - len(published)])
                continue
            with open(index[pf], 'rb') as f:
                f.seek(po)
                run_bytes = f.read(length)
            digest = hashlib.sha256(run_bytes).hexdigest()
            if is_allowed(allowed.get(digest), str(index[pf]), run_bytes):
                continue
            if pf not in file_sha:
                file_sha[pf] = hashlib.sha256(Path(index[pf]).read_bytes()).hexdigest()
            if file_allowed(allowed, str(index[pf]), file_sha[pf]):
                continue
            if is_progression(run_bytes):
                notes.append((str(index[pf]), po, length, retail[sf].name, 'counting run'))
                continue
            if sf >= strict_count and length <= args.max_generated_run:
                notes.append((str(index[pf]), po, length, retail[sf].name, digest))
                continue
            findings.append((str(index[pf]), po, length, retail[sf].name, digest))

    missed = [retail[i].name for i in planted if i not in found_controls]
    total = sum(p.stat().st_size for p in published)
    longest = max((f[2] for f in findings), default=0)
    lines.append(f'taint_scan: {len(published)} published files ({total} bytes) against {strict_count} retail and '
                 f'{len(retail) - strict_count} generated files; '
                 f'window {args.window}, min distinct {args.min_distinct}; {done}')
    for name in missed:
        lines.append(f'CONTROL MISSED {name}: this retail file was not read')
    for path, off, length, source, digest in findings:
        lines.append(f'FINDING {path} offset {off} length {length} shared with {source} sha256 {digest}')
    for path, off, length, source, digest in notes:
        why = ('a counting run (numbers stepping by a fixed amount)' if digest == 'counting run' else
               f'at most {args.max_generated_run} bytes of generated code: translator template or system-call name')
        lines.append(f'NOTE {path} offset {off} length {length} shared with {source}: {why}')
    if 'truncated=1' in done:
        lines.append('FAIL: too many findings; the list is cut short')
    verdict = 'PASS' if not findings and not missed and 'truncated=1' not in done else 'FAIL'
    lines.append(f'TAINT {verdict} findings={len(findings)} notes={len(notes)} longest_run={longest} controls={len(found_controls)}/{len(planted)} retail_files={len(retail)}')
    text = '\n'.join(lines) + '\n'
    sys.stdout.write(text)
    if args.report:
        Path(args.report).write_text(text)
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
