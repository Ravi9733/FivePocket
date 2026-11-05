"""Shared helpers for the recipe passes (package repro-recipe, 2026-10-04). No game code or game-derived data here.

Every recipe pass is a pure function text -> text over one generated chunk (XenonRecomp output plus earlier passes), with
its parameters read from a private file under the ignored local-analysis/. The command-line wrappers follow the house
rules of the other fp_codegen passes: IN and OUT paths, OUT is never overwritten, the text is decoded strictly as UTF-8
and a carriage return is refused (newline translation would hide it).
"""
from __future__ import annotations

import re
from pathlib import Path

START = re.compile(r'^(PPC_FUNC_IMPL|PPC_FUNC)\((.*)\)\s*\{\s*$')
LABEL = re.compile(r'loc_([0-9A-F]+):\n')
LR = re.compile(r'\tctx\.lr = (0x[0-9A-F]+);\n')


def read_text(path: Path) -> str:
    data = Path(path).read_bytes()
    if b'\r' in data:
        raise SystemExit(f'{path}: carriage return in a generated chunk')
    return data.decode('utf-8', errors='strict')


def write_new(path: Path, text: str) -> None:
    path = Path(path)
    if path.exists():
        raise SystemExit(f'{path} exists')
    path.write_text(text, encoding='utf-8', newline='\n')


def functions(lines: list[str]):
    """(header index, index of the column-0 closing brace, name) of every generated body; name = the guest address X of
    PPC_FUNC_IMPL(__imp__sub_X) / PPC_FUNC(sub_X), else the full name inside the parentheses (e.g. __imp____savegprlr_14)."""
    i = 0
    while i < len(lines):
        m = START.match(lines[i])
        if not m:
            i += 1
            continue
        j = i + 1
        while j < len(lines) and not lines[j].startswith('}'):
            j += 1
        fn = re.search(r'sub_([0-9A-F]{8})', m.group(2))
        yield i, j, (fn.group(1) if fn else m.group(2).strip())
        i = j + 1


def find_function(lines: list[str], name: str) -> tuple[int, int]:
    hits = [(i, j) for i, j, n in functions(lines) if n == name and lines[i].startswith('PPC_FUNC_IMPL(')]
    if len(hits) != 1:
        raise SystemExit(f'function {name}: {len(hits)} PPC_FUNC_IMPL definitions (want exactly 1)')
    return hits[0]


UNCONDITIONAL = re.compile(r'\t// (bctr|blr|b) ')


def instruction_lines(lines: list[str], i: int, j: int, start: int) -> dict[int, int]:
    """guest address -> line index of its generator instruction comment inside the body lines[i+1:j], counting the
    generator's one `\\t// <instruction>` comment per instruction from the function entry `start` (a `\\t// ERROR ` marker is
    not an instruction). Consistency checks: every generated `loc_X:` label must sit at X and every `ctx.lr = X;` must equal
    the address after its bl. The one allowed gap: inline data the generator skipped (a jump table it dispatches through
    its switch metadata) can only follow an unconditional bctr / blr / b, and the next label then sets the position
    forward. A body whose comments were edited fails here instead of being mis-addressed."""
    pc, pos, last = start, {}, ''
    for k in range(i + 1, j):
        line = lines[k]
        m = LABEL.fullmatch(line)
        if m and int(m.group(1), 16) != pc:
            x = int(m.group(1), 16)
            if not (x > pc and UNCONDITIONAL.match(last)):
                raise SystemExit(f'{start:08X}: label {m.group(1)} at instruction position {pc:08X}')
            pc = x  # skipped inline data after an unconditional transfer
        if line.startswith('\t// ') and not line.startswith('\t// ERROR '):
            pos[pc] = k
            pc += 4
            last = line
        m = LR.fullmatch(line)
        if m and int(m.group(1), 16) != pc:
            raise SystemExit(f'{start:08X}: ctx.lr {m.group(1)} at instruction position {pc:08X}')
    return pos
