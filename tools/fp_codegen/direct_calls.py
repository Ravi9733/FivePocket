#!/usr/bin/env python3
"""direct_calls.py rewrite IN.cpp OUT.cpp HOOKED.txt | check OUT.cpp HOOKED.txt | hooked LIBRARY_OBJ_DIR > HOOKED.txt

Stage 3 (leaf inlining, 2026-10-02): every generated guest call `sub_X(ctx, base)` goes through the weak, noinline wrapper
`PPC_WEAK_FUNC(sub_X) { __imp__sub_X(ctx, base); }`, which exists so that a hook can replace sub_X with a strong definition.
That wrapper keeps the compiler from inlining any guest callee, even inside one chunk. For a callee no hook replaces, the
weak wrapper is the definition the link keeps, and it only forwards, so calling `__imp__sub_X(ctx, base)` directly is the same
call; the profile-guided inliner may then inline hot small callees of the same chunk.

  hooked DIR    the guest functions with a STRONG C++ definition (`_Z12sub_XXXXXXXXR10PPCContextPh`, nm type T) in any
                object of DIR (a built attempt's build/obj): the hooks. One hex address per line.
  rewrite       replaces each call `sub_X(ctx, base)` whose X is not in HOOKED.txt by `__imp__sub_X(ctx, base)` and declares
                every such __imp__ target after the includes (PPC_FUNC_IMPL(__imp__sub_X);). Definitions, wrappers and every
                other use of a name are left as they are. Prints the number of rewritten calls.
  check         exit 1 if OUT.cpp calls __imp__sub_X directly for any X in HOOKED.txt outside the weak wrappers (a direct call
                would bypass that hook), or if a rewritten target is undeclared. DIRECT_CALLS_MUTANT=hook-one (tests only):
                rewrite also calls the first hooked callee it meets, which check must refuse.
Game-derived sources stay under local-analysis/; this tool contains no game code."""
from __future__ import annotations
import os, re, subprocess, sys
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'own_disc'))
import host_support

CALL = re.compile(r'(?<![A-Za-z0-9_])sub_([0-9A-F]{8})\(ctx, ?base\)')
DIRECT = re.compile(r'(?<![A-Za-z0-9_])__imp__sub_([0-9A-F]{8})\(ctx, ?base\)')
WRAPPER = re.compile(r'^PPC_WEAK_FUNC\(sub_([0-9A-F]{8})\)')
NM = os.environ.get('LLVM_NM', 'llvm-nm.exe' if sys.platform == 'win32' else 'llvm-nm')


def hooked_from(objdir: Path) -> list[str]:
    found = set()
    for o in sorted(objdir.glob('*.o')):
        out = subprocess.run([NM, '--defined-only', str(o)], capture_output=True, text=True, encoding='utf-8', check=True).stdout
        for line in out.splitlines():
            p = line.split()
            if len(p) == 3 and p[1] == 'T':
                m = re.fullmatch(r'_Z12sub_([0-9A-F]{8})R10PPCContextPh', p[2])
                if m: found.add(m.group(1))
    return sorted(found)


def rewrite(src: str, hooked: set[str]) -> tuple[str, int]:
    mutant = os.environ.get('DIRECT_CALLS_MUTANT', '')
    lines = src.split('\n'); targets = set(); n = 0; mutated = False
    for i, line in enumerate(lines):
        if WRAPPER.match(line): continue
        def sub(m):
            nonlocal n, mutated
            x = m.group(1)
            if x in hooked:
                if mutant == 'hook-one' and not mutated: mutated = True
                else: return m.group(0)
            targets.add(x); n += 1
            return f'__imp__sub_{x}(ctx, base)'
        if 'sub_' in line and '(ctx' in line: lines[i] = CALL.sub(sub, line)
    # declarations after the last leading #include / extern line block
    at = 0
    for i, line in enumerate(lines[:60]):
        if line.startswith('#include') or line.startswith('extern '): at = i + 1
    decl = [f'PPC_FUNC_IMPL(__imp__sub_{x});' for x in sorted(targets)]
    lines[at:at] = ['// direct_calls.py: unhooked guest callees called without their weak wrapper'] + decl
    return '\n'.join(lines), n


def check(src: str, hooked: set[str]) -> list[str]:
    errors = []; declared = set(re.findall(r'^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)', src, re.M))
    in_wrapper = False
    for no, line in enumerate(src.split('\n'), 1):
        if WRAPPER.match(line): in_wrapper = True; continue
        if in_wrapper:
            if line.startswith('}'): in_wrapper = False
            continue
        for x in DIRECT.findall(line):
            if x in hooked: errors.append(f'line {no}: direct call of hooked {x}')
            if x not in declared: errors.append(f'line {no}: __imp__sub_{x} not declared')
    return errors


def main() -> int:
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'hooked' and len(sys.argv) == 3:
        print('\n'.join(hooked_from(Path(sys.argv[2])))); return 0
    if cmd == 'rewrite' and len(sys.argv) == 5:
        hooked = set(Path(sys.argv[4]).read_text(encoding='utf-8').split())
        out, n = rewrite(Path(sys.argv[2]).read_text(encoding='utf-8'), hooked)
        Path(sys.argv[3]).write_text(out, encoding='utf-8', newline='\n'); print(f'{sys.argv[2]}: {n} calls rewritten'); return 0
    if cmd == 'check' and len(sys.argv) == 4:
        errors = check(Path(sys.argv[2]).read_text(encoding='utf-8'), set(Path(sys.argv[3]).read_text(encoding='utf-8').split()))
        for e in errors[:20]: print(e)
        print(f'{sys.argv[2]}: {"FAIL" if errors else "ok"} ({len(errors)} errors)'); return 1 if errors else 0
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main())
