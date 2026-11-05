#!/usr/bin/env python3
"""Recover bounded compiler switch idioms inside explicit PPC function ranges."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import struct
import subprocess


MAX_IMAGE_BYTES = 512 * 1024 * 1024
MAX_FUNCTION_BYTES = 1024 * 1024
MAX_CASES = 4096
MAX_GUARD_INSTRUCTIONS = 4
MAX_PRE_GUARD_INSTRUCTIONS = 8
MAX_EXTENDED_GUARD_INSTRUCTIONS = 64
MAX_EXTENDED_POST_GUARD_INSTRUCTIONS = 16
MAX_FUNCTION_MAPPINGS = 131072
MAX_MAPPING_BYTES = 16 * 1024 * 1024


def function_ranges_from_map(text: str, code_base: int, code_size: int) -> list[tuple[int, int]]:
    """Conservative intervals between validated adjacent function entries.

    An overlapping function whose targets exceed its next entry will fail the
    existing local-target checks. No widening/merging or inferred entry is used.
    """
    if len(text) > MAX_MAPPING_BYTES or not (0 <= code_base < 1 << 32 and code_base % 4 == 0
            and 0 < code_size <= MAX_IMAGE_BYTES and code_size % 4 == 0 and code_base + code_size <= 1 << 32):
        raise ValueError("invalid function-map code range or source size")
    declaration = "PPCFuncMapping PPCFuncMappings[] = {"
    if text.count(declaration) != 1:
        raise ValueError("function map must have one canonical declaration")
    body = text.split(declaration, 1)[1]
    if body.count("};") != 1 or body.split("};", 1)[1].strip():
        raise ValueError("function map terminator is not canonical")
    lines = [line.strip() for line in body.split("};", 1)[0].splitlines() if line.strip()]
    if not lines or not re.fullmatch(r"\{\s*0,\s*nullptr\s*\}", lines[-1]):
        raise ValueError("function map requires a final null sentinel")
    if not 1 <= len(lines) - 1 <= MAX_FUNCTION_MAPPINGS:
        raise ValueError("function map entry count is outside bounds")
    addresses = []
    for line in lines[:-1]:
        match = re.fullmatch(r"\{\s*(0x[0-9A-Fa-f]+),\s*([A-Za-z_][A-Za-z0-9_]*)\s*\},", line)
        if not match or match[2] in {"nullptr", "NULL"}:
            raise ValueError("function map entry is not canonical")
        address = int(match[1], 16)
        if not (code_base <= address < code_base + code_size and address % 4 == 0) \
                or (addresses and address <= addresses[-1]):
            raise ValueError("function map entries must be aligned, increasing and within code")
        addresses.append(address)
    ranges = [(base, end - base) for base, end in zip(addresses, addresses[1:] + [code_base + code_size])]
    if any(size > MAX_FUNCTION_BYTES for _, size in ranges):
        raise ValueError("a function-map interval exceeds the local scan bound")
    return ranges


def function_words(image: bytes, image_base: int, function_base: int, function_size: int):
    if not (0 <= image_base < 1 << 32 and 0 < len(image) <= MAX_IMAGE_BYTES
            and image_base + len(image) <= 1 << 32
            and function_base % 4 == 0 and function_size % 4 == 0
            and 0 < function_size <= MAX_FUNCTION_BYTES
            and image_base <= function_base
            and function_base + function_size <= image_base + len(image)):
        raise ValueError("invalid bounded image/function range")
    return struct.unpack_from(f">{function_size // 4}I", image, function_base - image_base)


@dataclass(frozen=True)
class SwitchTable:
    base: int
    selector: int
    default: int
    labels: tuple[int, ...]


def signed16(value: int) -> int:
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def discover_u16_switches(
    image: bytes, image_base: int, function_base: int, function_size: int
) -> list[SwitchTable]:
    """Accept only the complete idiom with verified register flow and local targets.

    cmplwi/bgt guard; lis; addi/rlwinm in either order; lhzx; lis;
    addi; optional nop; add; mtctr; bctr. No arbitrary backtracking,
    wildcard instructions, external targets, or partial metadata are accepted.
    """
    words = function_words(image, image_base, function_base, function_size)
    found: list[SwitchTable] = []
    op = lambda w: w >> 26
    rt = lambda w: (w >> 21) & 31
    ra = lambda w: (w >> 16) & 31
    rb = lambda w: (w >> 11) & 31
    xo = lambda w: (w >> 1) & 1023
    local = lambda address: function_base <= address < function_base + function_size and address % 4 == 0
    for index in range(2, len(words) - 8):
        first = words[index]
        if op(first) != 15 or ra(first) != 0 or rt(first) == 0:
            continue
        table_reg = rt(first)
        a, b, load, high, low = words[index + 1:index + 6]
        if op(a) == 21 and op(b) == 14:
            shift, addi = a, b
        elif op(a) == 14 and op(b) == 21:
            addi, shift = a, b
        else:
            continue
        selector, offset_reg = rt(shift), ra(shift)
        if (rt(addi) != table_reg or ra(addi) != table_reg
                or rb(shift) != 1 or ((shift >> 6) & 31) != 0
                or ((shift >> 1) & 31) != 30 or shift & 1
                or len({selector, offset_reg, table_reg}) != 3
                or op(load) != 31 or xo(load) != 279 or load & 1
                or rt(load) != offset_reg or ra(load) != table_reg or rb(load) != offset_reg
                or op(high) != 15 or rt(high) != table_reg or ra(high) != 0
                or op(low) != 14 or rt(low) != table_reg or ra(low) != table_reg):
            continue
        cursor = index + 6
        if words[cursor] == 0x60000000:
            cursor += 1
        if cursor + 2 >= len(words):
            continue
        add, mtctr, branch = words[cursor:cursor + 3]
        if (op(add) != 31 or xo(add) != 266 or add & 1
                or rt(add) != table_reg or ra(add) != table_reg or rb(add) != offset_reg
                or op(mtctr) != 31 or xo(mtctr) != 467 or mtctr & 1
                or rt(mtctr) != table_reg or ra(mtctr) != 9 or rb(mtctr) != 0
                or branch != 0x4E800420):
            continue
        compare, default_branch = words[index - 2:index]
        cr = (compare >> 23) & 7
        if (op(compare) != 10 or compare & 0x00600000 or ra(compare) != selector
                or op(default_branch) != 16 or rt(default_branch) != 12
                or ra(default_branch) != cr * 4 + 1 or default_branch & 3):
            continue
        cases = (compare & 0xFFFF) + 1
        if cases > MAX_CASES:
            continue
        default = function_base + (index - 1) * 4 + signed16(default_branch & 0xFFFC)
        table_address = (((first & 0xFFFF) << 16) + signed16(addi)) & 0xFFFFFFFF
        target_base = (((high & 0xFFFF) << 16) + signed16(low)) & 0xFFFFFFFF
        table_offset = table_address - image_base
        if (not local(default) or table_address % 2 != 0 or table_offset < 0
                or table_offset + cases * 2 > len(image)):
            continue
        offsets = struct.unpack_from(f">{cases}H", image, table_offset)
        labels = tuple(target_base + offset for offset in offsets)
        if not all(local(label) for label in labels):
            continue
        found.append(SwitchTable(function_base + index * 4, selector, default, labels))
    return found


def _selector_copy_before_compare(words, compare_index: int, compared: int, selector: int,
                                  *, extended: bool = False) -> bool:
    """Prove a nearby copy without crossing a call or an unknown GPR write.

    Compiler range checks may compare the original argument after copying it
    to a nonvolatile register. Only register copies, integer comparisons, NOPs
    and non-link CR-only branches may intervene. Live CTR checks remain required.
    """
    if compared == selector:
        return True
    for index in range(compare_index - 1, max(-1, compare_index - 17), -1):
        word = words[index]
        opcode = word >> 26
        source, destination, right = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
        if opcode == 31 and ((word >> 1) & 1023) == 444 and not word & 1 and source == right:
            if {source, destination} == {compared, selector}:
                return True
            if destination in {compared, selector}:
                return False
        elif opcode in {10, 11} or word == 0x60000000:
            continue
        elif extended and opcode in {14, 15} and destination == 0:
            # Independent li/lis does not invalidate an earlier selector copy.
            # Source is RT in these D forms, NOT the OR-form RS above.
            if source in {compared, selector}:
                return False
        elif opcode == 16 and source in {4, 12} and not word & 3:
            continue
        else:
            return False
    return False


def extended_guard_preserved(word: int) -> bool:
    """Exact extra non-GPR/non-CR-writing opcodes for opt-in long guards.

    Masks/operands come from pinned XenonRecomp ppc-dis.c. VX128 register
    extension bits are NOT scalar Rc. Only the non-recording GE vector compare
    is admitted (its record bit is 0x40); all other compares remain rejected.
    This proves register preservation only, not vector arithmetic fidelity.
    """
    if word >> 26 in {38, 44}:  # stb/sth, NOT updating 39/45
        return True
    if word & 0xFC0007F3 in {0x100000C3, 0x10000403}:  # lvx128/lvlx128, no base update
        return True
    if word & 0xFC0003D0 in {
            0x14000010,  # vaddfp128
            0x14000050,  # vsubfp128
            0x14000090,  # vmulfp128
            0x140000D0,  # vmaddfp128
            0x14000150,  # vnmsubfp128
            0x14000190,  # vmsum3fp128
            0x14000210,  # vand128
            0x140002D0,  # vor128
            0x18000080,  # vcmpgefp128 (NOT its CR6-writing record form)
            0x18000280,  # vmaxfp128
            0x180002C0}:  # vminfp128
        return True
    # Unary encodings also require the unused VA field to be zero.
    if word & 0xFC1F07F0 in {0x18000630, 0x18000670}:  # vrefp128 / vrsqrtefp128
        return True
    if word & 0xFC0007F0 == 0x18000730 and (word >> 16) & 31 <= 3:  # vspltw128, valid lane
        return True
    if word & 0xFC0007FF == (31 << 26) | (151 << 1):  # non-updating stwx
        return True
    if word & 0xFC00003F in {0x1000002E, 0x1000002F}:  # vmaddfp / vnmsubfp
        return True
    return word & 0xFC0007FF == 0x10000484 or word & 0xFC1F07FF == 0x1000010A  # vor / vrefp


def discover_u32_switches(
    image: bytes, image_base: int, function_base: int, function_size: int,
    *, extended_guards: bool = False
) -> list[SwitchTable]:
    """Guarded absolute word targets: lis; addi/rlwinm; lwzx; mtctr; bctr.

    Accept either address/index order, with exact register flow. Up to eight
    independent addi, non-updating stw/lwz/lhz/lfs/stfs or non-recording slw/mr/fmr
    and exact fmuls/fsubs/fsel or vmaddcfp128/stvx128/vsubfp instructions
    may separate cmplwi/bgt;
    they preserve CR and the selector, and generated side effects are retained.
    Up to four independent li/lis instructions may follow bgt before lis. They
    preserve the selector/CR too; their generated assignments are not removed.
    A nearby proven register copy may separate the compared and index registers.
    Live CTR validation is required in the generator.
    Opt-in extended_guards permits at most 64 instructions and the additional
    exact operations in extended_guard_preserved, still no selector/CR clobbers.
    Reject targets
    outside the supplied function, into table data, or into the dispatch sequence.
    """
    words = function_words(image, image_base, function_base, function_size)
    found = []
    op = lambda w: w >> 26
    rt = lambda w: (w >> 21) & 31
    ra = lambda w: (w >> 16) & 31
    rb = lambda w: (w >> 11) & 31
    xo = lambda w: (w >> 1) & 1023
    for index in range(2, len(words) - 5):
        high, a, b, load, mtctr, branch = words[index:index + 6]
        if op(high) != 15 or ra(high) != 0 or rt(high) == 0:
            continue
        if op(a) == 14 and op(b) == 21:
            low, shift = a, b
        elif op(a) == 21 and op(b) == 14:
            shift, low = a, b
        else:
            continue
        table, selector, offset = rt(high), rt(shift), ra(shift)
        if (rt(low) != table or ra(low) != table
                or rb(shift) != 2 or ((shift >> 6) & 31) != 0
                or ((shift >> 1) & 31) != 29 or shift & 1
                or len({table, selector, offset}) != 3
                or op(load) != 31 or xo(load) != 23 or load & 1
                or rt(load) != offset or ra(load) != table or rb(load) != offset
                or op(mtctr) != 31 or xo(mtctr) != 467 or mtctr & 1
                or rt(mtctr) != offset or ra(mtctr) != 9 or rb(mtctr) != 0
                or branch != 0x4E800420):
            continue
        guard_index = index - 1
        after_guard = 0
        post_guard_limit = MAX_EXTENDED_POST_GUARD_INSTRUCTIONS if extended_guards else MAX_GUARD_INSTRUCTIONS
        while guard_index > 0 and after_guard < post_guard_limit:
            word = words[guard_index]
            # An lfs writes only an FPR/FPSCR; neither selector GPR nor guard
            # CR changes. Preserve its actual load rather than moving it.
            if op(word) != 48 and (op(word) not in {14, 15}
                    or (ra(word) != 0 and not extended_guards) or rt(word) == selector):
                break
            guard_index -= 1
            after_guard += 1
        guard = words[guard_index]
        guard_start = guard_index - 1
        if guard_start < 0:
            continue
        intervening = 0
        def preserves_guard(word):
            if extended_guards and extended_guard_preserved(word):
                return True
            if extended_guards and op(word) == 34:  # lbz, no base update
                return rt(word) != selector
            if extended_guards and not word & 1 and ra(word) != selector:
                if op(word) == 21 or op(word) == 31 and xo(word) in {28, 60}:  # rlwinm/and/andc
                    return True
            # Exact VX/VX128/VX128_1 encodings from pinned XenonRecomp's
            # ppc-dis.c. These write vector registers or memory, not GPRs/CR.
            # VX128 low bits include register extensions, NOT a scalar Rc bit.
            # Keep stores in original order; live CTR pairing detects changes
            # to the subsequently loaded table. Never admit vector compares.
            if (word & 0xFC0003D0 == 0x14000110  # vmaddcfp128
                    or word & 0xFC0007F3 == 0x100001C3  # stvx128
                    or word & 0xFC0007FF == 0x1000004A):  # vsubfp
                return True
            if op(word) in {36, 48, 52}:  # stw/lfs/stfs: no GPR update or CR write
                return True
            if op(word) in {14, 32, 40}:  # addi/lwz/lhz, no update or CR write
                return rt(word) != selector
            if op(word) == 63 and xo(word) == 72:  # exact non-recording fmr
                return ra(word) == 0 and not word & 1
            if not word & 1:
                # A-form FP arithmetic writes only FPR/FPSCR, not the guard CR
                # or a GPR. Check unused fields; do not admit comparisons.
                if op(word) == 59 and ((word >> 1) & 31) == 25:  # fmuls
                    return rb(word) == 0
                if op(word) == 59 and xo(word) == 20:  # fsubs, FRC=0
                    return True
                if op(word) == 63 and ((word >> 1) & 31) == 23:  # fsel
                    return True
            return (op(word) == 31 and not word & 1 and ra(word) != selector
                    and (xo(word) == 24 or (xo(word) == 444 and rt(word) == rb(word))))
        guard_limit = MAX_EXTENDED_GUARD_INSTRUCTIONS if extended_guards else MAX_PRE_GUARD_INSTRUCTIONS
        while guard_start > 0 and preserves_guard(words[guard_start]) and intervening < guard_limit:
            # No CR/selector changes; table mutation is not assumed away.
            guard_start -= 1
            intervening += 1
        compare = words[guard_start]
        cr = (compare >> 23) & 7
        if (op(compare) != 10 or compare & 0x00600000
                or not _selector_copy_before_compare(words, guard_start, ra(compare), selector,
                                                     extended=extended_guards)
                or op(guard) != 16 or rt(guard) != 12 or ra(guard) != cr * 4 + 1
                or guard & 3):
            continue
        cases = (compare & 0xFFFF) + 1
        if cases > MAX_CASES:
            continue
        table_address = (((high & 0xFFFF) << 16) + signed16(low)) & 0xFFFFFFFF
        table_offset = table_address - image_base
        table_end = table_address + cases * 4
        dispatch_start = function_base + guard_start * 4
        dispatch_end = function_base + (index + 6) * 4
        if (table_address % 4 or table_offset < 0 or table_offset + cases * 4 > len(image)
                or table_address < dispatch_end and dispatch_start < table_end):
            continue
        default = function_base + guard_index * 4 + signed16(guard & 0xFFFC)
        labels = struct.unpack_from(f">{cases}I", image, table_offset)
        def valid_target(address):
            return (function_base <= address < function_base + function_size and address % 4 == 0
                    and not table_address <= address < table_end
                    and not dispatch_start <= address < dispatch_end)
        if valid_target(default) and all(valid_target(label) for label in labels):
            found.append(SwitchTable(function_base + index * 4, selector, default, labels))
    return found


def discover_u8_scaled_switches(
    image: bytes, image_base: int, function_base: int, function_size: int
) -> list[SwitchTable]:
    """Guarded byte offsets scaled by four, relative to a local code anchor.

    cmplwi/bgt; lis/addi; lbzx; rlwinm; lis/[nop]/addi/[nop]; add; mtctr; bctr.
    At most one nop is allowed. Every register edge, guard and local target is
    checked; no arbitrary backwards search or callable interior mapping is used.
    """
    words = function_words(image, image_base, function_base, function_size)
    found = []
    op = lambda w: w >> 26
    rt = lambda w: (w >> 21) & 31
    ra = lambda w: (w >> 16) & 31
    rb = lambda w: (w >> 11) & 31
    xo = lambda w: (w >> 1) & 1023
    local = lambda a: function_base <= a < function_base + function_size and a % 4 == 0
    for index in range(2, len(words) - 8):
        high, low, load, shift, anchor_high = words[index:index+5]
        if op(high) != 15 or ra(high) or not rt(high):
            continue
        table, selector, offset = rt(high), rb(load), rt(load)
        if (op(low) != 14 or rt(low) != table or ra(low) != table
                or op(load) != 31 or xo(load) != 87 or load & 1 or ra(load) != table
                or len({table, selector, offset}) != 3
                or op(shift) != 21 or rt(shift) != offset or ra(shift) != offset
                or rb(shift) != 2 or ((shift >> 6) & 31) != 0 or ((shift >> 1) & 31) != 29 or shift & 1
                or op(anchor_high) != 15 or rt(anchor_high) != table or ra(anchor_high)):
            continue
        cursor = index + 5
        before_nop = words[cursor] == 0x60000000
        if before_nop:
            cursor += 1
        anchor_low = words[cursor]
        if op(anchor_low) != 14 or rt(anchor_low) != table or ra(anchor_low) != table:
            continue
        cursor += 1
        if not before_nop and words[cursor] == 0x60000000:
            cursor += 1
        if cursor + 2 >= len(words):
            continue
        add, mtctr, branch = words[cursor:cursor+3]
        if (op(add) != 31 or xo(add) != 266 or add & 1 or add & (1 << 10)
                or rt(add) != table or ra(add) != table or rb(add) != offset
                or op(mtctr) != 31 or xo(mtctr) != 467 or mtctr & 1
                or rt(mtctr) != table or ra(mtctr) != 9 or rb(mtctr)
                or branch != 0x4E800420):
            continue
        compare, guard = words[index-2:index]
        cr = (compare >> 23) & 7
        if (op(compare) != 10 or compare & 0x00600000 or ra(compare) != selector
                or op(guard) != 16 or rt(guard) != 12 or ra(guard) != cr*4+1 or guard & 3):
            continue
        cases = (compare & 0xFFFF) + 1
        if cases > MAX_CASES:
            continue
        table_address = (((high & 0xFFFF) << 16) + signed16(low)) & 0xFFFFFFFF
        anchor = (((anchor_high & 0xFFFF) << 16) + signed16(anchor_low)) & 0xFFFFFFFF
        start, end = function_base+(index-2)*4, function_base+(cursor+3)*4
        table_offset = table_address-image_base
        if (table_offset < 0 or table_offset+cases > len(image) or not local(anchor)
                or table_address < end and start < table_address+cases):
            continue
        default = function_base+(index-1)*4+signed16(guard & 0xFFFC)
        labels = tuple(anchor + value*4 for value in image[table_offset:table_offset+cases])
        def valid_target(address):
            return (local(address) and not start <= address < end
                    and not table_address <= address < table_address+cases)
        if valid_target(default) and all(valid_target(label) for label in labels):
            found.append(SwitchTable(function_base+index*4, selector, default, labels))
    return found


def serialize(tables: list[SwitchTable]) -> str:
    lines = ["# Bounded switch metadata; private, game-derived addresses."]
    for table in tables:
        lines.extend(["", "[[switch]]", f"base = 0x{table.base:08X}",
                      f"r = {table.selector}", f"default = 0x{table.default:08X}",
                      "labels = [" + ", ".join(f"0x{x:08X}" for x in table.labels) + "]"])
    return "\n".join(lines) + "\n"


def verify_constant_leaf_switch(image: bytes, image_base: int, base: int, size: int) -> dict:
    """Verify a narrow complete leaf: two loads, guarded inline table, LI/BLR cases.

    Useful evidence when an analyzer mistakes inline table bytes for a function.
    All 256 possible byte selectors have an independently decoded result.
    """
    words = function_words(image, image_base, base, size)
    tables = discover_u32_switches(image, image_base, base, size)
    if len(words) < 13 or len(tables) != 1 or tables[0].base != base + 16:
        raise ValueError("not one complete constant-return leaf switch")
    table = tables[0]
    first, second = words[:2]
    register = (first >> 21) & 31
    pointer_offset, selector_offset = signed16(first), signed16(second)
    if (first >> 26 != 32 or (first >> 16) & 31 != 3 or not 4 <= register <= 12
            or second >> 26 != 34 or (second >> 16) & 31 != register
            or (second >> 21) & 31 != table.selector or table.selector != register
            or not 0 <= pointer_offset <= 1024 or pointer_offset % 4 or not 0 <= selector_offset <= 1024
            or len(table.labels) > 256):
        raise ValueError("leaf selector load chain is not supported")
    low = words[5] if words[5] >> 26 == 14 else words[6]
    table_address = (((words[4] & 0xFFFF) << 16) + signed16(low)) & 0xFFFFFFFF
    table_end = table_address + len(table.labels) * 4
    targets = sorted(set([table.default, *table.labels]))
    if table_address != base + 40 or targets != list(range(table_end, base + size, 8)):
        raise ValueError("leaf table and return blocks are not contiguous/exhaustive")
    values = {}
    for target in targets:
        index = (target - base) // 4
        if index + 1 >= len(words) or words[index] & 0xFFFF0000 != 0x38600000 or words[index + 1] != 0x4E800020:
            raise ValueError("leaf case is not a single signed-immediate r3 return")
        values[target] = signed16(words[index])
    return {"table": table, "pointer_offset": pointer_offset, "selector_offset": selector_offset,
            "returns": tuple(values[table.labels[i]] if i < len(table.labels) else values[table.default] for i in range(256))}


def verify_argument_leaf_switch(image: bytes, image_base: int, base: int, size: int) -> dict:
    """Prove a complete direct-argument switch ending in ADDI/BLR blocks.

    Only r3/r4 unsigned selectors are accepted, with no setup before the guard.
    Every byte belongs to the checked dispatch, inline table, or an exhaustive
    two-instruction return block. Returns either a signed immediate or r3 plus
    one; no memory access, call, arbitrary interval widening, or inferred tail.
    This verifier does not itself alter source or callable mappings.
    """
    words = function_words(image, image_base, base, size)
    tables = discover_u32_switches(image, image_base, base, size)
    if len(words) < 11 or len(tables) != 1 or tables[0].base != base + 8:
        raise ValueError("not one complete direct-argument leaf switch")
    table = tables[0]
    compare, guard, high, a, b = words[:5]
    if table.selector not in {3, 4} or ((compare >> 16) & 31) != table.selector:
        raise ValueError("leaf must directly compare the r3/r4 selector")
    low = a if a >> 26 == 14 else b
    table_address = (((high & 0xFFFF) << 16) + signed16(low)) & 0xFFFFFFFF
    table_end = table_address + len(table.labels) * 4
    targets = sorted(set([table.default, *table.labels]))
    if table_address != base + 32 or targets != list(range(table_end, base + size, 8)):
        raise ValueError("argument leaf table and returns are not contiguous/exhaustive")
    returns = {}
    for target in targets:
        index = (target - base) // 4
        if (index + 1 >= len(words) or words[index] & 0xFFFF0000 not in {0x38600000, 0x38630000}
                or words[index + 1] != 0x4E800020):
            raise ValueError("argument leaf return must be li/addi r3 followed by blr")
        returns[target] = {"source": (words[index] >> 16) & 31, "immediate": signed16(words[index])}
    return {"table": table, "returns": returns}


def parse_switch_metadata(text: str) -> list[SwitchTable]:
    """Read only this tool's flat numeric schema for a checked metadata merge."""
    if len(text) > 1024 * 1024:
        raise ValueError("prior switch metadata exceeds the size bound")
    tables = []
    fields = None
    def finish():
        if fields is None:
            return
        if fields.keys() != {"base", "r", "default", "labels"}:
            raise ValueError("incomplete prior switch metadata")
        addresses = [fields["base"], fields["default"], *fields["labels"]]
        if not 0 <= fields["r"] <= 31 or not 1 <= len(fields["labels"]) <= MAX_CASES \
                or any(not 0 < address < 1 << 32 or address % 4 for address in addresses):
            raise ValueError("prior switch metadata values are outside bounds")
        tables.append(SwitchTable(fields["base"], fields["r"], fields["default"], tuple(fields["labels"])))
    number = r"(?:0x[0-9A-Fa-f]+|[0-9]+)"
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line == "[[switch]]":
            finish(); fields = {}; continue
        key, separator, value = line.partition("=")
        key, value = key.strip(), value.strip()
        if not separator or fields is None or key not in {"base", "r", "default", "labels"} or key in fields:
            raise ValueError("unknown or duplicate prior switch metadata field")
        if key == "labels":
            if not re.fullmatch(r"\[\s*" + number + r"(?:\s*,\s*" + number + r")*\s*\]", value):
                raise ValueError("prior switch labels must be a numeric array")
            fields[key] = [int(part.strip(), 0) for part in value[1:-1].split(",")]
        else:
            if not re.fullmatch(number, value):
                raise ValueError("prior switch scalar must be numeric")
            fields[key] = int(value, 0)
    finish()
    if not tables or len({table.base for table in tables}) != len(tables) \
            or sum(len(table.labels) for table in tables) > 32768:
        raise ValueError("prior switch metadata is empty, duplicated or excessive")
    return tables


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--image-base", type=lambda x: int(x, 0), required=True)
    for name in ("function-base", "function-size", "code-base", "code-size"):
        parser.add_argument("--" + name, type=lambda x: int(x, 0))
    parser.add_argument("--function-map", type=Path,
                        help="explicit validated PPCFuncMappings source; requires --code-base/--code-size")
    parser.add_argument("--append-to", type=Path, help="merge prior verified numeric switch metadata; duplicate bases fail")
    parser.add_argument("--extended-guards", action="store_true", help="allow the audited 64-instruction vector guard whitelist")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--also-function", nargs=2, type=lambda x: int(x, 0), action="append",
                        default=[], metavar=("BASE", "SIZE"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    private = root / "local-analysis"
    if args.function_map is None:
        if args.function_base is None or args.function_size is None or args.code_base is not None or args.code_size is not None:
            raise ValueError("explicit local scan requires --function-base and --function-size only")
    elif args.function_base is not None or args.function_size is not None or args.also_function \
            or args.code_base is None or args.code_size is None:
        raise ValueError("function-map scan requires code range and cannot mix explicit function ranges")
    for path in (args.image, args.output, *(path for path in (args.function_map, args.append_to) if path is not None)):
        lexical = path.expanduser().absolute()
        if lexical.is_symlink() or lexical != lexical.resolve() or not lexical.is_relative_to(private):
            raise ValueError("image and output must be regular private paths without symlinks")
        if subprocess.run(["git", "check-ignore", "--quiet", "--", str(lexical)], cwd=root).returncode != 0:
            raise ValueError("private path must be Git ignored")
    if not args.image.is_file() or not 0 < args.image.stat().st_size <= MAX_IMAGE_BYTES:
        raise ValueError("image is outside the file-size bound")
    with args.image.open("rb") as stream:
        data = stream.read(MAX_IMAGE_BYTES + 1)
    if args.function_map is not None:
        if not args.function_map.is_file() or args.function_map.stat().st_size > MAX_MAPPING_BYTES:
            raise ValueError("function map is not a bounded regular file")
        functions = function_ranges_from_map(args.function_map.read_text(), args.code_base, args.code_size)
        if not (args.image_base <= args.code_base and args.code_base + args.code_size <= args.image_base + len(data)):
            raise ValueError("mapped code range is outside the decoded image")
    else:
        functions = [(args.function_base, args.function_size), *args.also_function]
        if len(functions) > 16:
            raise ValueError("at most sixteen explicit function ranges are supported")
    tables = []
    if args.append_to is not None:
        if not args.append_to.is_file() or args.append_to.stat().st_size > 1024 * 1024:
            raise ValueError("prior switch metadata must be a bounded regular file")
        tables = parse_switch_metadata(args.append_to.read_text())
    prior_count = len(tables)
    for function_base, function_size in functions:
        tables.extend(discover_u16_switches(data, args.image_base, function_base, function_size))
        tables.extend(discover_u32_switches(data, args.image_base, function_base, function_size,
                                           extended_guards=args.extended_guards))
        tables.extend(discover_u8_scaled_switches(data, args.image_base, function_base, function_size))
    tables.sort(key=lambda table: table.base)
    if len({table.base for table in tables}) != len(tables) or sum(len(t.labels) for t in tables) > 32768:
        raise ValueError("overlapping discovery or aggregate case limit exceeded")
    if len(tables) == prior_count:
        raise ValueError("no complete bounded switch idiom found")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "w", encoding='utf-8', newline='\n') as stream:
        stream.write(serialize(tables))
    print(f"Validated {len(tables)} switch table(s), {sum(len(t.labels) for t in tables)} cases; {len(functions)} scanned intervals, {prior_count} prior tables preserved; metadata kept private")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        raise SystemExit(f"switch discovery failed: {error}")
