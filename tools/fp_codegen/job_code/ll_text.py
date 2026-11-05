"""ll_text.py: the generated-chunk text patterns loop_locals.py shares with tools/fp_codegen/leaf_locals.py (copied
verbatim from it, 2026-10-04, so the loop_locals pass's tool closure - recipe_build.py records the tool and its sibling imports
- is complete and self-contained): guest-register fields of ctx, any other use of ctx, and the generated-body scanner.
This module contains no game code."""
from __future__ import annotations
import re

START = re.compile(r'^(PPC_FUNC_IMPL|PPC_FUNC)\((.*)\)\s*\{\s*$')
REG = re.compile(r'\bctx\.(r[0-9]{1,2}|f[0-9]{1,2}|v[0-9]{1,3}|cr[0-7]|ctr|xer)\b')
OTHER_CTX = re.compile(r'\bctx\b(?!\.(?:r[0-9]{1,2}|f[0-9]{1,2}|v[0-9]{1,3}|cr[0-7]|ctr|xer|fpscr|lr|msr|reserved)\b)')


def functions(lines: list[str]):
    """(start index, end index of the column-0 closing brace, guest address or '') of every generated body."""
    i = 0
    while i < len(lines):
        m = START.match(lines[i])
        if not m:
            i += 1; continue
        j = i + 1
        while j < len(lines) and not lines[j].startswith('}'): j += 1
        fn = re.search(r'sub_([0-9A-F]{8})', m.group(2))
        yield i, j, fn.group(1) if fn else ''
        i = j + 1
