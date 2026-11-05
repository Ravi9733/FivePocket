#!/usr/bin/env python3
"""flush_model.py: XenonRecomp's flush-to-zero (CSR) mode model over generated chunk text (package ftz-fix, 2026-10-04).

The generator keeps one software state csrState in {Unknown, FPU, VMX} while it prints a function (XenonRecomp
recompiler.cpp: printSetFlushMode). Before every FPU-class instruction it needs FPU mode (host FPCR.FZ off: IEEE
denormals), before every VMX-float-class instruction VMX mode (FZ on: the Xenon VMX unit flushes denormals). When the
state differs it prints `ctx.fpscr.{disable,enable}FlushMode();` if the state is Unknown, or the `...Unconditional();`
form if the state is the other known mode, right after the instruction's `\\t// <name> <operands>` comment, and sets the
state. The state becomes Unknown at the function entry, at every `loc_X:` label ("anyone could jump to this label") and
after every `bl` / `bctrl` ("the call could change it"). Both printed forms set an absolute mode (ppc_context.h:
csr |= / &= ~FlushMask, then the host FPCR), so the only way generated code runs an instruction in the wrong mode is an
instruction that has NO flush line while the actual mode differs from the generator's state: the generator guarantees
that only along fall-through from code it printed itself. A text pass that adds a `loc_X:` label (a switch expansion, a
jump-table repair) and jumps there without the reset breaks that guarantee: code after the label assumes the mode of the
fall-through predecessor.

  flush_model.py check-generator DIR [--chunks N,M | @FILE]
        Re-simulates printSetFlushMode over every PPC_FUNC_IMPL body of DIR/ppc_recomp.N.cpp (pure generator output) and
        compares the predicted flush line (none / conditional / unconditional, mode) with the printed one at every
        instruction. Validates the model (instruction classes, reset points) against the generator: expect 0 disagreements.
  flush_model.py scan FILE... [--json OUT]
        The same simulation over post-processed chunks: lists every instruction where the text has no flush line although
        the generator, given the text's labels and calls, would print one (a HAZARD: the code relies on the mode of a
        fall-through predecessor across a label / call / entry), and where the text has an unconditional line although the
        generator would print the conditional one (benign).
(The control-flow dataflow of the actual modes at such sites is tools/fp_codegen/ftz_modes.py.)

Instruction comments are told from the repair / stitcher / ERROR comments by their lower-case mnemonic; addresses count
one comment per instruction from the entry and re-synchronise at every label (Body.exact = no inconsistency). Instruction
classes are the generator's (recompiler.cpp cases that call printSetFlushMode; vpkd3d128 except pack types 2
and 4); record forms (`name.`) belong to their base class. Contains no game code or game-derived data.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import textpass  # noqa: E402
from g4text import REPAIR, segments  # noqa: E402

FPU = frozenset('''fabs fadd fadds fcfid fcmpu fctid fctidz fctiwz fdiv fdivs fmadd fmadds fmr fmsub fmsubs fmul fmuls
    fnabs fneg fnmadds fnmsub fnmsubs fres frsqrte frsp fsel fsqrt fsqrts fsub fsubs lfd lfdx lfdu lfdux lfs lfsu lfsux
    lfsx stfd stfdx stfdu stfdux stfiwx stfs stfsu stfsux stfsx'''.split())
VMX = frozenset('''vaddfp vaddfp128 vctsxs vcfpsxws128 vcfsx vcsxwfp128 vcfux vcuxwfp128 vcmpbfp vcmpbfp128 vcmpeqfp
    vcmpeqfp128 vcmpgefp vcmpgefp128 vcmpgtfp vcmpgtfp128 vexptefp vexptefp128 vlogefp vlogefp128 vmaddcfp128 vmaddfp
    vmaddfp128 vmaxfp vmaxfp128 vminfp vminfp128 vmsum3fp128 vmsum4fp128 vmulfp128 vnmsubfp vnmsubfp128 vpkd3d128 vrefp
    vrefp128 vrfim vrfim128 vrfin vrfin128 vrfip vrfip128 vrfiz vrfiz128 vrsqrtefp vrsqrtefp128 vctuxs vcfpuxws128 vsubfp
    vsubfp128'''.split())
CALLS = frozenset(('bl', 'bctrl'))          # the generator's csrState = Unknown after these
TERMINAL = frozenset(('b', 'blr', 'bctr'))  # no fall-through to the next instruction
FLUSH = re.compile(r'\tctx\.fpscr\.(enable|disable)FlushMode(Unconditional)?\(\);\n')
GOTO = re.compile(r'goto loc_([0-9A-F]+);')
MODE = {'enable': 'E', 'disable': 'D'}
NAME = {'E': 'enable', 'D': 'disable'}


def mnemonic(comment: str) -> tuple[str, str]:
    """(name, operands) of a generator instruction comment `\\t// <name> <operands>`"""
    body = comment[4:].rstrip('\n')
    name, _, ops = body.partition(' ')
    return name, ops.strip()


def required(comment: str) -> str | None:
    """'E' / 'D' when the generator puts this instruction in VMX / FPU mode, else None"""
    name, ops = mnemonic(comment)
    base = name[:-1] if name.endswith('.') else name
    if base in FPU:
        return 'D'
    if base in VMX:
        if base == 'vpkd3d128':
            parts = ops.split(',')
            if len(parts) > 2 and parts[2].strip() in ('2', '4'):
                return None
        return 'E'
    return None


INSN = re.compile(r'\t// ([a-z][a-z0-9]*\.?[+-]?)( |\n)')   # name, record dot, branch-prediction hint


def is_instruction(line: str) -> bool:
    """a generator instruction comment (`\\t// <name> <operands>`), not a repair / stitcher / ERROR comment"""
    return INSN.match(line) is not None and not REPAIR.match(line)


def addresses(lines: list[str], i: int, j: int, start: int) -> tuple[dict[int, int], list[str]]:
    """guest address -> comment line of every instruction of the body lines[i+1:j], counting one comment per instruction
    from `start` and re-synchronising at every `loc_X:` label (X is the address of the instruction that follows it). A
    label that moves the count backwards, or forwards after a transfer that can fall through, is reported (the body then
    carries comments the count does not know; addresses up to the next label may be off): exact == no reports."""
    pc, pos, notes, last = start, {}, [], ''
    for k in range(i + 1, j):
        line = lines[k]
        m = textpass.LABEL.fullmatch(line)
        if m:
            x = int(m.group(1), 16)
            if x != pc and not (x > pc and textpass.UNCONDITIONAL.match(last)):
                notes.append(f'label {m.group(1)} at count {pc:08X}')
            pc = x
            continue
        if is_instruction(line):
            if pc in pos:
                notes.append(f'address {pc:08X} twice')
            pos[pc] = k
            pc += 4
            last = line
    return pos, notes


class Body:
    """one PPC_FUNC_IMPL body: instruction blocks in text order with their labels, flush lines, gotos and calls"""

    def __init__(self, lines: list[str], i: int, j: int, start: int):
        self.lines, self.i, self.j, self.start = lines, i, j, start
        self.pos, self.notes = addresses(lines, i, j, start)   # address -> comment line
        self.exact = not self.notes
        self.order = sorted(self.pos, key=lambda a: self.pos[a])
        ks = [self.pos[a] for a in self.order] + [j]
        self.block, self.label = {}, {}
        prev = i
        for n, a in enumerate(self.order):
            k, end = ks[n], ks[n + 1]
            # the block of an instruction: its comment line up to the next instruction comment or label
            stop = k + 1
            while stop < end and not textpass.LABEL.fullmatch(lines[stop]):
                stop += 1
            self.block[a] = (k, stop)
            # a label anywhere between the previous instruction's comment and this one (positions() has checked that
            # every label sits at the address of the instruction that follows it)
            self.label[a] = any(textpass.LABEL.fullmatch(lines[q]) for q in range(prev + 1, k))
            prev = k

    def name(self, a: int) -> str:
        return mnemonic(self.lines[self.pos[a]])[0]

    def flushes(self, a: int) -> list[tuple[int, str, bool]]:
        """(line, mode, unconditional) of the one-tab flush lines in a's block"""
        k, stop = self.block[a]
        out = []
        for q in range(k + 1, stop):
            m = FLUSH.fullmatch(self.lines[q])
            if m:
                out.append((q, MODE[m.group(1)], bool(m.group(2))))
        return out

    def gotos(self, a: int) -> list[int]:
        k, stop = self.block[a]
        return [int(m.group(1), 16) for q in range(k + 1, stop) for m in GOTO.finditer(self.lines[q])]

    def call_target(self, a: int) -> int | None:
        name, ops = mnemonic(self.lines[self.pos[a]])
        if name == 'bl':
            try:
                return int(ops, 16)
            except ValueError:
                return None
        return None


def simulate(body: Body) -> list[dict]:
    """per instruction where the printed flush line differs from what the generator would print given the text's labels
    and calls: {"addr", "need", "state", "expected": cond|uncond, "actual": none|cond|uncond|..., "label": last label}"""
    state, last_label, out = 'U', None, []
    for a in body.order:
        if body.label[a]:
            state, last_label = 'U', a
        if a == body.start:
            last_label = None
        need = required(body.lines[body.pos[a]])
        fl = body.flushes(a)
        exp = None
        if need is not None and state != need:
            exp = 'cond' if state == 'U' else 'uncond'
        act = None
        if fl:
            q, m, unc = fl[0]
            act = ('uncond' if unc else 'cond') + ('' if m == need else f'-wrong-mode-{m}')
            if len(fl) > 1:
                act += '-multiple'
        if exp != act:
            out.append({'addr': a, 'need': need, 'state': state, 'expected': exp, 'actual': act,
                        'label': last_label, 'line': body.pos[a]})
        if need is not None:
            state = need
        elif fl:
            state = fl[-1][1]
        if body.name(a) in CALLS:
            state = 'U'
    return out


def chunk_bodies(text: str):
    lines = text.splitlines(keepends=True)
    for name, (i, j) in segments(lines).items():
        yield name, Body(lines, i, j, int(name, 16))


def cmd_check_generator(a) -> int:
    d = Path(a.dir)
    files = sorted(d.glob('ppc_recomp.*.cpp'), key=lambda p: int(p.name.split('.')[1]))
    if a.chunks:
        spec = Path(a.chunks[1:]).read_text() if a.chunks.startswith('@') else a.chunks
        want = {int(x) for x in re.split(r'[\s,]+', spec) if x}
        files = [p for p in files if int(p.name.split('.')[1]) in want]
    tot = {'chunks': 0, 'functions': 0, 'instructions': 0, 'flush_lines': 0, 'disagreements': 0}
    bad = []
    for p in files:
        tot['chunks'] += 1
        for name, b in chunk_bodies(textpass.read_text(p)):
            tot['functions'] += 1
            tot['inexact_addresses'] = tot.get('inexact_addresses', 0) + (not b.exact)
            if not b.exact and len(bad) < 20:
                bad.append({'chunk': p.name, 'function': name, 'notes': b.notes[:3]})
            tot['instructions'] += len(b.order)
            tot['flush_lines'] += sum(len(b.flushes(x)) for x in b.order)
            for r in simulate(b):
                tot['disagreements'] += 1
                if len(bad) < 20:
                    bad.append({'chunk': p.name, 'function': name, **{k: (f'{v:08X}' if k in ('addr', 'label') and v else v)
                                                                       for k, v in r.items()}})
    print(json.dumps(tot))
    for r in bad:
        print(' ', r)
    return 1 if tot['disagreements'] else 0


def classify(r: dict) -> str:
    if r['expected'] == 'cond' and r['actual'] is None:
        return 'hazard'
    if r['expected'] == 'cond' and r['actual'] == 'uncond':
        return 'unconditional-where-conditional'
    if r['expected'] is None and r['actual'] in ('cond', 'uncond'):
        return 'extra-flush-line'
    return 'other'


def scan_text(text: str) -> list[dict]:
    rows = []
    for name, b in chunk_bodies(text):
        for r in simulate(b):
            rows.append({'function': name, 'kind': classify(r), 'addr': f'{r["addr"]:08X}',
                         'label': f'{r["label"]:08X}' if r['label'] is not None else None, 'need': r['need'],
                         'state': r['state'], 'expected': r['expected'], 'actual': r['actual'], 'exact': b.exact})
    return rows


def cmd_scan(a) -> int:
    out, counts = {}, {}
    for f in a.files:
        rows = scan_text(textpass.read_text(Path(f)))
        out[f] = rows
        for r in rows:
            counts[r['kind']] = counts.get(r['kind'], 0) + 1
        print(f'{Path(f).name}: ' + json.dumps({k: sum(1 for r in rows if r['kind'] == k) for k in sorted({r['kind'] for r in rows})}))
    if a.json:
        if Path(a.json).exists():
            raise SystemExit(f'{a.json} exists')
        Path(a.json).write_text(json.dumps({'schema': 'gta5-flush-scan/1', 'counts': counts, 'files': out}, indent=1) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(counts))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    g = sub.add_parser('check-generator')
    g.add_argument('dir'); g.add_argument('--chunks')
    s = sub.add_parser('scan')
    s.add_argument('files', nargs='+'); s.add_argument('--json')
    a = ap.parse_args()
    return {'check-generator': cmd_check_generator, 'scan': cmd_scan}[a.cmd](a)


if __name__ == '__main__':
    raise SystemExit(main())
