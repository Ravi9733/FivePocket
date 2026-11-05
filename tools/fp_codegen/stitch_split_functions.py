#!/usr/bin/env python3
"""Rebuild analyzer-split functions from the original code and the generator's own translations.

XenonAnalyse sometimes ends a function at an inline jump table (or at a branch it could not
follow) and starts a new "function" at the next word. XenonRecomp then emits:

- the table dispatch as `PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32)`, which stops the game with
  invalid-indirect-target when a case label has no function entry (attempt v2, 0x82EF1114);
- every branch that crosses the split as `// ERROR <target>` + `return;`, a silent early
  return.

For a root function this tool collects every instruction reachable from its entry through
local control flow: fall-through, direct branches whose target is not a genuine function
entry, and guarded inline u32 jump tables. A branch to a genuine entry stays a tail call; a
`bl` stays a call. Every non-branch instruction reuses the generator's exact text for that
address (located through the generator's per-instruction comments and label/LR checks);
branches, returns and table dispatches are re-derived from the original encoding. The
rebuilt body replaces only the root's definition. Split pieces stay mapped and unchanged,
so any caller of them is unaffected; duplicated code is safe because PowerPC code is
position-independent and every `bl` sets LR explicitly.

"Genuine entry": a mapped entry that is called (`bl`), referenced by an absolute word, has
its address materialized by lis/addi or lis/ori, or is branched to from outside the region
being rebuilt. Entries with none of these are analyzer artifacts and are rebuilt inline.

Read-only over the corpus; writes patched chunk copies only where told to.
"""
from __future__ import annotations

import argparse
from bisect import bisect_right
from dataclasses import dataclass, field
import json
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from leaf_switch_fixture import extract_function  # noqa: E402
from preflight import HEADER, index_functions, source_positions  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
IMAGE_BASE = 0x82000000
MAX_REGION = 0x10000  # bytes a rebuilt function may span
MAX_CASES = 512


class Reject(ValueError):
    pass


def word_at(image: bytes, pc: int) -> int:
    return struct.unpack_from('>I', image, pc - IMAGE_BASE)[0]


def s16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def branch_target(pc: int, w: int) -> int | None:
    op = w >> 26
    if op == 18:
        d = w & 0x03FFFFFC
        d = d - 0x04000000 if d & 0x02000000 else d
        return (d if w & 2 else pc + d) & 0xFFFFFFFF
    if op == 16:
        d = s16(w & 0xFFFC)
        return (d if w & 2 else pc + d) & 0xFFFFFFFF
    return None


def cond_expr(bo: int, bi: int) -> str | None:
    """C condition for a BO/BI pair in the generator's spelling; None when always taken."""
    parts = []
    if not bo & 0x04:
        parts.append('ctx.ctr.u32 == 0' if bo & 0x02 else 'ctx.ctr.u32 != 0')
    if not bo & 0x10:
        flag = f'ctx.cr{bi >> 2}.{("lt", "gt", "eq", "so")[bi & 3]}'
        parts.append(flag if bo & 0x08 else '!' + flag)
    return ' && '.join(parts) if parts else None


@dataclass
class Dispatch:
    bctr: int
    selector: int
    table: int
    targets: list[int]


@dataclass
class Plan:
    root: int
    instructions: list[int]
    labels: set[int]
    dispatches: dict[int, Dispatch]
    pieces: list[int]
    errors_fixed: int = 0
    tail_calls: set[int] = field(default_factory=set)


class Corpus:
    def __init__(self, sources: Path, mapping: Path, image: bytes):
        self.image = image
        text = mapping.read_text(encoding='utf-8')
        self.names = {int(m[1], 16): m[2] for m in re.finditer(r'\{ 0x([0-9A-F]+), (\w+) \}', text)}
        self.entries = sorted(self.names)
        self.entry_set = set(self.entries)
        self.chunk_of: dict[int, Path] = {}
        self.functions: dict[int, str] = {}
        for path in sorted(sources.glob('ppc_recomp.*.cpp')):
            if not re.fullmatch(r'ppc_recomp\.\d+\.cpp', path.name):
                continue
            source = path.read_text(errors='replace', encoding='utf-8')
            for address, body in index_functions(source).items():
                self.chunk_of[address] = path
                self.functions[address] = body
        self._positions: dict[int, dict[int, int] | None] = {}
        self._refs: dict[int, list[tuple[str, int]]] | None = None
        self._encodings: dict[int, str] | None = None
        self.borrowed = 0

    def interval(self, entry: int) -> tuple[int, int]:
        i = bisect_right(self.entries, entry)
        return entry, self.entries[i] if i < len(self.entries) else entry + 4

    def owner(self, pc: int) -> int:
        i = bisect_right(self.entries, pc) - 1
        if i < 0:
            raise Reject(f'{pc:08X} precedes every mapped entry')
        return self.entries[i]

    def body(self, entry: int) -> str:
        path = self.chunk_of.get(entry)
        if path is None:
            raise Reject(f'no generated definition for {entry:08X}')
        return extract_function(path.read_text(errors='replace', encoding='utf-8'), entry, allow_indirect=True)

    def relaxed_positions(self, entry: int) -> dict[int, int]:
        """Positions of the instructions the generated body covers, without requiring it to span
        the whole mapped interval (the split case: the body ends at an unresolved table dispatch).
        Labels and LR constants must still agree with the instruction positions."""
        body = self.body(entry)
        pc, offset, positions = entry, 0, {}
        for line in body.splitlines(keepends=True):
            label = re.fullmatch(r'loc_([0-9A-F]+):\n', line)
            if label and int(label[1], 16) != pc:
                raise Reject(f'label disagrees with position in {entry:08X}')
            if line.startswith('\t// ') and not line.startswith('\t// ERROR '):  # marker, not an instruction
                positions[pc] = offset
                pc += 4
            lr = re.fullmatch(r'\tctx.lr = (0x[0-9A-F]+);\n', line)
            if lr and int(lr[1], 16) != pc:
                raise Reject(f'LR disagrees with position in {entry:08X}')
            offset += len(line)
        return positions

    def encoding_text(self, pc: int) -> str | None:
        """The generator's text for this exact 32-bit encoding, taken from any other address.

        Only straight-line translations qualify (no calls, returns, gotos, hooks, ERROR markers),
        and every sampled occurrence must produce identical text."""
        if self._encodings is None:
            self._encodings = {}
            bad = set()
            for entry in self.entries:
                if entry not in self.functions:
                    continue
                try:
                    positions = self.relaxed_positions(entry)
                except Reject:
                    continue
                body = self.functions[entry]
                ordered = sorted(positions.items())
                for i, (at, start) in enumerate(ordered):
                    end = ordered[i + 1][1] if i + 1 < len(ordered) else None
                    text = body[start:end] if end is not None else None
                    if text is None:
                        continue
                    lines = [l for l in text.splitlines(keepends=True) if not re.fullmatch(r'loc_[0-9A-F]+:\n', l)]
                    text = ''.join(lines)
                    word = word_at(self.image, at)
                    if word in bad:
                        continue
                    if re.search(r'goto |return;|\(ctx, base\)|ERROR|PPC_CALL|hook|longjmp|setjmp', text) or not text.startswith('\t// '):
                        bad.add(word)
                        self._encodings.pop(word, None)
                        continue
                    known = self._encodings.get(word)
                    if known is None:
                        self._encodings[word] = text
                    elif known != text:
                        bad.add(word)
                        self._encodings.pop(word, None)
        return self._encodings.get(word_at(self.image, pc))

    def positions(self, entry: int) -> dict[int, int] | None:
        if entry not in self._positions:
            start, end = self.interval(entry)
            try:
                self._positions[entry] = source_positions(self.body(entry), self.image, IMAGE_BASE, start, end - start)
            except (ValueError, Reject):
                self._positions[entry] = None
        return self._positions[entry]

    def translation(self, pc: int) -> str:
        """The generator's text for the instruction at pc: its comment and code, no labels.

        From the owning function's body when it covers pc; otherwise from the same encoding
        elsewhere in the corpus (straight-line instructions only; see encoding_text)."""
        entry = self.owner(pc)
        positions = self.positions(entry)
        if positions is None or pc not in positions:
            try:
                relaxed = self.relaxed_positions(entry)
            except Reject:
                relaxed = {}
            if pc in relaxed:
                positions = relaxed
            else:
                text = self.encoding_text(pc)
                if text is None:
                    raise Reject(f'no correlated generator text for {pc:08X} (in {entry:08X})')
                self.borrowed += 1
                return text
        body = self.body(entry)
        start = positions[pc]
        following = [o for p, o in positions.items() if p > pc]
        end = min(following) if following else body.rindex('}')
        lines = [l for l in body[start:end].splitlines(keepends=True) if not re.fullmatch(r'loc_[0-9A-F]+:\n', l)]
        return ''.join(lines)

    def references(self) -> dict[int, list[tuple[str, int]]]:
        """Image-wide references to mapped entries: calls, branches, absolute words, lis pairs."""
        if self._refs is not None:
            return self._refs
        refs: dict[int, list[tuple[str, int]]] = {}
        image, entries = self.image, self.entry_set
        count = len(image) // 4
        words = struct.unpack_from(f'>{count}I', image, 0)
        lis: dict[int, tuple[int, int]] = {}  # register -> (high, pc)
        for i, w in enumerate(words):
            pc = IMAGE_BASE + 4 * i
            if w in entries:
                refs.setdefault(w, []).append(('word', pc))
            op = w >> 26
            if op in (16, 18):
                t = branch_target(pc, w)
                if t in entries:
                    refs.setdefault(t, []).append(('call' if w & 1 else 'branch', pc))
            elif op == 15 and not (w >> 16) & 31:
                lis[(w >> 21) & 31] = ((w & 0xFFFF) << 16, pc)
            elif op in (14, 24) and (w >> 16) & 31 in lis:
                high, at = lis[(w >> 16) & 31]
                if pc - at <= 32:
                    value = (high + (s16(w & 0xFFFF) if op == 14 else w & 0xFFFF)) & 0xFFFFFFFF
                    if value in entries:
                        refs.setdefault(value, []).append(('address', pc))
        self._refs = refs
        return refs

    def genuine(self, entry: int, region: tuple[int, int]) -> bool:
        """Called anywhere, or referenced in any way from outside the region being rebuilt.

        References from inside the region (its own jump-table words, the lis/addi that forms a
        table base, local branches) do not make an entry genuine.
        """
        lo, hi = region
        for kind, pc in self.references().get(entry, []):
            if kind == 'call' or not lo <= pc < hi:
                return True
        return False


def find_dispatch(image: bytes, bctr: int) -> Dispatch | None:
    """A guarded inline u32 jump table ending at this bctr, or None."""
    if word_at(image, bctr) != 0x4E800420:
        return None
    seq = [word_at(image, bctr - 4 * k) for k in range(5, 0, -1)]  # the five words before bctr
    op = lambda w: w >> 26  # noqa: E731
    rt = lambda w: (w >> 21) & 31  # noqa: E731
    ra = lambda w: (w >> 16) & 31  # noqa: E731
    rb = lambda w: (w >> 11) & 31  # noqa: E731
    high, a, b, load, mtctr = seq
    if op(high) != 15 or ra(high):
        return None
    low, shift = (a, b) if op(a) == 14 else (b, a)
    table, selector, offset = rt(high), rt(shift), ra(shift)
    if not (op(low) == 14 and rt(low) == table and ra(low) == table and op(shift) == 21 and rb(shift) == 2
            and (shift >> 6) & 31 == 0 and (shift >> 1) & 31 == 29 and not shift & 1
            and len({table, selector, offset}) == 3
            and load == (31 << 26) | (offset << 21) | (table << 16) | (offset << 11) | (23 << 1)
            and mtctr == (31 << 26) | (offset << 21) | (9 << 16) | (467 << 1)):
        return None
    address = (((high & 0xFFFF) << 16) + s16(low & 0xFFFF)) & 0xFFFFFFFF
    # Guard: cmplwi crX,rSel,K then bgt crX (branch if true on gt) within the four words before lis.
    # The out-of-range branch may also be bgtlr crX (a conditional return, e.g. 829A2270).
    lis_pc = bctr - 20
    bound = None
    is_branch = lambda w: op(w) == 16 or (op(w) == 19 and (w >> 1) & 0x3FF == 16)  # noqa: E731  bc / bclr
    for k in range(1, 5):
        c = word_at(image, lis_pc - 4 * k)
        if op(c) == 10 and (c >> 16) & 31 == selector and not (c >> 21) & 1:
            crf = (c >> 23) & 7
            between = [word_at(image, lis_pc - 4 * j) for j in range(k - 1, 0, -1)]
            branches = [w for w in between if is_branch(w)]
            if (len(branches) == 1 and (branches[0] >> 21) & 0x1C == 0x0C and (branches[0] >> 16) & 31 == crf * 4 + 1
                    and not branches[0] & 1):
                writes = [w for w in between if not is_branch(w) and rt(w) == selector]
                if not writes:
                    bound = (c & 0xFFFF) + 1
            break
    if bound is None or not 1 <= bound <= MAX_CASES:
        return None
    if address != bctr + 4:
        return None  # only inline tables: the analyzer-split case this tool is for
    targets = [word_at(image, address + 4 * i) for i in range(bound)]
    return Dispatch(bctr, selector, address, targets)


def walk(corpus: Corpus, root: int, region: tuple[int, int]):
    """Instructions reachable from root when genuine entries are judged against this region."""
    image = corpus.image
    lo, hi = root - MAX_REGION, root + MAX_REGION
    seen: set[int] = set()
    labels: set[int] = set()
    dispatches: dict[int, Dispatch] = {}
    data: set[int] = set()
    tail_calls: set[int] = set()
    work = [root]
    while work:
        pc = work.pop()
        while pc not in seen:
            if not lo <= pc < hi or not 0 <= pc - IMAGE_BASE <= len(image) - 4:
                raise Reject(f'control flow leaves the bounded window at {pc:08X}')
            if pc in data:
                raise Reject(f'control flow reaches table data at {pc:08X}')
            seen.add(pc)
            w = word_at(image, pc)
            op = w >> 26
            if op in (16, 18):
                t = branch_target(pc, w)
                always = op == 18 or (w >> 21) & 0x14 == 0x14
                if not w & 1:
                    # A mapped entry outside the bounded window cannot be part of this function; the
                    # generator already emits the branch as a tail call to it (82460A10 -> 823B74B8).
                    far = t in corpus.entry_set and not lo <= t < hi
                    if t in corpus.entry_set and t != root and (far or corpus.genuine(t, region)):
                        tail_calls.add(t)
                    else:
                        labels.add(t)
                        work.append(t)
                    if always:
                        break
                pc += 4
                continue
            if op == 19:
                xo = (w >> 1) & 0x3FF
                bo = (w >> 21) & 31
                if xo == 16:  # bclr
                    if w & 1:
                        raise Reject(f'bclrl at {pc:08X}')
                    if bo & 0x14 == 0x14:
                        break
                    pc += 4
                    continue
                if xo == 528:  # bcctr
                    if w & 1:
                        pc += 4  # bctrl: an indirect call returns here
                        continue
                    if bo & 0x14 != 0x14:
                        raise Reject(f'conditional bcctr at {pc:08X}')
                    d = find_dispatch(image, pc)
                    if d:
                        dispatches[pc] = d
                        data.update(range(d.table, d.table + 4 * len(d.targets), 4))
                        for t in d.targets:
                            labels.add(t)
                            work.append(t)
                    break  # a table dispatch or an indirect tail call: no fall-through
            pc += 4
    if data & seen:
        raise Reject('table data is also executed')
    return seen, labels, dispatches, data, tail_calls


def plan_function(corpus: Corpus, root: int) -> Plan:
    region = corpus.interval(root)
    for _ in range(32):
        seen, labels, dispatches, data, tail_calls = walk(corpus, root, region)
        span = seen | data
        grown = (min(region[0], min(span)), max(region[1], max(span) + 4))
        if grown == region:
            break
        region = grown
    else:
        raise Reject('rebuilt region did not converge')
    pieces = sorted(e for e in corpus.entry_set if region[0] <= e < region[1] and e != root)
    return Plan(root, sorted(seen), labels, dispatches, pieces, tail_calls=tail_calls)


def emit(corpus: Corpus, plan: Plan) -> str:
    image = corpus.image
    out = [f'PPC_FUNC_IMPL(__imp__sub_{plan.root:X}) {{', '\tPPC_FUNC_PROLOGUE();']
    out.append(f'\t// Rebuilt by stitch_split_functions.py: {len(plan.instructions)} instructions reachable from '
               f'0x{plan.root:08X}, {len(plan.dispatches)} inline table(s); split pieces '
               + (', '.join(f'0x{p:08X}' for p in plan.pieces) or 'none') + ' left mapped and unchanged.')
    order = plan.instructions
    if order[0] != plan.root:
        out.append(f'\tgoto loc_{plan.root:X};')
    previous_falls = False
    for index, pc in enumerate(order):
        if index and pc != order[index - 1] + 4 and previous_falls:
            raise Reject(f'fall-through gap before {pc:08X}')
        if pc in plan.labels or (pc == plan.root and order[0] != plan.root):
            out.append(f'loc_{pc:X}:')
        w = word_at(image, pc)
        op = w >> 26
        previous_falls = True
        text = None
        if op in (16, 18) and not w & 1:
            t = branch_target(pc, w)
            bo, bi = ((w >> 21) & 31, (w >> 16) & 31) if op == 16 else (20, 0)
            comment = corpus.translation(pc).splitlines()[0]
            cond = cond_expr(bo, bi)
            decrement = op == 16 and not bo & 0x04
            callee = corpus.names.get(t, f'sub_{t:X}')  # helpers keep their mapped names (__restgprlr_N)
            action = f'goto loc_{t:X};' if t not in plan.tail_calls else f'{{\n\t\t{callee}(ctx, base);\n\t\treturn;\n\t}}'
            if t in plan.tail_calls and cond is None:
                action = f'{callee}(ctx, base);\n\treturn;'
            lines = [comment]
            if decrement:
                lines.append('\t--ctx.ctr.u64;')
            lines.append(f'\tif ({cond}) {action}' if cond else f'\t{action}')
            text = '\n'.join(lines) + '\n'
            if cond is None:
                previous_falls = False
        elif op == 19 and (w >> 1) & 0x3FF == 16 and not w & 1:
            bo, bi = (w >> 21) & 31, (w >> 16) & 31
            comment = corpus.translation(pc).splitlines()[0]
            cond = cond_expr(bo, bi)
            if cond and not bo & 0x04:
                text = f'{comment}\n\t--ctx.ctr.u64;\n\tif ({cond}) return;\n'
            else:
                text = f'{comment}\n\t' + (f'if ({cond}) return;' if cond else 'return;') + '\n'
            if cond is None:
                previous_falls = False
        elif pc in plan.dispatches:
            d = plan.dispatches[pc]
            comment = corpus.translation(pc).splitlines()[0]
            lines = [comment + f' (inline table 0x{d.table:08X}, {len(d.targets)} entries)',
                     f'\tswitch (ctx.r{d.selector}.u32) {{']
            for i, t in enumerate(d.targets):
                lines += [f'\tcase {i}:', f'\t\tif (ctx.ctr.u32 != 0x{t:X}U) {{',
                          '\t\t\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32); return;', '\t\t}', f'\t\tgoto loc_{t:X};']
            lines += ['\tdefault:', '\t\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32); return;', '\t}']
            text = '\n'.join(lines) + '\n'
            previous_falls = False
        elif op == 19 and (w >> 1) & 0x3FF == 528 and not w & 1:
            text = corpus.translation(pc)  # an unresolved bctr: the generator's indirect tail call
            if 'PPC_CALL_INDIRECT_FUNC' not in text:
                raise Reject(f'unexpected bctr translation at {pc:08X}')
            previous_falls = False
        elif op == 18 and w & 1 and not w & 2:  # bl: the generator's call form, for covered or uncovered code
            t = branch_target(pc, w)
            if t not in corpus.entry_set:
                raise Reject(f'call to unmapped {t:08X} at {pc:08X}')
            try:
                text = corpus.translation(pc)
            except Reject:
                text = f'\t// bl 0x{t:x}\n\tctx.lr = 0x{pc + 4:X};\n\t{corpus.names.get(t, f"sub_{t:X}")}(ctx, base);\n'
            if f'ctx.lr = 0x{pc + 4:X};' not in text or f'{corpus.names.get(t, f"sub_{t:X}")}(ctx, base);' not in text:
                raise Reject(f'unexpected call translation at {pc:08X}')
        else:
            text = corpus.translation(pc)
            if '// ERROR' in text or re.search(r'^\s*goto loc_', text, re.M):
                raise Reject(f'non-branch translation with control flow at {pc:08X}')
        out.append(text.rstrip('\n'))
    if previous_falls:
        raise Reject('rebuilt body falls off its end')
    out.append('}')
    body = '\n'.join(out) + '\n'
    # The generator declares these after the prologue when an instruction's text needs them.
    declarations = [d for name, d in (('ea', '\tuint32_t ea{};'), ('temp', '\tPPCRegister temp{};'),
                                      ('vTemp', '\tPPCVRegister vTemp{};'))
                    if re.search(rf'\b{name}\b', body.split('PPC_FUNC_PROLOGUE();', 1)[1])]
    if declarations:
        body = body.replace('\tPPC_FUNC_PROLOGUE();\n', '\tPPC_FUNC_PROLOGUE();\n' + '\n'.join(declarations) + '\n', 1)
    return body


def verify_control_flow(corpus: Corpus, plan: Plan, body: str) -> None:
    """Re-derive every instruction's successors from the emitted C++ and compare with the image.

    Independent of the planner: it reads only the original words and the emitted text. Each
    instruction must keep its original successor set (fall-through, branch target, call return,
    table cases), each label must sit on its own instruction, and fall-through must reach pc+4.
    """
    image = corpus.image
    lines = body.split('PPC_FUNC_PROLOGUE();\n', 1)[1].splitlines()
    blocks: list[list[str]] = []
    label_at: dict[int, int] = {}
    pending_labels: list[int] = []
    started = False
    for line in lines:
        m = re.fullmatch(r'loc_([0-9A-F]+):', line)
        if m:
            pending_labels.append(int(m[1], 16))
            continue
        if line.startswith('\t// ') and not line.startswith('\t// Rebuilt by'):
            started = True
            for l in pending_labels:
                label_at[l] = len(blocks)
            pending_labels = []
            blocks.append([line])
        elif started:
            blocks[-1].append(line)
    if len(blocks) != len(plan.instructions):
        raise Reject(f'{len(blocks)} emitted instruction blocks for {len(plan.instructions)} instructions')
    order = plan.instructions
    index_of = {pc: i for i, pc in enumerate(order)}
    for label, i in label_at.items():
        if order[i] != label:
            raise Reject(f'label loc_{label:X} sits on {order[i]:08X}')

    def falls_to_next(i: int) -> None:
        if i + 1 >= len(order) or order[i + 1] != order[i] + 4:
            raise Reject(f'{order[i]:08X} falls through but the next emitted instruction is not pc+4')

    def local(t: int, text: str) -> bool:
        if f'goto loc_{t:X};' in text:
            if t not in label_at or t not in index_of:
                raise Reject(f'goto loc_{t:X} without an emitted label')
            return True
        callee = corpus.names.get(t, f'sub_{t:X}')
        if f'{callee}(ctx, base);' in text and 'return;' in text:
            if not corpus.genuine(t, (min(order), max(order) + 4)) and t in index_of:
                raise Reject(f'tail call to rebuilt local code {t:08X}')
            return False
        raise Reject(f'branch to {t:08X} is neither a local goto nor a tail call')

    for i, pc in enumerate(order):
        w = word_at(image, pc)
        op = w >> 26
        text = '\n'.join(blocks[i])
        gotos = re.findall(r'goto loc_([0-9A-F]+);', text)
        if op in (16, 18) and not w & 1:
            t = branch_target(pc, w)
            always = op == 18 or (w >> 21) & 0x14 == 0x14
            local(t, text)
            if len(set(gotos) - {f'{t:X}'}):
                raise Reject(f'{pc:08X}: extra gotos {gotos}')
            if not always:
                if 'if (' not in text:
                    raise Reject(f'{pc:08X}: conditional branch emitted unconditionally')
                falls_to_next(i)
        elif op == 19 and (w >> 1) & 0x3FF == 16:
            always = (w >> 21) & 0x14 == 0x14
            if 'return;' not in text or gotos:
                raise Reject(f'{pc:08X}: bclr lost its return')
            if not always:
                if 'if (' not in text:
                    raise Reject(f'{pc:08X}: conditional return emitted unconditionally')
                falls_to_next(i)
        elif op == 19 and (w >> 1) & 0x3FF == 528 and not w & 1:
            if pc in plan.dispatches:
                d = find_dispatch(image, pc)
                cases = dict(re.findall(r'case (\d+):\n\t\tif \(ctx.ctr.u32 != 0x[0-9A-F]+U\) \{\n\t\t\t'
                                        r'PPC_CALL_INDIRECT_FUNC\(ctx.ctr.u32\); return;\n\t\t\}\n\t\tgoto loc_([0-9A-F]+);', text))
                if d is None or len(cases) != len(d.targets) or any(
                        int(cases[str(k)], 16) != t or t not in label_at for k, t in enumerate(d.targets)):
                    raise Reject(f'{pc:08X}: switch cases differ from the image table')
                if f'switch (ctx.r{d.selector}.u32)' not in text:
                    raise Reject(f'{pc:08X}: switch selector differs')
            elif 'PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32)' not in text or gotos:
                raise Reject(f'{pc:08X}: indirect tail branch changed')
        else:
            if gotos or re.search(r'^\s*return;', text, re.M) and not (op == 19 and w & 1):
                raise Reject(f'{pc:08X}: straight-line instruction transfers control')
            falls_to_next(i)


def rebuild(corpus: Corpus, root: int) -> tuple[Plan, str]:
    plan = plan_function(corpus, root)
    if not plan.pieces and not plan.dispatches:
        raise Reject('nothing split: the region is the mapped interval and has no inline table')
    body = emit(corpus, plan)
    verify_control_flow(corpus, plan, body)
    return plan, body


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', type=Path, default=ROOT / 'local-analysis/Disc2-Play/default.image.bin')
    parser.add_argument('--sources', type=Path, default=ROOT / 'local-analysis/android-private-trace-window/api26/validated-ppc')
    parser.add_argument('--root', action='append', default=[], help='hex entry of a function to rebuild')
    parser.add_argument('--candidates', action='store_true',
                        help='every root whose region holds a generator ERROR branch or an inline-table dispatch')
    parser.add_argument('--out', type=Path, help='directory for patched chunk copies (ignored private path)')
    parser.add_argument('--report', type=Path)
    parser.add_argument('--show', action='store_true', help='print rebuilt bodies')
    args = parser.parse_args()
    corpus = Corpus(args.sources, args.sources / 'ppc_func_mapping.cpp', args.image.read_bytes())
    roots = [int(r, 16) for r in args.root]
    if args.candidates:
        suspects = set()
        for entry, body in corpus.functions.items():
            if '// ERROR ' in body or '\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n\treturn;' in body:
                suspects.add(entry)
        # A suspect that is itself an analyzer artifact belongs to the nearest genuine entry before it.
        for s in sorted(suspects):
            if find_dispatch(corpus.image, s - 4):
                continue  # the entry is another dispatch's inline table: data, never a root
            root = s
            while True:
                i = corpus.entries.index(root)
                if i == 0 or corpus.genuine(root, corpus.interval(root)):
                    break
                root = corpus.entries[i - 1]
            if not find_dispatch(corpus.image, root - 4):
                roots.append(root)
    results, patched = [], {}
    for root in sorted(set(roots)):
        borrowed_before = corpus.borrowed
        try:
            plan, body = rebuild(corpus, root)
        except Reject as error:
            results.append(dict(root=f'{root:08X}', status='rejected', reason=str(error)))
            continue
        path = corpus.chunk_of[root]
        text = patched.get(path) or path.read_text(encoding='utf-8')
        old = extract_function(text, root, allow_indirect=True)
        if text.count(old) != 1:
            results.append(dict(root=f'{root:08X}', status='rejected', reason='definition not unique in chunk'))
            continue
        patched[path] = text.replace(old, body.rstrip('\n'))
        results.append(dict(root=f'{root:08X}', status='rebuilt', chunk=path.name, instructions=len(plan.instructions),
                            borrowed=corpus.borrowed - borrowed_before,
                            dispatches=len(plan.dispatches), pieces=[f'{p:08X}' for p in plan.pieces],
                            tail_calls=sorted(f'{t:08X}' for t in plan.tail_calls)))
        if args.show:
            print(body)
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        for path, text in patched.items():
            (args.out / path.name).write_text(text, encoding='utf-8', newline='\n')
    summary = dict(rebuilt=sum(r['status'] == 'rebuilt' for r in results),
                   rejected=sum(r['status'] == 'rejected' for r in results),
                   chunks=sorted(p.name for p in patched))
    if args.report:
        args.report.write_text(json.dumps(dict(summary=summary, results=results), indent=1) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps(summary))
    for r in results:
        if r['status'] == 'rejected':
            print(f"rejected {r['root']}: {r['reason']}")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
