#!/usr/bin/env python3
"""switch_recover.py: recover explicit switch-table metadata from switch dispatches present in generated code.

  switch_recover.py scan GEN_DIR CODE_DIR OUT.json [--chunks N,M,...]
        For every chunk present in both trees, pairs each generated function of GEN_DIR (XenonRecomp output) with the
        same function in CODE_DIR (a corpus that carries later repairs) and, per `\\t// bctr ` dispatch (paired in order),
        records the switch that CODE_DIR emits there: selector register, case targets in case order, the emitted form
        and what GEN_DIR emits at the same bctr (an indirect call = no table, or a switch with its own targets). The
        bctr's guest address comes from GEN_DIR's instruction comments (textpass.instruction_lines, label/LR checked);
        the table's default target from the bounds guard the generator translated before the dispatch (`// bgt crN,X`
        within the 12 preceding instructions), when present.
  switch_recover.py toml BASE.toml SCAN.json OUT.toml [--forms live,compact,compact-default]
        Writes BASE.toml unchanged (byte for byte) followed by one [[switch]] entry in the generator's schema (base, r,
        default, labels; parse_switch_metadata-compatible) per recovered dispatch that GEN_DIR left without a table.
        base = the bctr address (the generator binds a table at the first instruction address >= the function entry it
        reaches while no table is pending, so the bctr itself is always a valid binding point). A dispatch without a
        recoverable default is refused (exit 1) instead of inventing one.

Recognised switch forms (exactly; anything else is reported as "other" with its line count, never parsed loosely):
  live            patch 0014's lowering: case i: if (ctx.ctr.u32 != 0xXU) { PPC_CALL_INDIRECT_FUNC; return; } goto loc_X;
                  (a case outside the function: `// ERROR: 0xX` + `return;`) ... default: PPC_CALL_INDIRECT_FUNC; return;
  compact         case i: if (ctx.ctr.u32 == 0xXU) goto loc_X; break; ... } PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32); return;
  compact-default the same with `\\tdefault: break;` before the closing brace.
The tool reads generated code and writes addresses into private files under local-analysis/; it contains no game data.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import textpass  # noqa: E402
from g4text import bctr_lines, segments  # noqa: E402,F401

CHUNK = re.compile(r'ppc_recomp\.(\d+)\.cpp')
BCTR = '\t// bctr \n'
INDIRECT = ['\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n', '\treturn;\n']
SWITCH = re.compile(r'\tswitch \(ctx\.r(\d+)\.u32\) \{\n')
CASE = re.compile(r'\tcase (\d+):\n')
LIVE_IF = re.compile(r'\t\tif \(ctx\.ctr\.u32 != 0x([0-9A-F]+)U\) \{\n')
LIVE_GOTO = re.compile(r'\t\tgoto loc_([0-9A-F]+);\n')
LIVE_ERR = re.compile(r'\t\t// ERROR: 0x([0-9A-F]+)\n')
COMPACT = re.compile(r'\tcase (\d+): if \(ctx\.ctr\.u32 == 0x([0-9A-F]+)U\) goto loc_([0-9A-F]+); break;\n')
GUARD = re.compile(r'\t// bgt cr(\d),0x([0-9a-f]+)\n')
CALL_RET = '\t\t\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32); return;\n'


def parse_switch(lines: list[str], k: int) -> dict | None:
    """the switch emitted right after the bctr comment at lines[k], or None for the generator's indirect call"""
    if lines[k + 1:k + 3] == INDIRECT:
        return None
    m = SWITCH.fullmatch(lines[k + 1])
    if not m:
        return {'form': 'other', 'reason': 'no switch after bctr', 'end': k + 1}
    sel, i = int(m.group(1)), k + 2
    if COMPACT.fullmatch(lines[i]):
        labels = []
        while (c := COMPACT.fullmatch(lines[i])):
            if int(c.group(1)) != len(labels) or c.group(2) != c.group(3):
                return {'form': 'other', 'reason': 'compact case order/target mismatch', 'selector': sel, 'end': i}
            labels.append(int(c.group(2), 16))
            i += 1
        form = 'compact'
        if lines[i] == '\tdefault: break;\n':
            form, i = 'compact-default', i + 1
        if lines[i:i + 3] != ['\t}\n'] + INDIRECT:
            return {'form': 'other', 'reason': 'compact tail', 'selector': sel, 'end': i}
        return {'form': form, 'selector': sel, 'labels': labels, 'end': i + 3}
    labels, outside = [], []
    while (c := CASE.fullmatch(lines[i])):
        if int(c.group(1)) != len(labels):
            return {'form': 'other', 'reason': 'live case order', 'selector': sel, 'end': i}
        a, b = LIVE_IF.fullmatch(lines[i + 1]), LIVE_ERR.fullmatch(lines[i + 1])
        if a and lines[i + 2] == CALL_RET and lines[i + 3] == '\t\t}\n' and (g := LIVE_GOTO.fullmatch(lines[i + 4])) \
                and g.group(1) == a.group(1):
            labels.append(int(a.group(1), 16))
            i += 5
        elif b and lines[i + 2] == '\t\treturn;\n':
            labels.append(int(b.group(1), 16))
            outside.append(len(labels) - 1)
            i += 3
        else:
            return {'form': 'other', 'reason': 'live case body', 'selector': sel, 'end': i}
    if not labels or lines[i:i + 3] != ['\tdefault:\n', '\t\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32); return;\n', '\t}\n']:
        return {'form': 'other', 'reason': 'live tail', 'selector': sel, 'end': i}
    return {'form': 'live', 'selector': sel, 'labels': labels, 'outside_cases': outside, 'end': i + 3}


GUARD_WINDOW = 32


def default_target(lines: list[str], pos: dict[int, int], bctr: int) -> int | None:
    """the target of the nearest bounds guard (`bgt crN,X`) the generator translated within GUARD_WINDOW instructions
    before bctr (metadata only: XenonRecomp ignores `default`; verify-image checks it against the original idiom)"""
    for back in range(1, GUARD_WINDOW + 1):
        k = pos.get(bctr - 4 * back)
        if k is None:
            return None
        g = GUARD.fullmatch(lines[k])
        if g:
            return int(g.group(2), 16)
    return None


def scan_chunk(gen_text: str, code_text: str) -> list[dict]:
    g, c = gen_text.splitlines(keepends=True), code_text.splitlines(keepends=True)
    gs, cs = segments(g), segments(c)
    if list(gs) != list(cs):
        raise SystemExit('function lists differ')
    rows = []
    for name, (gi, gj) in gs.items():
        ci, cj = cs[name]
        if g[gi:gj] == c[ci:cj]:
            continue
        gb, cb = bctr_lines(g, gi, gj), bctr_lines(c, ci, cj)
        cand = [k for k in cb if not c[k + 1:k + 3] == INDIRECT]
        if not cand:
            continue
        start = int(name, 16)
        pos = textpass.instruction_lines(g, gi, gj, start)
        addr = {k: a for a, k in pos.items()}
        if len(gb) != len(cb):
            rows.append({'function': name, 'problem': f'bctr count differs: generator {len(gb)}, code {len(cb)}'})
            continue
        for kg, kc in zip(gb, cb):
            sw = parse_switch(c, kc)
            if sw is None:
                continue
            site = addr[kg]
            gen = parse_switch(g, kg)
            row = {'function': name, 'bctr': f'{site:08X}', 'form': sw['form']}
            if sw['form'] == 'other':
                row['reason'] = sw['reason']
                row['code_lines'] = sw['end'] - kc
            else:
                row['selector'] = sw['selector']
                row['labels'] = [f'{x:08X}' for x in sw['labels']]
                if sw.get('outside_cases'):
                    row['outside_cases'] = sw['outside_cases']
                # inline data after the dispatch: the generator translates the words after the bctr as instructions;
                # a repaired code text that resumes with a label further on dropped them
                nxt = c[sw['end']] if sw['end'] < len(c) else ''
                lab = textpass.LABEL.fullmatch(nxt)
                if lab and int(lab.group(1), 16) > site + 4 and (site + 4) in pos:
                    row['dropped'] = [f'{site + 4:08X}', lab.group(1)]
            d = default_target(g, pos, site)
            if d is not None:
                row['default'] = f'{d:08X}'
            if gen is None:
                row['generator'] = 'indirect'
            elif gen['form'] == 'live':
                row['generator'] = 'live'
                row['generator_selector'] = gen['selector']
                if gen['labels'] != sw.get('labels') or gen['selector'] != sw.get('selector'):
                    row['generator_labels'] = [f'{x:08X}' for x in gen['labels']]
            else:
                row['generator'] = gen['form']
            rows.append(row)
    return rows


def cmd_scan(a) -> int:
    gen, code = Path(a.gen), Path(a.code)
    chunks = sorted(int(CHUNK.fullmatch(p.name).group(1)) for p in code.glob('ppc_recomp.*.cpp') if CHUNK.fullmatch(p.name))
    if a.chunks:  # N,M,... or @FILE (whitespace/comma separated chunk numbers)
        spec = Path(a.chunks[1:]).read_text() if a.chunks.startswith('@') else a.chunks
        chunks = [int(x) for x in re.split(r'[\s,]+', spec) if x]
    out, summary = {}, {}
    for n in chunks:
        gp, cp = gen / f'ppc_recomp.{n}.cpp', code / f'ppc_recomp.{n}.cpp'
        gt, ct = textpass.read_text(gp), textpass.read_text(cp)
        if gt == ct:
            continue
        rows = scan_chunk(gt, ct)
        if rows:
            out[str(n)] = rows
        for r in rows:
            key = r.get('problem') and 'problem' or f'{r["form"]} (generator {r["generator"]})'
            summary[key] = summary.get(key, 0) + 1
    Path(a.out).write_text(json.dumps({'schema': 'gta5-switch-recover/1', 'generator_dir': str(gen), 'code_dir': str(code),
                                       'summary': summary, 'chunks': out}, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(summary))
    return 0


def entry(base: int, sel: int, default: int | None, labels: list[int], note: str = '') -> str:
    return (f'\n{note}[[switch]]\n' f'base = 0x{base:08X}\n' f'r = {sel}\n'
            + (f'default = 0x{default:08X}\n' if default is not None else '')
            + 'labels = [' + ', '.join(f'0x{x:08X}' for x in labels) + ']\n')


def table_rows(scan: dict, forms: set[str]) -> list[dict]:
    """the recovered dispatches the generator left without a table, as metadata records (base/r/default/labels)"""
    out = []
    for r in (r for rs in scan['chunks'].values() for r in rs if r.get('generator') == 'indirect' and r['form'] in forms):
        im = r.get('image', {})
        rec = {'bctr': r['bctr'], 'function': r['function'], 'r': r['selector'], 'labels': r['labels'],
               'base': im['base'] if im.get('match') else r['bctr'],
               'default': im['default'] if im.get('match') else r.get('default'),
               'evidence': 'code + image idiom' if im.get('match') else ('code + guard' if r.get('default') else 'code only')}
        out.append(rec)
    return sorted(out, key=lambda x: int(x['bctr'], 16))


def cmd_toml(a) -> int:
    base_text = Path(a.base).read_text()
    scan = json.loads(Path(a.scan).read_text())
    rows = table_rows(scan, set(a.forms.split(',')))
    nodef = [r['bctr'] for r in rows if r['default'] is None]
    if nodef and not a.allow_no_default:
        print(f'no recoverable default for {len(nodef)} dispatches {nodef}: refused (--allow-no-default omits the key; '
              'XenonRecomp reads only base, r and labels)')
        return 1
    if Path(a.out).exists():
        raise SystemExit(f'{a.out} exists')
    if not base_text.endswith('\n'):
        raise SystemExit('base switch file does not end with a newline')
    have = {int(m.group(1), 16) for m in re.finditer(r'^base = (0x[0-9A-Fa-f]+)$', base_text, re.M)}
    out = [base_text, f'\n# Recovered by switch_recover.py from switch dispatches in repaired generated code ({len(rows)} tables).\n']
    for r in rows:
        if int(r['base'], 16) in have:
            raise SystemExit(f'base {r["base"]} already in the base metadata')
        note = '' if r['evidence'] != 'code only' else '# unguarded dispatch: no default target in the original code\n'
        out.append(entry(int(r['base'], 16), r['r'], int(r['default'], 16) if r['default'] else None,
                         [int(x, 16) for x in r['labels']], note))
    Path(a.out).write_text(''.join(out), encoding='utf-8', newline='\n')
    ev: dict[str, int] = {}
    for r in rows:
        ev[r['evidence']] = ev.get(r['evidence'], 0) + 1
    print(f'{len(rows)} tables appended to {len(base_text)} bytes of base metadata; evidence {ev}')
    return 0


def cmd_records(a) -> int:
    """the switch_expand.py parameter file: per chunk, every recovered dispatch in its corpus form (expand = the generator
    left an indirect call, reformat = the generator's live switch with the same targets in another form)"""
    scan = json.loads(Path(a.scan).read_text())
    out, counts = {}, {}
    for n, rows in scan['chunks'].items():
        recs = []
        for r in rows:
            if 'labels' not in r:
                continue
            if r['generator'] == 'indirect':
                mode = 'expand'
            elif r['generator'] == 'live' and 'generator_labels' not in r and r['form'] != 'live':
                mode = 'reformat'
            else:
                continue  # same table and same form as the generator: nothing to do
            rec = {'function': r['function'], 'bctr': r['bctr'], 'r': r['selector'], 'labels': r['labels'],
                   'form': r['form'], 'mode': mode}
            if r.get('dropped'):
                rec['drop'] = r['dropped']
            recs.append(rec)
            key = f'{mode} {r["form"]}' + (' + drop inline data' if r.get('dropped') else '')
            counts[key] = counts.get(key, 0) + 1
        if recs:
            out[n] = recs
    if Path(a.out).exists():
        raise SystemExit(f'{a.out} exists')
    Path(a.out).write_text(json.dumps({'schema': 'gta5-switch-expand/1', 'note': 'private: guest addresses recovered '
                                       'from repaired generated code (switch_recover.py records)', 'chunks': out},
                                      indent=1) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(counts), f'{len(out)} chunks')
    return 0


FLUSH = re.compile(r'\tctx\.fpscr\.(enable|disable)FlushMode\(\);\n')


def cmd_regen_check(a) -> int:
    """compare a regeneration WITH the recovered tables (REGEN_DIR) against GEN_DIR (without them) + switch_expand's
    expand records minus the inline-data drop: the switch text and the case labels must be identical; every remaining
    difference is classified (a flush-mode line the generator emits because a new label resets its CSR state)"""
    import difflib
    import switch_expand
    recs = json.loads(Path(a.records).read_text())['chunks']
    regen, gen = Path(a.regen), Path(a.gen)
    rows, totals = {}, {'chunks': 0, 'tables': 0, 'identical_chunks': 0}
    for n, rs in sorted(recs.items(), key=lambda x: int(x[0])):
        exp = [{k: v for k, v in r.items() if k != 'drop'} for r in rs if r['mode'] == 'expand']
        if not exp:
            continue
        want = switch_expand.apply(textpass.read_text(gen / f'ppc_recomp.{n}.cpp'), exp)[0].splitlines(keepends=True)
        got = textpass.read_text(regen / f'ppc_recomp.{n}.cpp').splitlines(keepends=True)
        kinds: dict[str, int] = {}
        for op, i1, i2, j1, j2 in difflib.SequenceMatcher(None, want, got, autojunk=False).get_opcodes():
            if op == 'equal':
                continue
            for ln in want[i1:i2]:
                kinds['only without regeneration: ' + ('flush-mode line' if FLUSH.fullmatch(ln) else 'other')] = \
                    kinds.get('only without regeneration: ' + ('flush-mode line' if FLUSH.fullmatch(ln) else 'other'), 0) + 1
            for ln in got[j1:j2]:
                k = 'only in regeneration: ' + ('flush-mode line' if FLUSH.fullmatch(ln) else 'other')
                kinds[k] = kinds.get(k, 0) + 1
        rows[n] = {'tables': len(exp), 'differences': kinds}
        totals['chunks'] += 1
        totals['tables'] += len(exp)
        totals['identical_chunks'] += not kinds
        for k, v in kinds.items():
            totals[k] = totals.get(k, 0) + v
    if Path(a.out).exists():
        raise SystemExit(f'{a.out} exists')
    Path(a.out).write_text(json.dumps({'schema': 'gta5-switch-regen-check/1', 'regen_dir': str(regen), 'gen_dir': str(gen),
                                       'totals': totals, 'chunks': rows}, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(totals))
    return 0


FLUSH_ANY = re.compile(r'\tctx\.fpscr\.(enable|disable)FlushMode(Unconditional)?\(\);\n')


def cmd_csr_edges(a) -> int:
    """the flush-mode (CSR) state on the switch edges a text expansion adds. XenonRecomp resets its CSR state at every
    label and then emits a conditional `ctx.fpscr.{enable,disable}FlushMode();` before the next FP/VMX instruction; a
    text expansion inserts the case labels without that reset. For every conditional flush line that only the
    regeneration WITH the tables (REGEN_DIR) has, the expanded code assumes the needed mode M there; entering through
    the switch, the mode is the generator's linear state at the dispatching bctr (the last flush line since the last
    label, in GEN_DIR). Verdicts: same mode (safe), corpus sets the mode unconditionally here (safe), MODE MISMATCH
    (the case code runs in the other flush-to-zero mode when entered through the switch), unknown at bctr."""
    import difflib
    import switch_expand
    from g4text import positions
    recs = json.loads(Path(a.records).read_text())['chunks']
    regen, gen = Path(a.regen), Path(a.gen)
    counts, rows = {}, []
    for n, rs in sorted(recs.items(), key=lambda x: int(x[0])):
        exp = [{k: v for k, v in r.items() if k != 'drop'} for r in rs if r['mode'] == 'expand']
        if not exp:
            continue
        g = textpass.read_text(gen / f'ppc_recomp.{n}.cpp').splitlines(keepends=True)
        want = switch_expand.apply(''.join(g), exp)[0].splitlines(keepends=True)
        got = textpass.read_text(regen / f'ppc_recomp.{n}.cpp').splitlines(keepends=True)
        seg = segments(g)
        for op, i1, i2, j1, j2 in difflib.SequenceMatcher(None, want, got, autojunk=False).get_opcodes():
            if op not in ('insert', 'replace'):
                continue
            for j in range(j1, j2):
                m = FLUSH_ANY.fullmatch(got[j])
                if not m or m.group(2):
                    continue
                need = m.group(1)
                if any((u := FLUSH_ANY.fullmatch(x)) and u.group(1) == need and u.group(2) for x in want[i1:i2]):
                    verdict, row = 'corpus sets the mode unconditionally here', None
                else:
                    k = j
                    while k > 0 and not textpass.LABEL.fullmatch(got[k]) and not got[k].startswith('PPC_FUNC_IMPL'):
                        k -= 1
                    lab = textpass.LABEL.fullmatch(got[k])
                    hit = [r for r in exp if lab and lab.group(1) in r['labels']]
                    if not hit:
                        verdict, row = 'not after a recovered case label', None
                    else:
                        r = hit[0]
                        fi, _ = seg[r['function']]
                        kb = positions(g, fi, seg[r['function']][1], int(r['function'], 16))[int(r['bctr'], 16)]
                        state, q = None, kb - 1
                        while q > fi and not textpass.LABEL.fullmatch(g[q]):
                            if (f := FLUSH_ANY.fullmatch(g[q])):
                                state = f.group(1)
                                break
                            q -= 1
                        verdict = 'unknown at bctr' if state is None else 'same mode' if state == need else 'MODE MISMATCH'
                        row = {'chunk': int(n), 'function': r['function'], 'bctr': r['bctr'], 'label': lab.group(1),
                               'needs': need, 'state_at_bctr': state, 'verdict': verdict}
                counts[verdict] = counts.get(verdict, 0) + 1
                if row:
                    rows.append(row)
    if Path(a.out).exists():
        raise SystemExit(f'{a.out} exists')
    Path(a.out).write_text(json.dumps({'schema': 'gta5-switch-csr-edges/1', 'summary': counts, 'rows': rows}, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(counts))
    return 0


def cmd_verify_image(a) -> int:
    """independent evidence: re-discover each recovered table from the original instructions with switch_tables.py's
    bounded idiom scanners (read-only on the decoded image) and compare selector + targets; adds the scanner's default"""
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    import switch_tables as st  # noqa: E402
    scan = json.loads(Path(a.scan).read_text())
    image = Path(a.image).read_bytes()
    ib, cb, cs = int(a.image_base, 0), int(a.code_base, 0), int(a.code_size, 0)
    ranges = dict(st.function_ranges_from_map(Path(a.map).read_text(), cb, cs))
    counts: dict[str, int] = {}
    for rows in scan['chunks'].values():
        for r in rows:
            if 'labels' not in r:
                continue
            f = int(r['function'], 16)
            size = ranges.get(f)
            found = []
            if size is not None:
                for fn in (st.discover_u32_switches, st.discover_u16_switches, st.discover_u8_scaled_switches):
                    try:
                        found += fn(image, ib, f, size, extended_guards=True) if fn is st.discover_u32_switches \
                            else fn(image, ib, f, size)
                    except ValueError:
                        pass
            labels = tuple(int(x, 16) for x in r['labels'])
            hit = [t for t in found if t.labels == labels and t.selector == r['selector']]
            site = int(r['bctr'], 16)
            hit = [t for t in hit if t.base <= site <= t.base + 64] or hit
            if len(hit) == 1:
                t = hit[0]
                r['image'] = {'match': True, 'base': f'{t.base:08X}', 'default': f'{t.default:08X}'}
                key = 'image match' + ('' if r.get('default') in (None, f'{t.default:08X}') else ' (default differs)')
            else:
                r['image'] = {'match': False, 'candidates': len(found), 'same_targets': len(hit)}
                key = 'image: no unique bounded idiom' if not hit else 'image: ambiguous'
            counts[key] = counts.get(key, 0) + 1
    scan['image_check'] = {'image': str(a.image), 'counts': counts}
    if Path(a.out).exists():
        raise SystemExit(f'{a.out} exists')
    Path(a.out).write_text(json.dumps(scan, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(counts))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('scan')
    s.add_argument('gen'); s.add_argument('code'); s.add_argument('out'); s.add_argument('--chunks')
    t = sub.add_parser('toml')
    t.add_argument('base'); t.add_argument('scan'); t.add_argument('out')
    t.add_argument('--forms', default='live,compact,compact-default')
    t.add_argument('--allow-no-default', action='store_true')
    v = sub.add_parser('verify-image')
    v.add_argument('scan'); v.add_argument('out'); v.add_argument('--image', required=True); v.add_argument('--map', required=True)
    v.add_argument('--image-base', required=True); v.add_argument('--code-base', required=True)
    v.add_argument('--code-size', required=True)
    r = sub.add_parser('records')
    r.add_argument('scan'); r.add_argument('out')
    g = sub.add_parser('regen-check')
    g.add_argument('regen'); g.add_argument('gen'); g.add_argument('records'); g.add_argument('out')
    e = sub.add_parser('csr-edges')
    e.add_argument('regen'); e.add_argument('gen'); e.add_argument('records'); e.add_argument('out')
    a = ap.parse_args()
    return {'scan': cmd_scan, 'toml': cmd_toml, 'verify-image': cmd_verify_image, 'records': cmd_records,
            'regen-check': cmd_regen_check, 'csr-edges': cmd_csr_edges}[a.cmd](a)


if __name__ == '__main__':
    raise SystemExit(main())
