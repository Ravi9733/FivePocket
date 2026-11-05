#!/usr/bin/env python3
"""case_label_flush.py: give text-added labels XenonRecomp's flush-to-zero (CSR) mode reset (package ftz-fix, 2026-10-04).

  case_label_flush.py IN.cpp OUT.cpp RECORDS.json KEY     apply the records of RECORDS.json["chunks"][KEY]
  case_label_flush.py derive OUT.json --attempt vN --functions F1,F2,... | @FILE   [--note TEXT] [--form F]
        records for every site of the listed functions in the attempt's effective chunk text (flush_model.scan_text):
        the generator's handling at every label the text added without it (--form generator, the default), or the
        PGO-neutral variant (--form unconditional: straight-line `...FlushModeUnconditional();` inserts, no change where
        the text already has the unconditional line; the same mode at every instruction, one FPCR write per pass
        through the site, the function's control-flow graph and so its PGO profile hash unchanged)
  case_label_flush.py derive-text CHUNK.cpp KEY OUT.json [--functions ...] [--form F]
        the same for one chunk file (all functions unless --functions)

XenonRecomp resets its CSR state at every label it prints (recompiler.cpp: "anyone could jump to this label"), so the first
FP / VMX-float instruction after a label it printed gets `ctx.fpscr.{disable,enable}FlushMode();` (the conditional form:
correct from either incoming mode). The corpus repairs that insert labels by text (switch expansions, the stitcher's
copied translations) keep the generator's lines of the fall-through context instead: no line (the code assumes the
fall-through predecessor's mode) or an unconditional line. Per record of RECORDS.json["chunks"][KEY] (private, under
local-analysis/: guest addresses):
    {"function": F, "label": L, "insn": A, "mode": "enable"|"disable", "action": "insert"|"conditional",
     "line": "unconditional" (optional, insert only)}
in sub_F, A is the first instruction at or after the label loc_L whose class needs a mode (flush_model.required), with no
label, bl / bctrl or flush line between; the pass checks that the generator's model (flush_model.simulate over the text's
own labels and calls) predicts exactly this: the conditional `mode` line at A, where the text has none ("insert": the line
is added right after A's instruction comment, where the generator prints it) or the unconditional form ("conditional": that
line, directly after the comment, becomes the conditional form; with "line": "unconditional" the insert is that form,
the PGO-neutral variant). After the edits the model must report no remaining
difference at the recorded sites and the same differences elsewhere. Every other line is copied unchanged. Semantics:
each recorded instruction then runs in its class's mode whatever mode the jump to the label carries (the conditional form
writes the FPCR only on a real change), as in the generator's own output with the label. Exit 1 when a record does not
apply exactly. Contains no game code or game-derived data.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import textpass  # noqa: E402
import flush_model as fm  # noqa: E402

ACTIONS = ('insert', 'conditional')


def line_for(mode: str) -> str:
    return f'\tctx.fpscr.{mode}FlushMode();\n'


def site_rows(lines: list[str], i: int, j: int, name: str) -> tuple[fm.Body, dict[int, dict]]:
    b = fm.Body(lines, i, j, int(name, 16))
    if not b.exact:
        raise SystemExit(f'sub_{name}: instruction addresses are not exact ({b.notes[:2]})')
    return b, {r['addr']: r for r in fm.simulate(b)}


def record_of(r: dict, name: str) -> dict | None:
    """the record for one simulate() row, or None when the row is not a label-reset site"""
    if r['expected'] != 'cond' or r['state'] != 'U' or r['label'] is None or r['actual'] not in (None, 'uncond'):
        return None
    return {'function': name, 'label': f'{r["label"]:08X}', 'insn': f'{r["addr"]:08X}', 'mode': fm.NAME[r['need']],
            'action': 'insert' if r['actual'] is None else 'conditional'}


def apply(text: str, records: list[dict]) -> tuple[str, dict]:
    lines = text.splitlines(keepends=True)
    edits: dict[int, tuple[str, str]] = {}           # line index -> ('after', line) | ('replace', line)
    by_fn: dict[str, list[dict]] = {}
    for rec in records:
        by_fn.setdefault(f'{int(rec["function"], 16):08X}', []).append(rec)
    stats = {'inserted': 0, 'made_conditional': 0, 'functions': len(by_fn)}
    before_rows: dict[str, dict] = {}
    for name, recs in by_fn.items():
        i, j = textpass.find_function(lines, name)
        b, rows = site_rows(lines, i, j, name)
        before_rows[name] = rows
        seen = set()
        for rec in recs:
            a, lab = int(rec['insn'], 16), int(rec['label'], 16)
            form = rec.get('line', 'conditional')
            if rec['action'] not in ACTIONS or rec['mode'] not in ('enable', 'disable') or a in seen or \
                    form not in ('conditional', 'unconditional') or (form == 'unconditional' and rec['action'] != 'insert'):
                raise SystemExit(f'sub_{name}: bad record {rec}')
            seen.add(a)
            r = rows.get(a)
            want = record_of(r, name) if r else None
            core = {k: v for k, v in rec.items() if k != 'line'}
            if want is None or want != {**core, 'function': name, 'label': f'{lab:08X}', 'insn': f'{a:08X}'}:
                raise SystemExit(f'sub_{name}: record {rec} does not match the generator model here ({r})')
            if a not in b.pos or lab not in b.pos or not b.label[lab]:
                raise SystemExit(f'sub_{name}: no label loc_{lab:X} / instruction {a:08X}')
            k = b.pos[a]
            if rec['action'] == 'insert':
                if b.flushes(a):
                    raise SystemExit(f'sub_{name}: {a:08X} already has a flush line')
                if form == 'unconditional':   # the PGO-neutral form: straight-line, same absolute mode
                    edits[k] = ('after', f'\tctx.fpscr.{rec["mode"]}FlushModeUnconditional();\n')
                    stats['inserted_unconditional'] = stats.get('inserted_unconditional', 0) + 1
                else:
                    edits[k] = ('after', line_for(rec['mode']))
                    stats['inserted'] += 1
            else:
                unc = f'\tctx.fpscr.{rec["mode"]}FlushModeUnconditional();\n'
                if lines[k + 1] != unc:
                    raise SystemExit(f'sub_{name}: {a:08X}: the unconditional line does not follow the comment')
                edits[k + 1] = ('replace', line_for(rec['mode']))
                stats['made_conditional'] += 1
    out = []
    for k, line in enumerate(lines):
        e = edits.get(k)
        if e and e[0] == 'replace':
            out.append(e[1])
            continue
        out.append(line)
        if e:
            out.append(e[1])
    new = ''.join(out)
    # re-check: the recorded sites now agree with the model, every other difference is unchanged
    nl = new.splitlines(keepends=True)
    for name, recs in by_fn.items():
        i, j = textpass.find_function(nl, name)
        _, rows = site_rows(nl, i, j, name)
        done = {int(r['insn'], 16) for r in recs}
        unc = {int(r['insn'], 16) for r in recs if r.get('line') == 'unconditional'}
        rest = {a: {k: v for k, v in r.items() if k != 'line'} for a, r in before_rows[name].items() if a not in done}
        now = {a: {k: v for k, v in r.items() if k != 'line'} for a, r in rows.items()}
        for a in unc:   # an unconditional line where the generator prints the conditional one: the benign difference
            r = now.pop(a, None)
            if not r or r['expected'] != 'cond' or r['actual'] != 'uncond' or r['state'] != 'U':
                raise SystemExit(f'sub_{name}: {a:08X}: unexpected model state after the unconditional insert ({r})')
        if now != rest:
            raise SystemExit(f'sub_{name}: the model differs after the edits at {sorted(set(now) ^ set(rest))[:4]}')
    return new, stats


def derive_text(text: str, functions: set[str] | None, form: str = 'generator') -> tuple[list[dict], dict]:
    """records for every label-reset site of the functions; form 'unconditional' = the PGO-neutral variant: inserts in
    the unconditional form, and no record where the text already has the unconditional line (already exact)"""
    if form not in ('generator', 'unconditional'):
        raise SystemExit(f'unknown form {form}')
    lines = text.splitlines(keepends=True)
    recs, other = [], {}
    from g4text import segments
    for name, (i, j) in segments(lines).items():
        if functions is not None and name not in functions:
            continue
        b, rows = site_rows(lines, i, j, name)
        for a in sorted(rows):
            rec = record_of(rows[a], name)
            if rec and form == 'unconditional':
                if rec['action'] == 'conditional':
                    continue
                rec['line'] = 'unconditional'
            if rec:
                recs.append(rec)
            else:
                k = fm.classify(rows[a])
                other[k] = other.get(k, 0) + 1
    return recs, other


def parse_list(spec: str) -> list[str]:
    text = Path(spec[1:]).read_text() if spec.startswith('@') else spec
    return [f'{int(x, 16):08X}' for x in re.split(r'[\s,]+', text) if x]


def cmd_derive(a) -> int:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    import recipe_build
    root = recipe_build.find_data_root(None)
    eff = recipe_build.effective(root, a.attempt)
    want = set(parse_list(a.functions))
    found, chunks, other = set(), {}, {}
    for n, p in sorted(eff.items()):
        text = textpass.read_text(p)
        names = {m.group(1) for m in re.finditer(r'^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\) \{', text, re.M)} & want
        if not names:
            continue
        recs, o = derive_text(text, names, a.form)
        found |= names
        for k, v in o.items():
            other[k] = other.get(k, 0) + v
        if recs:
            chunks[str(n)] = recs
    if found != want:
        raise SystemExit(f'functions not found: {sorted(want - found)}')
    return write_records(a.out, chunks, other, f'attempt {a.attempt}, functions {",".join(sorted(want))}', a.note)


def cmd_derive_text(a) -> int:
    recs, other = derive_text(textpass.read_text(Path(a.chunk)), set(parse_list(a.functions)) if a.functions else None,
                              a.form)
    return write_records(a.out, {a.key: recs} if recs else {}, other, f'{Path(a.chunk).name}', a.note)


def write_records(out: str, chunks: dict, other: dict, source: str, note: str | None) -> int:
    if Path(out).exists():
        raise SystemExit(f'{out} exists')
    n = sum(len(v) for v in chunks.values())
    Path(out).write_text(json.dumps({'schema': 'gta5-case-label-flush/1', 'note': 'private: guest addresses (ftz-fix '
                                     'case_label_flush.py derive)' + (f'; {note}' if note else ''), 'source': source,
                                     'other_model_differences': other, 'chunks': chunks}, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(f'{n} records in {len(chunks)} chunks; other model differences in these functions: {other}')
    return 0


def main() -> int:
    if len(sys.argv) == 5 and sys.argv[1] not in ('derive', 'derive-text'):
        src, dst, params, key = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]
        records = json.loads(params.read_text())['chunks'][key]
        text, stats = apply(textpass.read_text(src), records)
        textpass.write_new(dst, text)
        print(f'{src.name}: {stats}')
        return 0
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    d = sub.add_parser('derive')
    d.add_argument('out'); d.add_argument('--attempt', required=True); d.add_argument('--functions', required=True)
    d.add_argument('--note'); d.add_argument('--form', default='generator', choices=['generator', 'unconditional'])
    t = sub.add_parser('derive-text')
    t.add_argument('chunk'); t.add_argument('key'); t.add_argument('out'); t.add_argument('--functions')
    t.add_argument('--note'); t.add_argument('--form', default='generator', choices=['generator', 'unconditional'])
    a = ap.parse_args()
    return {'derive': cmd_derive, 'derive-text': cmd_derive_text}[a.cmd](a)


if __name__ == '__main__':
    raise SystemExit(main())
