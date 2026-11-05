#!/usr/bin/env python3
"""Disc shader inventory/build adapter. No key files or resident shader inputs.

Run from an installed checkout, or pass --repo when testing a lane copy. Output
is private, append-only per phase. Any unresolved shader prevents publication
as a complete builder stage; diagnostic translation still preserves its pack.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass

sys.path.insert(0, str(Path(__file__).resolve().parent))      # the host helper lies beside this file
import host_support  # noqa: E402
import shader_toolchain  # noqa: E402

sys.dont_write_bytecode = True


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, obj):
    with path.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(obj, stream, indent=2, sort_keys=True)
        stream.write('\n')


@dataclass
class View:
    size: int
    read_at: object

    def read(self, off, size):
        if off < 0 or size < 0 or size > 512 * 1024**2 or off + size > self.size:
            raise ValueError('bounded input extent refused')
        data = self.read_at(off, size)
        if len(data) != size:
            raise ValueError('short input read')
        return data

    def sub(self, off, size):
        if off < 0 or size < 0 or off + size > self.size:
            raise ValueError('nested extent refused')
        return View(size, lambda p, n: self.read(off + p, n))


def disc_entries(disc):
    """Enumerate through the project's XDVDFS reader, with cycle/depth limits."""
    pending = [('', disc.root_sector, disc.root_size, 0)]
    seen = set()
    with disc.image.open('rb') as f:
        while pending:
            prefix, sector, size, depth = pending.pop()
            if (sector, size) in seen or depth > 32 or len(seen) > 10000:
                raise ValueError('disc directory recursion refused')
            seen.add((sector, size))
            for name, (start, length, directory) in sorted(disc._read_dir(f, sector, size).items()):
                if name in ('.', '..') or '/' in name or '\\' in name:
                    raise ValueError('unsafe disc component')
                path = prefix + name
                if directory:
                    pending.append((path + '/', start, length, depth + 1))
                else:
                    yield path, length


def package_disposition(title, expected_title, content_type):
    # The dashboard's theme/picture packages cannot be mounted as game archives.
    # Unknown same-title package classes remain unresolved; do not guess them away.
    if title != expected_title:
        return 'different-title'
    if content_type in (0x20000, 0x30000):
        return 'dashboard-picture-or-theme'
    return None


def extract(args):
    from disc_builder import XisoDisc, disc_copy
    from stfs_inventory import Package, BLOCK
    from rpf7_toc import parse_toc_header, _decrypt_tables
    from rpf7_catalog import parse_catalog_entries
    from rpf7_static_key_recover import recover_constant_stores, _MemoryKeyProvider
    from rpf7_extract import decode_binary_payload, decode_xmem_with_tool
    from fxc_xenos_inspect import inspect_fxc_bytes
    from generic_shader_pack import parse_container, canonical_microcode, fnv1a64

    out = args.output
    out.mkdir(parents=True, exist_ok=False)
    (out / 'fxc').mkdir()
    (out / 'containers').mkdir()
    (out / 'packages').mkdir()
    # Existing project recovery evaluates generated immediate-store instructions.
    # Never serialise the key or store records, hash them, or open a key file.
    secret, stores = recover_constant_stores(args.generated, target_address=args.key_address)
    provider = _MemoryKeyProvider(secret)
    del stores
    manifest = dict(schema=1, discs=[], archives=[], effects=[], containers=[],
                    unresolved=[], skipped_packages=[], packages=[], key_files_used=False,
                    source='disc', codegen_input=str(args.generated))
    known_containers = {}
    archive_limit = 4096

    def decode(view, entry):
        blob = view.read(entry.payload_offset, entry.stored_size)
        return decode_binary_payload(blob, encrypted=bool(entry.encrypted),
            compressed=bool(entry.compressed), logical_size=entry.logical_size,
            key=secret if entry.encrypted else None,
            xmem_decoder=lambda data, expected, maximum: decode_xmem_with_tool(
                data, expected, maximum, executable=args.xmem))

    def effect(view, origin):
        data = view.read(0, view.size)
        ident = sha(data)
        fxc_path = out / 'fxc' / (ident + '.fxc')
        if not fxc_path.exists():
            fxc_path.write_bytes(data)
        item = dict(origin=origin, sha256=ident, bytes=len(data), file=record_name(fxc_path, out))
        manifest['effects'].append(item)
        try:
            inspection = inspect_fxc_bytes(data)
        except ValueError:
            item['status'] = 'unsupported-fxc'
            manifest['unresolved'].append(dict(origin=origin, reason='unsupported-fxc'))
            return
        item.update(containers=len(inspection.containers), rejected=inspection.rejected_magic_candidates)
        if inspection.rejected_magic_candidates or not inspection.containers:
            manifest['unresolved'].append(dict(origin=origin, reason='incomplete-fxc-staging',
                rejected=inspection.rejected_magic_candidates))
        for c in inspection.containers:
            blob = data[c.offset:c.offset + c.size]
            digest = sha(blob)
            provenance = dict(origin=origin, effect_sha256=ident, offset=c.offset)
            if digest in known_containers:
                known_containers[digest]['origins'].append(provenance)
                continue
            entry = dict(sha256=digest, bytes=len(blob), stage=c.stage, origins=[provenance])
            dest = out / 'containers' / (digest + '.xenos')
            dest.write_bytes(blob)
            entry['file'] = record_name(dest, out)
            try:
                microcode, elements, outputs, samplers = parse_container(blob, c.stage)
                entry.update(key=f'{fnv1a64(canonical_microcode(microcode)):016x}',
                             canonical_sha256=sha(canonical_microcode(microcode)))
            except (ValueError, IndexError, struct.error):
                entry['status'] = 'pack-metadata-refused'
                manifest['unresolved'].append(dict(origin=origin, reason=entry['status']))
            known_containers[digest] = entry
            manifest['containers'].append(entry)

    def archive(view, origin, depth=0):
        if depth > 16 or len(manifest['archives']) >= archive_limit:
            raise ValueError('archive recursion budget refused')
        header = parse_toc_header(view.read(0, 16), view.size)
        eb = view.read(16, header.entries_size)
        nb = view.read(16 + header.entries_size, header.names_size)
        eb, nb, decrypted = _decrypt_tables(eb, nb, header=header, archive=Path(origin),
            key_provider=provider, decryptor=__import__('rpf7_crypto').decrypt_aes256_ecb_prefix)
        entries = parse_catalog_entries(eb, nb, header=header, decrypted=decrypted)
        manifest['archives'].append(dict(origin=origin, bytes=view.size, entries=len(entries),
                                         toc_sha256=sha(eb + nb), depth=depth))
        for entry in entries:
            if entry.kind == 'directory':
                continue
            suffix = Path(entry.path).suffix.lower()
            if suffix not in ('.rpf', '.fxc'):
                continue
            child_origin = origin + '!/' + entry.path
            if entry.kind != 'binary':
                manifest['unresolved'].append(dict(origin=child_origin, reason='nonbinary-shader-or-archive'))
                continue
            if entry.encrypted or entry.compressed:
                data = decode(view, entry)
                child = View(len(data), lambda p, n, data=data: data[p:p+n])
            else:
                child = view.sub(entry.payload_offset, entry.logical_size)
            if suffix == '.rpf':
                archive(child, child_origin, depth + 1)
            else:
                effect(child, child_origin)

    def package_view(package, entry):
        def read(off, size):
            chunks = []
            while size:
                index, skip = divmod(off, BLOCK)
                count = min(size, BLOCK - skip)
                chunks.append(package.block(entry.blocks[index])[skip:skip+count])
                off += count
                size -= count
            return b''.join(chunks)
        return View(entry.size, read)

    start = time.monotonic()
    for label, image in [('play', args.play), ('install', args.install)]:
        disc = XisoDisc(image)
        files = list(disc_entries(disc))
        manifest['discs'].append(dict(role=label, bytes=disc.image_size, files=len(files)))
        for rel, size in files:
            origin = label + ':/' + rel
            with disc.open(rel)[0] as stream:
                base = stream.tell()
                view = View(size, lambda off, n: host_support.pread(stream.fileno(), n, base + off))
                head = view.read(0, min(size, 4))
                if head == b'RPF7':
                    archive(view, origin)
                elif head == b'rgxe':
                    effect(view, origin)
                elif head in (b'PIRS', b'LIVE', b'CON '):
                    title = int.from_bytes(view.read(0x360, 4), 'big')
                    content_type = int.from_bytes(view.read(0x344, 4), 'big')
                    disposition = package_disposition(title, args.title_id, content_type)
                    if disposition:
                        manifest['skipped_packages'].append(dict(origin=origin, reason=disposition))
                        continue
                    dest = out / 'packages' / f'package-{len(list((out / "packages").iterdir())):03d}'
                    disc_copy(disc, rel, dest)
                    manifest['packages'].append(dict(origin=origin, file=record_name(dest, out), bytes=size))
                    try:
                        with Package(dest, args.title_id) as package:
                            for entry in package.entries:
                                if entry.directory:
                                    continue
                                child = package_view(package, entry)
                                magic = child.read(0, min(4, child.size))
                                if magic == b'RPF7':
                                    archive(child, origin + '!/' + entry.path)
                                elif magic == b'rgxe':
                                    effect(child, origin + '!/' + entry.path)
                            package.check_unchanged()
                    except ValueError:
                        manifest['unresolved'].append(dict(origin=origin, reason='package-validation-refused'))
        print('DISC_EXTRACT', label, 'archives', len(manifest['archives']), 'effects', len(manifest['effects']),
              'containers', len(manifest['containers']), flush=True)
    manifest['elapsed_seconds'] = time.monotonic() - start
    manifest['status'] = 'incomplete' if manifest['unresolved'] else 'complete'
    write_json(out / 'inventory.json', manifest)
    print('EXTRACT_RESULT', manifest['status'], 'unresolved', len(manifest['unresolved']), flush=True)
    return 0


def read_pack(data):
    """Strict structural reader used for connected pack comparison, not execution."""
    if len(data) < 16:
        raise ValueError('short pack')
    magic, version, count, reserved = struct.unpack_from('<4sIII', data)
    if magic != b'GSP2' or version != 2 or reserved or count > 100000:
        raise ValueError('pack header refused')
    end = 16 + 152 * count
    if end > len(data):
        raise ValueError('pack table truncated')
    result = {}
    intervals = []
    for i in range(count):
        entry = data[16+i*152:16+(i+1)*152]
        key, stage, offset, words, elements = struct.unpack_from('<QIIII', entry)
        if f'{key:016x}' in result or stage > 1 or elements > 24 or not words or offset % 4 or offset < end or offset + words*4 > len(data):
            raise ValueError('pack entry refused')
        blob = data[offset:offset+words*4]
        if blob[:4] != b'\x03\x02\x23\x07':
            raise ValueError('pack module magic refused')
        result[f'{key:016x}'] = dict(stage=stage, metadata_sha256=sha(entry[8:12]+entry[20:]),
            spirv_sha256=sha(blob), bytes=len(blob))
        intervals.append((offset, offset+words*4))
    cursor = end
    for begin, finish in sorted(intervals):
        if begin != cursor:
            raise ValueError('pack overlapping or unowned payload')
        cursor = finish
    if cursor != len(data):
        raise ValueError('pack trailing bytes')
    return result


def compare_entries(actual, expected):
    same, different = [], []
    for key in sorted(actual.keys() & expected.keys()):
        (same if actual[key] == expected[key] else different).append(key)
    return dict(same=same, different=different, missing=sorted(expected.keys()-actual.keys()),
                extra=sorted(actual.keys()-expected.keys()))


def compare(args):
    result = compare_entries(read_pack(args.actual.read_bytes()), read_pack(args.expected.read_bytes()))
    write_json(args.output, result)
    print('PACK_COMPARE', ' '.join(f'{name}={len(values)}' for name, values in result.items()))
    return 0


def record_name(path, base) -> str:
    """A file's name in an inventory or a translation record: its path below the folder, with forward slashes on every
    host, so the records of one host are the records of another and a later stage can check the name's form."""
    return path.relative_to(base).as_posix()


def validate_inventory(directory):
    from generic_shader_pack import parse_container, canonical_microcode, fnv1a64
    directory = directory.resolve(strict=True)
    manifest = json.loads((directory / 'inventory.json').read_text())
    if manifest.get('schema') != 1 or manifest.get('source') != 'disc' or manifest.get('key_files_used') is not False:
        raise ValueError('inventory provenance refused')
    seen = set()
    canonical = {}
    result = []
    for r in manifest['containers']:
        path = directory / r['file']
        if path.is_symlink() or not path.resolve().is_relative_to(directory) or path.stat().st_size > 16 * 1024**2:
            raise ValueError('inventory path refused')
        blob = path.read_bytes()
        if len(blob) != r['bytes'] or sha(blob) != r['sha256'] or r['sha256'] in seen:
            raise ValueError('inventory container identity refused')
        seen.add(r['sha256'])
        mc, _, _, _ = parse_container(blob, r['stage'])
        canon = canonical_microcode(mc)
        key = f'{fnv1a64(canon):016x}'
        if r.get('key') != key or r.get('canonical_sha256') != sha(canon):
            raise ValueError('inventory runtime key refused')
        if key in canonical and canonical[key] != (r['stage'], sha(canon)):
            raise ValueError('runtime hash collision refused')
        canonical[key] = (r['stage'], sha(canon))
        result.append(r)
    if not result:
        raise ValueError('empty inventory refused')
    # Current runtime family wins duplicate keys; all other effects are still
    # translated and their provenance retained. No shipped pack is an input.
    def order(r):
        candidates = [(0 if '/fxl_final_final/' in o['origin'] else 1,
                       o['origin'], o['offset']) for o in r['origins']]
        return min(candidates) + (r['sha256'],)
    return manifest, sorted(result, key=order)


def require_bulk_priority(count):
    if count > 8 and host_support.effective_nice() <= 0:
        raise ValueError('bulk translation requires positive effective nice priority')


def worker(arguments):
    """Preserve the project CLI contract, adding the caught exception to private logs."""
    import xenos_translate_batch as converter
    original = converter.translate_batch
    def traced(*args, **kwargs):
        try:
            return original(*args, **kwargs)
        except (ValueError, OSError) as error:
            print('SHADER_WORKER_REASON', type(error).__name__, str(error), file=sys.stderr, flush=True)
            raise
    converter.translate_batch = traced
    try:
        return converter.main(arguments)
    finally:
        converter.translate_batch = original


def translate(args):
    from xenos_translate_batch import Toolchain, translate_batch, _require_validator
    inventory_hash = sha((args.inventory / 'inventory.json').read_bytes())
    manifest, selected = validate_inventory(args.inventory)
    config = shader_toolchain.load(getattr(args, 'toolchain', None), getattr(args, 'host_tools', None), getattr(args, 'ndk', None))
    required = shader_toolchain.BASE
    if set(config) != set(required):
        raise ValueError('toolchain fields refused')
    paths = {name: Path(value).resolve(strict=True) for name, value in config.items()}
    for name in ('translator', 'preflight', 'dxc', 'spirv_validator'):
        _require_validator(paths[name])
    if not paths['common_header'].is_file() or not paths['compiler_library_directory'].is_dir():
        raise ValueError('toolchain support files missing')
    toolchain = Toolchain(**paths)
    if args.limit is not None:
        if not 1 <= args.limit <= 8:
            raise ValueError('diagnostic limit must be 1..8')
        selected = selected[:args.limit]
    jobs = getattr(args, 'jobs', 1)
    if not 1 <= jobs <= 4:
        raise ValueError('compiler job count must be 1..4')
    if args.check:
        print('SHADER_STAGE_CHECK PASS', 'selected', len(selected), 'unresolved', len(manifest['unresolved']),
              'complete_inventory', manifest['status'] == 'complete', flush=True)
        return 0
    require_bulk_priority(len(selected))
    args.output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    receipts = []
    tool_hashes = {name: sha(path.read_bytes()) for name, path in paths.items() if path.is_file()}
    sources = [Path(__file__).resolve(), args.repo / 'tools/iter/generic_shader_pack.py',
               args.repo / 'tools/own_disc/disc_builder.py',
               *sorted((args.repo / 'tools/rpf7_inventory').glob('*.py'))]
    source_hashes = {str(path): sha(path.read_bytes()) for path in sources}
    # Bind dynamic DXC libraries too, never a key directory or a whole game tree.
    tool_hashes['compiler_libraries'] = {p.name: sha(p.read_bytes())
        for p in sorted(paths['compiler_library_directory'].iterdir()) if p.is_file()}
    write_json(args.output / 'run-inputs.json', dict(inventory_sha256=inventory_hash,
        source_hashes=source_hashes, tool_hashes=tool_hashes, jobs=jobs,
        effective_nice=host_support.effective_nice()))
    parts = [selected[begin:begin + 60] for begin in range(0, len(selected), 60)]

    def convert_batch(batch):
        part = parts[batch]
        directory = args.output / f'records-{batch:03d}'
        inputs = [args.inventory / r['file'] for r in part]
        if len(parts) == 1 or jobs == 1:
            return translate_batch(inputs, directory, toolchain=toolchain, repository=args.repo,
                                   max_containers=60, constant_binding='descriptor32')
        # Each child is single-threaded when its bounded worker uses preexec_fn.
        # The parent only starts subprocesses without preexec_fn from its threads.
        command = [sys.executable, str(Path(__file__).resolve()), '--repo', str(args.repo), 'worker',
                   str(directory), *map(str, inputs), '--max-containers', '60',
                   '--constant-binding', 'descriptor32']
        for name, path in paths.items():
            command += ['--' + name.replace('_', '-'), str(path)]
        write_json(args.output / f'invocation-{batch:03d}.json', dict(argv=command, cwd=str(args.repo)))
        environment = os.environ.copy()
        environment['PYTHONDONTWRITEBYTECODE'] = '1'
        completed = subprocess.run(command, cwd=args.repo, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (args.output / f'batch-{batch:03d}.log').write_bytes(completed.stdout)
        if completed.returncode not in (0, 1) or not (directory / 'report.json').is_file():
            raise ValueError('translation worker failed; preserve partial batch')
        report = json.loads((directory / 'report.json').read_text())
        print('TRANSLATE_WORKER', batch, 'validated', report['validated_container_count'], 'of', len(part), flush=True)
        return report

    # All processes use the inherited, verified positive nice level. At most
    # four DXC/translator/validator children can execute at any instant.
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        reports = list(executor.map(convert_batch, range(len(parts))))
    for batch, (part, report) in enumerate(zip(parts, reports)):
        directory = args.output / f'records-{batch:03d}'
        identities = json.loads((directory / 'artifact-identities.json').read_text())['artifacts']
        validated = {r['index']: r for r in identities}
        for original, rec in zip(part, report['records']):
            record_dir = directory / f"shader-{rec['index']:04d}"
            if sha((record_dir / 'container.xenos').read_bytes()) != original['sha256']:
                raise ValueError('translated container changed from inventory')
            if rec['status'] == 'validated' and (
                    validated[rec['index']]['container_sha256'] != original['sha256'] or
                    sha((record_dir / 'translated.spv').read_bytes()) != validated[rec['index']]['spirv_sha256']):
                raise ValueError('validated translation identity changed')
            receipts.append(dict(container_sha256=original['sha256'], key=original['key'], stage=original['stage'],
                status=rec['status'], record=record_name(directory / f"shader-{rec['index']:04d}", args.output)))
        print('TRANSLATE_BATCH', batch, 'validated', report['validated_container_count'], 'of', len(part), flush=True)
    # The actual production packer performs metadata generation and canonical
    # hashing. Do not fabricate a second, subtly different GSP2 producer here.
    relative = args.output.relative_to(args.repo)
    command = [sys.executable, str(args.repo / 'tools/iter/generic_shader_pack.py'), '--out',
               str(args.output / 'generic-shaders.bin'), '--corpus', str(relative / 'records-*/shader-*')]
    completed = subprocess.run(command, cwd=args.repo, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (args.output / 'packer.log').write_bytes(completed.stdout)
    if completed.returncode:
        raise ValueError('production packer failed')
    entries = read_pack((args.output / 'generic-shaders.bin').read_bytes())
    failures = sum(r['status'] != 'validated' for r in receipts)
    if any(sha(Path(path).read_bytes()) != digest for path, digest in source_hashes.items()):
        raise ValueError('translation implementation changed during run')
    if any(sha(paths[name].read_bytes()) != digest for name, digest in tool_hashes.items()
           if name != 'compiler_libraries'):
        raise ValueError('translation tool changed during run')
    if sha((args.inventory / 'inventory.json').read_bytes()) != inventory_hash:
        raise ValueError('inventory changed during translation')
    current_libraries = {p.name: sha(p.read_bytes())
        for p in sorted(paths['compiler_library_directory'].iterdir()) if p.is_file()}
    if current_libraries != tool_hashes['compiler_libraries']:
        raise ValueError('compiler libraries changed during translation')
    expected_keys = {r['key'] for r in receipts if r['status'] == 'validated'}
    if set(entries) != expected_keys:
        raise ValueError('production pack lost validated runtime keys')
    complete = manifest['status'] == 'complete' and not manifest['unresolved'] and not failures and args.limit is None
    result = dict(schema=1, status='complete' if complete else 'incomplete',
        selected=len(selected), validated=len(receipts)-failures, failed=failures, entries=len(entries),
        inventory_sha256=inventory_hash,
        pack_sha256=sha((args.output/'generic-shaders.bin').read_bytes()),
        tool_hashes=tool_hashes, source_hashes=source_hashes, records=receipts, unresolved=manifest['unresolved'],
        jobs=jobs, effective_nice=host_support.effective_nice(), python_version=sys.version,
        elapsed_seconds=time.monotonic()-started, semantic_equivalence_verified=False,
        gpu_execution=False, diagnostic_limit=args.limit)
    write_json(args.output / 'translation.json', result)
    print('SHADER_STAGE_RESULT', result['status'], 'translated', result['validated'], 'entries', len(entries), flush=True)
    # A diagnostic pack is preserved, but cannot authorize a complete builder
    # stage receipt until unresolved source/translation coverage is settled.
    # --unresolved-ok: every selected container translated, but the disc also holds effects the inventory could not
    # resolve. A later `select` then decides coverage: it refuses unless every key a build needs is in the pack.
    return 0 if complete or (getattr(args, 'unresolved_ok', False) and not failures and args.limit is None) else 1


def select(args):
    """The pack a build is pinned to: exactly the listed runtime keys, written by the production packer from the
    records of a full translation. The packer sorts entries by key and the first record of a key wins, so the same
    key list over the same translation gives the same file on any host. A key list holds hashes only."""
    listed = json.loads(args.keys.read_text())
    keys = listed.get('keys') if isinstance(listed, dict) else None
    if not isinstance(listed, dict) or listed.get('schema') != 'shader-keys/1' or not isinstance(keys, list) or not keys \
            or not all(isinstance(k, str) for k in keys) or keys != sorted(set(keys)):
        raise ValueError('key list refused')
    report_bytes = (args.translation / 'translation.json').read_bytes()
    wanted = set(keys)
    chosen = [r for r in json.loads(report_bytes)['records'] if r['key'] in wanted]
    if any(r['status'] != 'validated' for r in chosen) or {r['key'] for r in chosen} != wanted:
        raise ValueError('a listed key has no validated translation')
    args.output.mkdir(parents=True)
    for r in chosen:                      # plain copies under the same names: the packer's order is by name
        source = (args.translation / r['record']).resolve(strict=True)
        if not source.is_relative_to(args.translation) or not re.fullmatch(r'records-\d+/shader-\d+', r['record']):
            raise ValueError('record path refused')
        copy = args.output / 'selected' / r['record']
        copy.mkdir(parents=True)
        for name in ('container.xenos', 'translated.spv', 'checkpoint.json'):
            shutil.copyfile(source / name, copy / name)
    relative = args.output.relative_to(args.repo)
    command = [sys.executable, str(args.repo / 'tools/iter/generic_shader_pack.py'), '--out',
               str(args.output / 'generic-shaders.bin'), '--corpus', str(relative / 'selected/records-*/shader-*')]
    completed = subprocess.run(command, cwd=args.repo, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (args.output / 'packer.log').write_bytes(completed.stdout)
    if completed.returncode:
        raise ValueError('production packer failed')
    data = (args.output / 'generic-shaders.bin').read_bytes()
    if set(read_pack(data)) != wanted:
        raise ValueError('production pack does not hold exactly the listed keys')
    digest = sha(data)
    same = args.expect_sha256 in (None, digest)
    write_json(args.output / 'selection.json', dict(schema=1, status='complete' if same else 'differs', keys=len(keys),
        records=len(chosen), keys_sha256=sha(args.keys.read_bytes()), translation_sha256=sha(report_bytes),
        pack_sha256=digest, pack_bytes=len(data), expected_sha256=args.expect_sha256))
    print('SHADER_SELECT_RESULT', 'complete' if same else 'differs', 'keys', len(keys), 'pack', digest[:12], flush=True)
    return 0 if same else 1


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    sub = parser.add_subparsers(dest='command', required=True)
    wp = sub.add_parser('worker')
    wp.add_argument('arguments', nargs=argparse.REMAINDER)
    ep = sub.add_parser('extract')
    for name in ('play', 'install', 'generated', 'xmem', 'output'):
        ep.add_argument('--'+name, type=Path, required=True)
    ep.add_argument('--key-address', type=lambda s: int(s, 0), required=True)
    ep.add_argument('--title-id', type=lambda s: int(s, 0), required=True)
    tp = sub.add_parser('translate')
    for name in ('inventory', 'output'):
        tp.add_argument('--'+name, type=Path, required=True)
    for name in ('toolchain', 'host-tools', 'ndk'):      # the toolchain as a file of paths, or named from the two folders
        tp.add_argument('--'+name, type=Path)
    tp.add_argument('--check', action='store_true')
    tp.add_argument('--limit', type=int)
    tp.add_argument('--jobs', type=int, default=4)
    tp.add_argument('--unresolved-ok', action='store_true',
                    help='exit 0 although the inventory lists unresolved effects, if every selected container translated')
    cp = sub.add_parser('compare')
    for name in ('actual', 'expected', 'output'):
        cp.add_argument('--'+name, type=Path, required=True)
    sp = sub.add_parser('select')
    for name in ('translation', 'keys', 'output'):
        sp.add_argument('--'+name, type=Path, required=True)
    sp.add_argument('--expect-sha256')
    args = parser.parse_args()
    args.repo = args.repo.resolve()
    for path in ('tools/own_disc', 'tools/rpf7_inventory', 'tools/host_guest_trace', 'tools/iter'):
        sys.path.insert(0, str(args.repo / path))
    if args.command == 'worker':
        return worker(args.arguments)
    from disc_builder import check_private_path
    args.output = args.output.absolute()
    check_private_path(args.output, 'shader stage output')
    if args.output.is_symlink() or args.output != args.output.resolve():
        raise ValueError('output symlink refused')
    if args.command == 'translate':
        args.inventory = args.inventory.resolve()
    if args.command == 'select':
        args.translation = args.translation.resolve()
    return {'extract': extract, 'compare': compare, 'translate': translate, 'select': select}[args.command](args)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError):
        print('SHADER_STAGE_REFUSED: input, tool, or extent validation failed; partial outputs preserved', file=sys.stderr)
        raise SystemExit(2)
