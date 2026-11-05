"""Shared helpers of the G4 corpus-repair passes (package repro-g4, 2026-10-04). No game code or game-derived data here.

positions() is textpass.instruction_lines that also tolerates the one-tab comment lines the corpus repairs insert
(`\\t// Original ...`, `\\t// Recover ...`, ...): they are not instruction comments, so a pass that runs after another
repair pass still addresses instructions exactly (the label and LR consistency checks stay in force). Index finds the
generated function whose instruction comments contain a guest address (for passes that copy the generator's own
translation of original instructions).
"""
from __future__ import annotations

import re

import textpass

REPAIR = re.compile(r'\t// (Original |Recover|Verified |Native |Jump-table repair|Recovered |unknown/mutated |as the tail )')


def positions(lines: list[str], i: int, j: int, start: int) -> dict[int, int]:
    if any(REPAIR.match(lines[k]) for k in range(i + 1, j)):
        lines = lines[:i + 1] + [('\t/* repair comment */\n' if REPAIR.match(x) else x) for x in lines[i + 1:j]] + lines[j:]
    return textpass.instruction_lines(lines, i, j, start)


class Index:
    """the generated functions (PPC_FUNC_IMPL(__imp__sub_X)) of one chunk text, with lazily computed positions"""

    def __init__(self, lines: list[str]):
        self.lines = lines
        self.fn: dict[str, tuple[int, int]] = {}
        for i, j, name in textpass.functions(lines):
            if lines[i].startswith('PPC_FUNC_IMPL(') and re.fullmatch(r'[0-9A-F]{8}', name):
                if name in self.fn:
                    raise SystemExit(f'duplicate function {name}')
                self.fn[name] = (i, j)
        self._sorted = sorted(self.fn, key=lambda n: int(n, 16))
        self._pos: dict[str, dict[int, int]] = {}

    def positions(self, name: str) -> dict[int, int]:
        if name not in self._pos:
            i, j = self.fn[name]
            self._pos[name] = positions(self.lines, i, j, int(name, 16))
        return self._pos[name]

    def find(self, x: int) -> tuple[str, dict[int, int]]:
        """the function whose instruction comments contain x (searched among the 8 nearest preceding entries)"""
        cands = [n for n in self._sorted if int(n, 16) <= x][-8:]
        for name in reversed(cands):
            pos = self.positions(name)
            if x in pos:
                return name, pos
        raise SystemExit(f'no generated function of this chunk translates {x:08X}')


BCTR = '\t// bctr \n'


def segments(lines: list[str]) -> dict[str, tuple[int, int]]:
    out = {}
    for i, j, name in textpass.functions(lines):
        if lines[i].startswith('PPC_FUNC_IMPL(') and re.fullmatch(r'[0-9A-F]{8}', name):
            if name in out:
                raise SystemExit(f'duplicate function {name}')
            out[name] = (i, j)
    return out


def bctr_lines(lines: list[str], i: int, j: int) -> list[int]:
    return [k for k in range(i + 1, j) if lines[k] == BCTR]
