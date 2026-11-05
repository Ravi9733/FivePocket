#!/usr/bin/env python3
"""leaf_locals.py IN.cpp OUT.cpp FUNC[,FUNC...] | --all-leaves | --all [--calls] [--twin FLAG]: rewrites the named generated PPC functions (or every leaf: no call of
any guest function, save/restore helper or indirect target) so that their guest registers (rN, fN, vN, crN, ctr, xer) are function
locals: each register the body uses is copied from ctx after PPC_FUNC_PROLOGUE() and written back to ctx before every return and
at the closing brace. Inside a leaf nothing else can observe ctx between entry and exit (no callee; the prologue checkpoint and
the memory-access slow paths do not read guest registers), so the final ctx and every guest memory access are unchanged while
the compiler may keep loop-carried registers in host registers (a store->load round trip through ctx per iteration before).
Other ctx fields (fpscr, lr, msr, reserved...) stay in ctx. A body with any other use of `ctx` (passed by name, taken by address)
is left unchanged and reported. --calls extends the rewrite to whole functions (rewrite(): ctx is synchronised around every call,
indirect call and trap; --all = every eligible function). --twin FLAG keeps each original body and adds the rewritten one as
a separate function chosen at entry while the extern "C" std::atomic<unsigned> FLAG is non-zero (in-run A/B). tools/fp_codegen/leaf_locals_check.py is the differential test.
Game-derived sources stay under local-analysis/; this tool contains no game code."""
from __future__ import annotations
import re, sys
from pathlib import Path

START = re.compile(r'^(PPC_FUNC_IMPL|PPC_FUNC)\((.*)\)\s*\{\s*$')
REG = re.compile(r'\bctx\.(r[0-9]{1,2}|f[0-9]{1,2}|v[0-9]{1,3}|cr[0-7]|ctr|xer)\b')
CALL = re.compile(r'\b(?:__imp__)?sub_[0-9A-F]{8}\(ctx,\s*base\)|PPC_CALL_INDIRECT_FUNC|__savegprlr_|__restgprlr_|__savefpr_|__restfpr_|__savevmx_|__restvmx_')
OTHER_CTX = re.compile(r'\bctx\b(?!\.(?:r[0-9]{1,2}|f[0-9]{1,2}|v[0-9]{1,3}|cr[0-7]|ctr|xer|fpscr|lr|msr|reserved)\b)')


# --calls: a call statement (a whole line `[else ]NAME(ctx, base);`), an indirect call or a trap is a synchronisation point
SYNC_CALL = re.compile(r'^(\s*)(else\s+)?((?:__imp__)?[A-Za-z_][A-Za-z0-9_]*)\(ctx, ?base\);\s*$')
SYNC_MACRO = re.compile(r'\bPPC_(?:CALL_INDIRECT_FUNC|RUNTIME_TRAP)\(')
INDIRECT_STMT = re.compile(r'^(\s*)PPC_CALL_INDIRECT_FUNC\(([^;]*)\);(\s*return;)?\s*$')
TRAP_STMT = re.compile(r'^(\s*)(if \(.*\) )?PPC_RUNTIME_TRAP\(\);\s*$')


def rewrite(body: list[str], mutant: str = '', calls: bool = False) -> list[str] | None:
    """The rewritten body lines (without the closing brace), or None when the body is not eligible. calls=False: leaves only
    (any call, indirect call or save/restore helper makes the body ineligible). calls=True (whole functions): every used
    register is also written back to ctx before each call statement, indirect call and trap, and reloaded from ctx after it,
    so ctx is exact wherever anything else can read or write it. mutant (checker tests only): 'drop-writeback' leaves the
    first register out of every write-back, 'no-copy-in' starts the first register from a zero value instead of ctx's,
    'no-reload' leaves it out of the reload after a call."""
    regs: list[str] = []
    for line in body:
        code = line.split('//', 1)[0]
        sync = calls and (SYNC_CALL.match(code) or SYNC_MACRO.search(code))
        if not calls and CALL.search(code):
            return None
        if not sync and OTHER_CTX.search(code.replace('PPC_FUNC_PROLOGUE()', '')):
            return None
        for m in REG.finditer(code):
            if m.group(1) not in regs: regs.append(m.group(1))
    if not regs: return None
    wb = ''.join(f'ctx.{r}=l_{r};' for r in regs[(mutant == 'drop-writeback'):])
    rl = ''.join(f'l_{r}=ctx.{r};' for r in regs[(mutant == 'no-reload'):])
    out = []
    if not any('PPC_FUNC_PROLOGUE();' in l for l in body): return None
    prologue = False
    for line in body:
        if not prologue and 'PPC_FUNC_PROLOGUE();' not in line:
            out.append(line)  # a repair's statement before the prologue reads ctx itself (the locals start after it)
            continue
        if 'PPC_FUNC_PROLOGUE();' in line:
            prologue = True
            out.append(line)
            init = [f'auto l_{r}=ctx.{r};' for r in regs]
            if mutant == 'no-copy-in':  # r3 (an argument: read before written) when used, else the first register
                k = regs.index('r3') if 'r3' in regs else 0
                init[k] = f'decltype(ctx.{regs[k]}) l_{regs[k]}{{}};'
            out.append('\t' + ''.join(init) + ' // leaf_locals\n')
            continue
        code, sep, comment = line.partition('//')
        m = SYNC_CALL.match(code) if calls else None
        if m:
            out.append(f'{m.group(1)}{m.group(2) or ""}{{{wb}{m.group(3)}(ctx, base);{rl}}}' + (' //' + comment if sep else '\n'))
            continue
        code = REG.sub(lambda m: 'l_' + m.group(1), code)
        tail = '\n' if not sep else ' '
        if calls and SYNC_MACRO.search(code):
            m = INDIRECT_STMT.match(code) or TRAP_STMT.match(code)
            if not m:
                return None  # a call or trap inside another statement: left unchanged
            # the call is wrapped (write back, call, reload); a `return;` after it on the same line writes back the
            # reloaded values (never the stale locals)
            if m.re is INDIRECT_STMT:
                code = f'{m.group(1)}{{{wb}PPC_CALL_INDIRECT_FUNC({m.group(2)});{rl}}}' + ('{' + wb + 'return;}' if m.group(3) else '') + tail
            else:
                code = f'{m.group(1)}{m.group(2) or ""}{{{wb}PPC_RUNTIME_TRAP();}}' + tail
            out.append(code + sep + comment)
            continue
        code = re.sub(r'\breturn;', '{' + wb + 'return;}', code)
        out.append(code + sep + comment)
    # the closing brace: a body that can fall off its end writes back first
    last = next((l for l in reversed(out) if l.strip()), '')
    if 'return;' not in last:
        out.append('\t' + wb + ' // leaf_locals (end)\n')
    return out


def is_leaf(body: list[str]) -> bool:
    return not any(CALL.search(l.split('//', 1)[0]) for l in body)


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


def main() -> None:
    argv = sys.argv[1:]
    calls = '--calls' in argv
    twin = argv[argv.index('--twin') + 1] if '--twin' in argv else ''
    args = [x for i, x in enumerate(argv) if x != '--calls' and x != '--twin' and (i == 0 or argv[i - 1] != '--twin')]
    src, dst, which = args[0], args[1], args[2]
    names = None if which in ('--all-leaves', '--all') else set(which.split(','))
    lines = Path(src).read_text(encoding='utf-8').splitlines(keepends=True)
    out, prev, done, skipped = [], 0, 0, []
    for i, j, name in functions(lines):
        out.extend(lines[prev:i]); prev = j + 1
        body = lines[i + 1:j]
        want = (names is None and (calls or is_leaf(body))) or (names is not None and name in names)
        new = rewrite(body, calls=calls) if want else None
        if want and new is None: skipped.append(name)
        if new is None:
            out.extend(lines[i:j + 1]); continue
        done += 1
        if twin and name and lines[i].startswith('PPC_FUNC_IMPL('):
            # --twin FLAG (in-run A/B): the rewritten body becomes a separate noinline function; the original entry runs it
            # (a tail call) while FLAG is non-zero, else its own unchanged body
            out.append(f'static __attribute__((noinline)) void llx_sub_{name}(PPCContext& __restrict ctx, uint8_t* base) {{\n')
            out.extend(new); out.append('}\n')
            out.append(lines[i])
            out.append(f'\tif ({twin}.load(std::memory_order_relaxed)) {{ llx_sub_{name}(ctx, base); return; }} // leaf_locals twin\n')
            out.extend(body); out.append(lines[j])
            continue
        out.append(lines[i]); out.extend(new); out.append(lines[j])
    out.extend(lines[prev:])
    if twin and done:
        k = next(n for n, l in enumerate(out) if l.startswith('#include')) + 1
        out.insert(k, f'#include <atomic>\nextern "C" std::atomic<unsigned> {twin}; // leaf_locals twin switch\n')
    Path(dst).write_text(''.join(out), encoding='utf-8', newline='\n')
    print(f'{done} functions rewritten; skipped {len(skipped)}' + (f' ({",".join(skipped[:8])})' if skipped else ''))


if __name__ == '__main__':
    main()
