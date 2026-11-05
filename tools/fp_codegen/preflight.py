#!/usr/bin/env python3
"""Read-only batch triage of known switch idioms and generated dependencies.

This inventories requirements, not correctness or runtime reachability. It never
patches source, widens function ranges, hydrates cloud files or launches a game.
"""
from __future__ import annotations

import argparse
from collections import Counter, deque
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess

from leaf_switch_fixture import extract_function
from switch_tables import (MAX_IMAGE_BYTES, MAX_MAPPING_BYTES, SwitchTable,
                           discover_u16_switches, discover_u32_switches,
                           discover_u8_scaled_switches, function_ranges_from_map)
from switch_target_fixture import checked_fragment
from split_tail_switch import verify_split_tail_switch, original_head, patched_head


MAX_SOURCE_BYTES = 8 * 1024 * 1024
MAX_SOURCE_TOTAL = 1024 * 1024 * 1024
MAX_CHUNKS = 1024
DATALESS = 0x40000000  # macOS SF_DATALESS; don't trigger iCloud hydration.
HEADER = re.compile(r'^PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\) \{', re.M)
DIRECT = re.compile(r'^\s*PPC_CALL_FUNC\(sub_([0-9A-F]+)\);', re.M)
DIRECT_NATIVE = re.compile(r'^\s*sub_([0-9A-F]+)\(ctx, base\);', re.M)
TRAP = re.compile(r'^\s*(?:__builtin_debugtrap|__builtin_trap|__debugbreak)\(\);', re.M)
RUNTIME_TRAP = re.compile(r'^\s*PPC_RUNTIME_TRAP\(\);', re.M)
BASELINE = '\t// bctr \n\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n\treturn;'


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def resident_bytes(path: Path, limit: int) -> bytes | None:
    """Return None for unavailable cloud content; fail closed on other hazards."""
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
        raise ValueError('input is not a bounded regular file')
    if getattr(info, 'st_flags', 0) & DATALESS:
        return None
    with path.open('rb') as stream:
        data = stream.read(limit + 1)
    if len(data) != info.st_size or len(data) > limit:
        raise ValueError('input changed size while reading')
    return data


def index_functions(source: str) -> dict[int, str]:
    """Bound each definition by its next header; no chunk-number assumptions.

    The returned slices may include the trailing alias declaration. Consumers
    needing a body must additionally call extract_function (brace/format check).
    """
    matches = list(HEADER.finditer(source))
    result = {}
    for i, match in enumerate(matches):
        address = int(match[1], 16)
        if address in result:
            raise ValueError('duplicate generated function definition')
        end = matches[i + 1].start() if i + 1 < len(matches) else len(source)
        result[address] = source[match.start():end]
    return result


def source_positions(function: str, image: bytes, image_base: int,
                     start: int, size: int) -> dict[int, int]:
    """Correlate unchanged generator instruction comments with label/LR checks.

    This is location/extent evidence, NOT proof of each emitted instruction's
    semantics. Repaired sources with removed inline data deliberately fail.
    """
    pc, offset = start, 0
    positions = {}
    for line in function.splitlines(keepends=True):
        label = re.fullmatch(r'loc_([0-9A-F]+):\n', line)
        if label and int(label[1], 16) != pc:
            raise ValueError('generated label disagrees with instruction position')
        # `\t// ERROR <addr>` follows an unconditional branch the generator could not place; it
        # is a marker, not an instruction comment (ppc_recomp.10.cpp, sub_822D9F78).
        if line.startswith('\t// ') and not line.startswith('\t// ERROR '):
            positions[pc] = offset
            pc += 4
        lr = re.fullmatch(r'\tctx.lr = (0x[0-9A-F]+);\n', line)
        if lr and int(lr[1], 16) != pc:
            raise ValueError('generated LR disagrees with instruction position')
        offset += len(line)
    end = start + size
    if not start <= pc <= end or end - pc > 12:
        raise ValueError('generated extent differs from mapped interval')
    padding = image[pc-image_base:end-image_base]
    if len(padding) != end-pc or any(w[0] not in (0, 0x60000000) for w in struct.iter_unpack('>I', padding)):
        raise ValueError('unaccounted original instructions after generated body')
    return positions


def classify_switch(source: str, table: SwitchTable, image: bytes,
                    image_base: int, start: int, size: int) -> dict:
    function = extract_function(source, start, allow_indirect=True)
    try:
        checked_fragment(function, table)
        return {'status': 'paired_dispatch_present', 'evidence': 'exact fragment and unique local labels only'}
    except ValueError:
        pass
    compact = [f'\tswitch (ctx.r{table.selector}.u32) {{']
    compact.extend(f'\tcase {i}: if (ctx.ctr.u32 == 0x{target:X}U) goto loc_{target:X}; break;'
                   for i, target in enumerate(table.labels))
    compact_end = ['\t}', '\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);', '\treturn;']
    forms = ('\n'.join(compact+compact_end), '\n'.join(compact+['\tdefault: break;']+compact_end))
    if sum(function.count(form) for form in forms) == 1 and all(
            len(re.findall(rf'^loc_{target:X}:$', function, re.M)) == 1 for target in table.labels):
        return {'status': 'paired_dispatch_present', 'evidence': 'exact compact paired fragment and unique labels only'}
    # Every supported idiom ends within eleven instructions of its first LIS.
    branches = [pc for pc in range(table.base, min(start+size, table.base+44), 4)
                if image[pc-image_base:pc-image_base+4] == b'\x4e\x80\x04\x20']
    if not branches:
        return {'status': 'needs_source_review', 'reason': 'dispatch branch not correlated'}
    try:
        positions = source_positions(function, image, image_base, start, size)
    except ValueError as error:
        return {'status': 'needs_source_review', 'reason': str(error)}
    offset = positions.get(branches[0])
    if offset is None or not function[offset:].startswith(BASELINE):
        return {'status': 'needs_source_review', 'reason': 'dispatch differs from baseline and audited paired form'}
    if any(target not in positions for target in (*table.labels, table.default)):
        return {'status': 'needs_source_review', 'reason': 'case/default lacks correlated instruction position'}
    return {'status': 'unspecialized_dispatch', 'bctr': f'{branches[0]:08X}',
            'evidence': 'known original idiom; generated extent/label/LR and baseline dispatch match'}


def unproven_u32_dispatches(image: bytes, image_base: int, start: int, size: int) -> list[int]:
    """Find only the six-instruction absolute-word dispatch chain, WITHOUT guard proof.

    Triage only: no table size, case target, valid function extent or safe patch
    is inferred. This catches variants outside the recovery scanner's whitelist.
    """
    words = struct.unpack_from(f'>{size//4}I', image, start-image_base)
    found = []
    op = lambda w: w >> 26
    rt = lambda w: (w >> 21) & 31
    ra = lambda w: (w >> 16) & 31
    rb = lambda w: (w >> 11) & 31
    for i in range(len(words)-5):
        high, a, b, load, mtctr, branch = words[i:i+6]
        if op(high) != 15 or ra(high) or not rt(high):
            continue
        low, shift = (a, b) if op(a) == 14 else (b, a)
        table, selector, offset = rt(high), rt(shift), ra(shift)
        if (op(low) == 14 and rt(low) == table and ra(low) == table
                and op(shift) == 21 and rb(shift) == 2 and (shift >> 6) & 31 == 0
                and (shift >> 1) & 31 == 29 and not shift & 1
                and len({table, selector, offset}) == 3
                and load == (31 << 26) | (offset << 21) | (table << 16) | (offset << 11) | (23 << 1)
                and mtctr == (31 << 26) | (offset << 21) | (9 << 16) | (467 << 1)
                and branch == 0x4E800420):
            found.append(start+i*4)
    return found


def direct_distances(graph: dict[int, set[int]], roots: list[int], depth: int) -> dict[int, int]:
    """Syntactic direct-call proximity, not dynamic reachability."""
    if not 0 <= depth <= 8 or len(roots) > 64:
        raise ValueError('direct dependency scan exceeds depth/root bound')
    distance = dict.fromkeys(roots, 0)
    queue = deque(roots)
    while queue:
        address = queue.popleft()
        if distance[address] == depth:
            continue
        for target in sorted(graph.get(address, ())):
            if target not in distance:
                distance[target] = distance[address] + 1
                queue.append(target)
    return distance


def audit(image: bytes, image_base: int, mapping: str, code_base: int,
          code_size: int, sources: Path, roots: list[int], depth: int) -> dict:
    if not 0 < len(image) <= MAX_IMAGE_BYTES or not (0 <= image_base <= code_base
            and code_base + code_size <= image_base + len(image) <= 1 << 32):
        raise ValueError('mapped code range is outside bounded image')
    ranges = function_ranges_from_map(mapping, code_base, code_size)
    mapped = {start for start, _ in ranges}
    candidates, unproven = {}, {}
    bctr_intervals = 0
    for start, size in ranges:
        if b'\x4e\x80\x04\x20' not in image[start-image_base:start-image_base+size]:
            continue
        bctr_intervals += 1
        found = []
        for kind, scanner in (('u32', discover_u32_switches), ('u16', discover_u16_switches),
                              ('u8_scaled', discover_u8_scaled_switches)):
            options = {'extended_guards': True} if kind == 'u32' else {}
            found.extend((kind, table) for table in scanner(image, image_base, start, size, **options))
        if len({table.base for _, table in found}) != len(found):
            raise ValueError('overlapping switch discoveries')
        if found:
            candidates[start] = (size, found)
        shells = sorted(set(unproven_u32_dispatches(image, image_base, start, size))
                        - {table.base for _, table in found})
        if shells:
            unproven[start] = (size, shells)
    paths = sorted((p for p in sources.iterdir() if re.fullmatch(r'ppc_recomp\.[0-9]+\.cpp', p.name)),
                   key=lambda p: int(p.name.split('.')[1]))
    if not 1 <= len(paths) <= MAX_CHUNKS:
        raise ValueError('generated chunk count outside bound')
    report = {'schema': 1, 'scope': 'known idioms in mapped intervals; resident generated source; no runtime proof',
              'image_sha256': sha(image), 'mapping_sha256': sha(mapping.encode()),
              'image_base': f'{image_base:08X}', 'code_base': f'{code_base:08X}', 'code_size': code_size,
              'mapped_intervals': len(ranges), 'bctr_intervals': bctr_intervals,
              'chunks': [], 'switches': [], 'unproven_dispatches': [], 'trap_functions': [],
              'runtime_trap_functions': []}
    observed, graph, total = set(), {}, 0
    for path in paths:
        data = resident_bytes(path, MAX_SOURCE_BYTES)
        if data is None:
            report['chunks'].append({'path': str(path), 'status': 'dataless_not_read'})
            continue
        total += len(data)
        if total > MAX_SOURCE_TOTAL:
            raise ValueError('resident source total exceeds bound')
        report['chunks'].append({'path': str(path), 'status': 'read', 'sha256': sha(data), 'bytes': len(data)})
        definitions = index_functions(data.decode())
        for start, source in definitions.items():
            if start in observed:
                raise ValueError('function definition occurs in multiple chunks')
            observed.add(start)
            graph[start] = {int(target, 16) for target in
                            [*DIRECT.findall(source), *DIRECT_NATIVE.findall(source)]}
            traps = len(TRAP.findall(source))
            if traps:
                report['trap_functions'].append({'function': f'{start:08X}', 'source': str(path), 'count': traps,
                                                'evidence': 'textual trap; may be legitimate or unreachable table data'})
            runtime_traps = len(RUNTIME_TRAP.findall(source))
            if runtime_traps:
                report['runtime_trap_functions'].append(dict(function=f'{start:08X}', source=str(path),
                    count=runtime_traps, evidence='original conditional/assertion trap; not an unsupported-instruction diagnosis'))
            if start in unproven:
                size, shells = unproven[start]
                try:
                    function = extract_function(source, start, allow_indirect=True)
                    positions = source_positions(function, image, image_base, start, size)
                except ValueError:
                    function, positions = '', {}
                for dispatch in shells:
                    offset = positions.get(dispatch+20)
                    baseline = offset is not None and function[offset:].startswith(BASELINE)
                    row=dict(function=f'{start:08X}', size=size,
                        dispatch=f'{dispatch:08X}', source=str(path),
                        status='baseline_dispatch_needs_guard_proof' if baseline else 'needs_source_review',
                        evidence='six-instruction dataflow only; guard, table, cases NOT proven')
                    try:
                        proof=verify_split_tail_switch(image,image_base,start,mapped)
                        exact=extract_function(source,start,allow_indirect=True)
                        original=original_head(proof)
                        if exact==original or exact==patched_head(original,proof):
                            row.update(status='split_tail_repair_present' if exact!=original else 'split_tail_repair_proven',
                                evidence='exact unsigned guard/inline table; missing cases are unconditional mapped tails or BLR; no range widening',
                                missing_tails={f'{k:08X}':f'{v:08X}' for k,v in proof['missing'].items()},
                                missing_returns=[f'{k:08X}' for k in proof['returns']])
                    except ValueError:
                        pass
                    report['unproven_dispatches'].append(row)
            if start not in candidates:
                continue
            size, found = candidates[start]
            for kind, table in found:
                try:
                    result = classify_switch(source, table, image, image_base, start, size)
                except ValueError as error:
                    result = {'status': 'needs_source_review', 'reason': str(error)}
                report['switches'].append(dict(function=f'{start:08X}', size=size, kind=kind,
                    dispatch=f'{table.base:08X}', selector=table.selector, cases=len(table.labels),
                    targets=[f'{t:08X}' for t in table.labels], default=f'{table.default:08X}',
                    unmapped_targets=[f'{t:08X}' for t in sorted(set(table.labels)-mapped)],
                    source=str(path), **result))
    for start, (size, found) in candidates.items():
        if start not in observed:
            for kind, table in found:
                report['switches'].append(dict(function=f'{start:08X}', size=size, kind=kind,
                    dispatch=f'{table.base:08X}', cases=len(table.labels), status='source_unavailable',
                    reason='no definition in resident chunks; no inferred chunk number'))
    for start, (size, shells) in unproven.items():
        if start not in observed:
            for dispatch in shells:
                report['unproven_dispatches'].append(dict(function=f'{start:08X}', size=size,
                    dispatch=f'{dispatch:08X}', status='source_unavailable',
                    evidence='six-instruction dataflow only; guard, table, cases NOT proven'))
    distance = direct_distances(graph, roots, depth)
    for row in [*report['switches'], *report['unproven_dispatches'], *report['trap_functions'],
                *report['runtime_trap_functions']]:
        row['direct_call_distance'] = distance.get(int(row['function'], 16))
    report['roots'] = [f'{r:08X}' for r in roots]
    report['direct_call_depth'] = depth
    report['dependency_source_unavailable'] = [f'{a:08X}' for a in sorted(distance) if a not in graph]
    report['switches'].sort(key=lambda r: (r['direct_call_distance'] is None,
        r['direct_call_distance'] or 0, r['function'], r['dispatch']))
    report['summary'] = dict(switch_statuses=dict(Counter(r['status'] for r in report['switches'])),
                            unproven_statuses=dict(Counter(r['status'] for r in report['unproven_dispatches'])),
                            resident_chunks=sum(r['status'] == 'read' for r in report['chunks']),
                            unavailable_chunks=sum(r['status'] != 'read' for r in report['chunks']),
                            resident_functions=len(observed), resident_bytes=total,
                            trap_functions=len(report['trap_functions']), direct_dependencies=len(distance))
    report['summary']['runtime_trap_functions'] = len(report['runtime_trap_functions'])
    report['limitations'] = ['No function-range widening: most analyzer-split switches can still be missed.',
        'Only three supported idiom families; other indirect calls are not automatically bugs.',
        'Source fingerprints are not proof the installed library contains that source.',
        'Direct-call edges omit virtual/indirect calls and do not prove an executed path.',
        'Native import/helper implementations are not expanded by this generated sub-function graph.',
        'Missing resident files, generated traps and unknown lowerings require investigation.',
        'No renderer, asset, import semantics or gameplay-completeness proof.']
    return report


def private_path(path: Path, root: Path) -> Path:
    path = path.expanduser().absolute()
    if path != path.resolve() or not path.is_relative_to(root/'local-analysis') \
            or subprocess.run(['git', 'check-ignore', '--quiet', '--', str(path)], cwd=root).returncode:
        raise ValueError('audit paths must be ignored local-analysis paths without symlinks')
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--function-map', type=Path, required=True)
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    for name in ('image-base', 'code-base', 'code-size'):
        parser.add_argument('--'+name, type=lambda v: int(v, 0), required=True)
    parser.add_argument('--image-sha256', required=True)
    parser.add_argument('--root-function', type=lambda v: int(v, 0), action='append', default=[])
    parser.add_argument('--direct-depth', type=int, default=2)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    for name in ('image', 'function_map', 'sources', 'output'):
        setattr(args, name, private_path(getattr(args, name), root))
    if args.output.exists():
        raise ValueError('report already exists; choose a new checkpoint name')
    image = resident_bytes(args.image, MAX_IMAGE_BYTES)
    mapping = resident_bytes(args.function_map, MAX_MAPPING_BYTES)
    if image is None or mapping is None:
        raise ValueError('image/map unavailable locally; no hydration attempted')
    if not re.fullmatch('[0-9a-f]{64}', args.image_sha256) or sha(image) != args.image_sha256:
        raise ValueError('image fingerprint mismatch')
    report = audit(image, args.image_base, mapping.decode(), args.code_base, args.code_size,
                   args.sources, args.root_function, args.direct_depth)
    report['tool_sha256'] = sha(Path(__file__).read_bytes())
    report['scanner_sha256'] = sha(Path(__file__).with_name('switch_tables.py').read_bytes())
    report['proof_modules'] = {name: sha(Path(__file__).with_name(name).read_bytes())
                              for name in ('leaf_switch_fixture.py', 'switch_target_fixture.py', 'split_tail_switch.py')}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, 'w', encoding='utf-8', newline='\n') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    print(json.dumps(report['summary'], sort_keys=True))
    print('Private triage report created; no code, device or cloud content changed.')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        raise SystemExit(f'preflight failed: {error}')
