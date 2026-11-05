#!/usr/bin/env python3
"""loop_locals.py IN.cpp OUT.cpp SPEC[,SPEC...] --twin FLAG [--mutant M]: loop-scoped guest
registers as C++ locals (job-code package, 2026-10-04), the region form of tools/fp_codegen/leaf_locals.py.

SPEC = FUNC@LABEL: the call-free region of generated function FUNC from the line `LABEL:` to the last line of its body that
jumps back to LABEL (`goto LABEL;`), i.e. one loop. Inside the region every guest register it names (rN, fN, vN, crN, ctr, xer)
is a local: copied from ctx when the region is entered from outside (fall-through into LABEL, or a jump to LABEL from outside,
which is redirected to a new label LABEL_jcl_in placed before the copy-in), and written back to ctx on every edge that leaves
the region (the fall-through after the last back-edge line, and every `goto X;` inside the region whose target X lies outside
it, rewritten to `{ write-back; goto X; }`). Registers the region provably only reads (syntactic rule: every occurrence is a
field read l_X.f... that is neither assigned, incremented, passed by address/whole nor the destination of a store intrinsic)
are not written back (they still equal ctx); everything else is. Locals are declared uninitialised at the top of the body, so
no goto crosses an initialisation. The region must contain no call, indirect call, trap, return, switch or other use of ctx
(fpscr/lr/msr/reserved stay in ctx), and no label in it other than LABEL may be the target of a jump from outside; otherwise
the SPEC is refused (reported) and the function left unchanged. Nothing outside the region changes, so ctx is exact at every
call, return and trap of the function and at every point outside the loop; inside the loop nothing else can observe ctx (no
callee; the guest-memory slow paths take base and an address only).

--twin FLAG keeps each original body and adds the rewritten one as static noinline jcl_sub_FUNC, chosen at entry (a tail
call before PPC_FUNC_PROLOGUE) while the extern "C" std::atomic<unsigned> FLAG is non-zero (in-run A/B; the original body
then loses its PGO record: its CFG gained the dispatch branch). transform() is the pure text -> text function the recipe
pass loop_locals_pass.py calls. (The earlier --fn-twin whole-function option was removed: whole-function locals cost 2x on
the early-out calls that dominate those functions.) --mutant (tests only): drop-writeback (the first written register is left out of every write-back), no-copy-in (the first register is not
copied in), exit-no-wb (side exits leave without write-back), ro-as-written-off (every register treated as read-only).
Game-derived sources stay under local-analysis/; this tool contains no game code."""
from __future__ import annotations
import re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ll_text  # noqa: E402  (sibling: REG / OTHER_CTX / functions() of leaf_locals.py, copied)

REG = ll_text.REG
OTHER_CTX = ll_text.OTHER_CTX
BAD = re.compile(r'\b(?:__imp__)?sub_[0-9A-F]{8}\(ctx|PPC_CALL_INDIRECT_FUNC|PPC_RUNTIME_TRAP|__savegpr|__restgpr|__savefpr|'
                 r'__restfpr|__savevmx|__restvmx|\breturn\b|\bswitch\b|goto\s*\*|\(ctx\s*,|\bctx\s*\)')
LABEL_LINE = re.compile(r'^(loc_[0-9A-Fa-f]+):\s*$')
GOTO = re.compile(r'\bgoto\s+(loc_[0-9A-Fa-f]+)\s*;')
LOADS = ('simde_mm_load_si128(', 'simde_mm_load_ps(', 'simde_mm_loadu_si128(', 'simde_mm_loadu_ps(', 'simde_mm_load_pd(',
         'simde_mm_loadu_pd(', 'simde_mm_load_ss(', 'simde_mm_load_sd(', 'simde_mm_loadl_epi64(')


def code_of(line: str) -> str:
    return line.split('//', 1)[0]


def enclosing_call(pre: str) -> str:
    """The identifier of the innermost call whose argument list is open at the end of pre ('' if none)."""
    depth = 0
    for k in range(len(pre) - 1, -1, -1):
        ch = pre[k]
        if ch == ')':
            depth += 1
        elif ch == '(':
            if depth == 0:
                m = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*$', pre[:k])
                if m:
                    return m.group(1) + '('
                depth = 0  # a parenthesised cast / grouping: keep looking outward
            else:
                depth -= 1
    return ''


def written_regs(lines: list[str], regs: list[str]) -> set[str]:
    """Registers with any occurrence in the (already substituted) region lines that is not a provable read. A vector register
    (an array member: it decays to a pointer) is read only where it is directly inside a known SIMDe load intrinsic; any other
    occurrence (a store intrinsic's destination, a memcpy, an unknown helper) counts as a write."""
    out = set()
    for r in regs:
        pat = re.compile(r'\bl_' + re.escape(r) + r'\b')
        for line in lines:
            code = code_of(line)
            for m in pat.finditer(code):
                pre, post = code[:m.start()], code[m.end():]
                acc = re.match(r'((?:\.\w+|\[[^\]]*\])+)', post)
                if not acc:  # the whole register object (by reference, e.g. the xer argument of compare<>): maybe written
                    out.add(r); break
                rest = post[acc.end():]
                if r.startswith('v') and not acc.group(1).endswith(']'):
                    # l_vN.u8 / .f32 ...: the array member decays to a pointer (l_vN.u32[k] is an element: scalar rules)
                    if enclosing_call(pre) not in LOADS:
                        out.add(r); break
                    continue
                if re.match(r'\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)', rest) or re.match(r'\s*(\+\+|--)', rest) or re.search(r'(\+\+|--)\s*$', pre):
                    out.add(r); break
                if re.match(r'\s*[(<]', rest):  # a method call on the register (CR compare / setFromMask ...)
                    out.add(r); break
                if re.search(r'&\s*$', pre):
                    out.add(r); break
                if re.search(r'\*\)\s*$', pre) or re.search(r'\*\s*\)\s*$', pre):  # (T*)l_X.f: a pointer: load or store?
                    call = re.search(r'([A-Za-z_][A-Za-z0-9_]*\()\s*\([^()]*\*\s*\)\s*$', pre)
                    if not call or call.group(1) not in LOADS:
                        out.add(r); break
            if r in out:
                break
    return out


def rewrite_region(body: list[str], label: str, mutant: str = '') -> tuple[list[str] | None, str]:
    """(new body lines, '') or (None, reason). body = the lines between the opening line and the closing brace."""
    try:
        h = next(i for i, l in enumerate(body) if l.strip() == f'{label}:')
    except StopIteration:
        return None, f'no label {label}'
    backs = [i for i, l in enumerate(body) if i > h and re.search(r'\bgoto\s+' + label + r'\s*;', code_of(l))]
    if not backs:
        return None, f'no back edge to {label}'
    e = backs[-1]
    region = body[h:e + 1]
    inner_labels = {m.group(1) for l in region for m in [LABEL_LINE.match(l.strip())] if m}
    for i, l in enumerate(body):
        if h <= i <= e:
            continue
        for m in GOTO.finditer(code_of(l)):
            if m.group(1) in inner_labels and m.group(1) != label:
                return None, f'jump from outside into {m.group(1)}'
    if any('loop_locals' in l for l in region):
        return None, 'overlaps a region already rewritten'
    for l in region:
        c = code_of(l)
        if BAD.search(c):
            return None, f'region has a call/return/trap/switch: {c.strip()[:60]}'
        if OTHER_CTX.search(c):
            return None, f'other ctx use: {c.strip()[:60]}'
    regs: list[str] = []
    for l in region:
        for m in REG.finditer(code_of(l)):
            if m.group(1) not in regs:
                regs.append(m.group(1))
    if not regs:
        return None, 'no registers'
    sub = []
    for l in region:
        c, sep, com = l.partition('//')
        sub.append(REG.sub(lambda m: 'l_' + m.group(1), c) + sep + com)
    wr = written_regs(sub, regs) if mutant != 'ro-as-written-off' else set()
    wlist = [r for r in regs if r in wr]
    if mutant == 'drop-writeback' and wlist:
        wlist = wlist[1:]
    wb = ''.join(f'ctx.{r}=l_{r};' for r in wlist)
    cin = [r for r in regs]
    if mutant == 'no-copy-in':  # a register the region only reads (its copy-in is live), else the first one
        ro = [r for r in regs if r not in wr]
        cin.remove(ro[0] if ro else cin[0])
    copy_in = ''.join(f'l_{r}=ctx.{r};' for r in cin)
    out = []
    # declarations at the top of the body (before any label: no jump crosses them); a second region of the same body
    # declares only registers the first did not
    have = set(re.findall(r'decltype\(ctx\.\w+\) l_(\w+);', ''.join(l for l in body if '// loop_locals loc_' in l)))
    new = [r for r in regs if r not in have]
    if new:
        out.append('\t' + ''.join(f'decltype(ctx.{r}) l_{r};' for r in new) + f' // loop_locals {label}\n')
    entry = f'{label}_jcl_in'
    for i, l in enumerate(body):
        if i == h:
            out.append(f'{entry}:\n')
            out.append('\t{' + copy_in + '} // loop_locals copy-in\n')
            out.append(l)
            continue
        if h < i <= e:
            c, sep, com = sub[i - h].partition('//')

            def exit_goto(m):
                if m.group(1) in inner_labels:
                    return m.group(0)
                return m.group(0) if mutant == 'exit-no-wb' else '{' + wb + 'goto ' + m.group(1) + ';}'
            c = GOTO.sub(exit_goto, c)
            out.append(c + sep + com if sep else c.rstrip('\n') + '\n')
            if i == e:
                out.append('\t{' + wb + '} // loop_locals write-back (fall-through exit)\n')
            continue
        # outside the region: jumps to the loop head enter through the copy-in
        c, sep, com = l.partition('//')
        c2 = re.sub(r'\bgoto\s+' + label + r'\s*;', f'goto {entry};', c)
        out.append(c2 + sep + com)
    return out, ''


def transform(text: str, loops: dict[str, list[str]], twin: str, mutant: str = '') -> tuple[str, list[str]]:
    """(new chunk text, report lines): every function named in loops gets its listed regions rewritten into a static
    noinline jcl_sub_ twin dispatched at entry on the atomic flag twin; the flag declaration goes after the first #include
    (only when some function changed). Pure text -> text."""
    lines = text.splitlines(keepends=True)
    out, prev, report, done = [], 0, [], 0
    for i, j, name in ll_text.functions(lines):
        out.extend(lines[prev:i]); prev = j + 1
        body = lines[i + 1:j]
        new_loop = None
        if name in loops and lines[i].startswith('PPC_FUNC_IMPL('):
            cur = body
            for lab in loops[name]:
                nb, why = rewrite_region(cur, lab, mutant)
                if nb is None:
                    report.append(f'{name}@{lab}: refused ({why})'); continue
                cur = nb; report.append(f'{name}@{lab}: rewritten')
            if cur is not body:
                new_loop = cur
        if new_loop is None:
            out.extend(lines[i:j + 1]); continue
        done += 1
        out.append(f'static __attribute__((noinline)) void jcl_sub_{name}(PPCContext& __restrict ctx, uint8_t* base) {{\n')
        out.extend(new_loop); out.append('}\n')
        out.append(lines[i])
        out.append(f'\tif ({twin}.load(std::memory_order_relaxed)) {{ jcl_sub_{name}(ctx, base); return; }} // loop_locals twin\n')
        out.extend(body); out.append(lines[j])
    out.extend(lines[prev:])
    if done:
        k = next(n for n, l in enumerate(out) if l.startswith('#include')) + 1
        # hidden: defined in the same library (generic_renderer.cpp), so the dispatch addresses it with adrp+add (no GOT)
        out.insert(k, f'#include <atomic>\nextern "C" {{ __attribute__((visibility("hidden"))) extern std::atomic<unsigned> {twin}; }}'
                      ' // loop_locals twin switch\n')
    return ''.join(out), report


def main() -> None:
    a = sys.argv[1:]
    mutant = a[a.index('--mutant') + 1] if '--mutant' in a else ''
    twin = a[a.index('--twin') + 1] if '--twin' in a else ''
    pos = [x for i, x in enumerate(a) if x not in ('--mutant', '--twin') and (i == 0 or a[i - 1] not in ('--mutant', '--twin'))]
    src, dst, specs = pos[0], pos[1], pos[2]
    if not twin:
        raise SystemExit('--twin FLAG is required (in-run A/B form only)')
    loops: dict[str, list[str]] = {}
    for s_ in specs.split(','):
        if s_:
            f, lab = s_.split('@'); loops.setdefault(f, []).append(lab)
    text, report = transform(Path(src).read_text(), loops, twin, mutant)
    Path(dst).write_text(text, encoding='utf-8', newline='\n')
    print('\n'.join(report))


if __name__ == '__main__':
    main()
