#!/usr/bin/env python3
"""loop_locals_pass.py IN.cpp OUT.cpp --sites-file PARAMS.json: the job-code loop-scoped register locals as a deterministic
chunk pass for tools/fp_codegen/recipe_build.py (generic sites adapter: `tool {in} {out} --sites-file <params>`, kind "rg";
the chunk scope is PARAMS["chunks"]).

PARAMS (private, under local-analysis/; schema "jc-loop-locals/1"): {"schema", "flag": the extern "C" std::atomic<unsigned>
the dispatch reads (defined by the runtime), "chunks": [N, ...], "loops": {GUEST_FUNCTION: [LOOP_LABEL, ...]}}. For every
function in "loops" that the input chunk defines (PPC_FUNC_IMPL(__imp__sub_X) {), loop_locals.transform() adds a static
noinline twin jcl_sub_X whose listed loops keep their guest registers in C++ locals (copied in on entry to the loop, written
back on every exit; loop_locals.rewrite_region), dispatched at the original's entry while the flag is non-zero; the original
body is unchanged. Refuses (exit 1) when a listed loop of a function the chunk defines is not eligible, or when the chunk
defines none of the listed functions (it is in scope, so that is drift): a recipe can never silently lose a region. Pure text -> text: the output depends only on the input text and PARAMS.
Game-specific data (function addresses, labels) lives only in PARAMS; this tool contains no game code."""
from __future__ import annotations
import argparse, json, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import loop_locals  # noqa: E402

SCHEMA = 'jc-loop-locals/1'


def apply(text: str, params: dict) -> tuple[str, list[str]]:
    if params.get('schema') != SCHEMA:
        raise SystemExit('not a jc-loop-locals parameter file')
    loops = {f: list(v) for f, v in params['loops'].items()}
    defined = {f for f in loops if f'PPC_FUNC_IMPL(__imp__sub_{f}) {{' in text}
    mine = {f: v for f, v in loops.items() if f in defined}
    out, report = loop_locals.transform(text, mine, params['flag'])
    bad = [r for r in report if not r.endswith(': rewritten')]
    if bad:
        raise SystemExit('refused: ' + '; '.join(bad))
    return out, report


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('src'); ap.add_argument('dst'); ap.add_argument('--sites-file', required=True)
    a = ap.parse_args()
    params = json.loads(Path(a.sites_file).read_text())
    text = Path(a.src).read_text()
    out, report = apply(text, params)
    if not report:  # the pass only runs on PARAMS["chunks"]: a chunk in scope without any listed function is drift
        raise SystemExit('none of the listed functions is defined in this chunk')
    Path(a.dst).write_text(out, encoding='utf-8', newline='\n')
    print('\n'.join(report))


if __name__ == '__main__':
    main()
