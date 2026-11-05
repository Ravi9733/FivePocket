#!/usr/bin/env python3
"""Compile and link the game library on any host from a frozen command list, with every input accounted for.

compile_rehearsal.py's compile step derives its commands from this Mac's private build tree (a CMake folder, ninja,
plans with this Mac's paths). Another host has none of that. freeze_build.py wrote a finished build down as a
manifest: every command, every file the commands read with its SHA-256, the link. This tool makes the library from
that manifest on any host:

  1. Every input the manifest names in the first run's work folder is looked for in THIS run's work folder. If it is
     there with the same bytes (an earlier stage of this run wrote it: the decoder's headers, the shader packs, the
     code headers), it stays. If not, its bytes must be found, by SHA-256, in a stage output of this run (--stage) or
     in a supplied folder or file (--supplied); a copy is laid where the commands expect it and its origin is
     recorded. An input found nowhere refuses the run: nothing is taken from anywhere else.
  2. replay_build.py compiles every unit (the work folder of the first run mapped to this run's).
  3. The linker script that binds the generated wrappers to their bodies is made from this run's own objects
     (compile_rehearsal.redirect_script) and must be the one the manifest names.
  4. replay_build.py links. 5. The compiler's name strings are removed (the portable library).

    portable_compile.py --manifest FILE --frozen-work REL --repo DIR --work DIR --ndk DIR
                        [--stage NAME=DIR ...] [--supplied PATH ...] [--made-later REL ...] [--host-built REL=HEX ...]
                        [--jobs N]

--host-built names an input that is a library an earlier stage of this run built with this host's compiler, with its
archive identity (elf_sections.py --archive). Such a library has the first machine's bytes, or the same objects with
this host's compiler name in them (the NDK's compiler names itself differently per host): either is accepted, anything
else refuses the run. The compiler's name strings are not in the final library on any host.

--frozen-work is the first run's work folder as a path below its root, as the manifest's inputs name it. --stage and
--supplied folders are read, never written. Outputs, all in the work folder: compile/ (the laid-out inputs, the
objects, the linked library, libgame-portable.so) and compile/portable-compile.json (where every input came from,
the units, the hashes). Exit 0: built; 1: a step failed; 2: refused before compiling.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import host_support  # noqa: E402
import elf_sections  # noqa: E402

REDIRECT = 'compile/redirect.ld'
PORTABLE = 'libgame-portable.so'


def sha_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def below(rel: str) -> bool:
    return bool(rel) and not rel.startswith('/') and '\\' not in rel and '..' not in PurePosixPath(rel).parts


def index(folders: list[tuple[str, Path]]) -> dict:
    """SHA-256 -> (label, file) for every file of the folders (or single files); the first folder named wins."""
    found = {}
    for label, base in folders:
        files = [base] if base.is_file() else sorted(p for p in base.rglob('*') if p.is_file() and not p.is_symlink()) if base.is_dir() else None
        if files is None:
            raise ValueError(f'{label}: {base} is not there')
        for path in files:
            found.setdefault(sha_file(path), (label, path))
    return found


def lay_out(manifest: dict, frozen: str, work: Path, stages: list[tuple[str, Path]], supplied: list[tuple[str, Path]], later: set,
            host_built: dict | None = None) -> dict:
    """Puts every input of the first run's work folder into this run's, by content. Returns the record of origins."""
    host_built = host_built or {}
    wanted = {rel[len(frozen) + 1:]: want for rel, want in manifest['inputs'].items() if rel.startswith(frozen + '/')}
    if not wanted:
        raise ValueError('the manifest names no input in ' + frozen)
    unknown = later - set(wanted)
    if unknown:
        raise ValueError('--made-later names no input of the manifest: ' + ', '.join(sorted(unknown)))
    unknown = set(host_built) - set(wanted)
    if unknown:
        raise ValueError('--host-built names no input of the manifest: ' + ', '.join(sorted(unknown)))
    origins, missing, to_place = {}, [], []
    for rel, want in sorted(wanted.items()):
        if not below(rel):
            raise ValueError('the manifest names an input outside the work folder: ' + rel)
        if rel in later:
            origins[rel] = 'made later from this run\'s objects'
            continue
        here = work.joinpath(*PurePosixPath(rel).parts)
        if here.is_file() and not rel.startswith('compile/'):
            if here.stat().st_size != want['size'] or sha_file(here) != want['sha256']:
                found = archive_identity(here) if rel in host_built else ''
                if not found or found != host_built[rel]:          # (a file that is no library of objects has no identity)
                    raise ValueError(f'{rel}: this run\'s file is not the one the commands were written for')
                origins[rel] = 'this run, in place (the first machine\'s objects with this host\'s compiler name in them)'
                continue
            origins[rel] = 'this run, in place'
            continue
        to_place.append((rel, want, here))
    if to_place:
        from_stage, from_supplied = index(stages), index(supplied)
        for rel, want, here in to_place:
            label_path = from_stage.get(want['sha256']) or from_supplied.get(want['sha256'])
            if label_path is None:
                missing.append(rel)
                continue
            label, source = label_path
            if here.exists() and (here.stat().st_size != want['size'] or sha_file(here) != want['sha256']):
                raise ValueError(f'{rel}: a different file lies there already')
            here.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, here)
            origins[rel] = label
    if missing:
        raise ValueError(f'{len(missing)} inputs of the compile are neither a stage output of this run nor a supplied file: '
                         + ', '.join(missing[:8]) + (' ...' if len(missing) > 8 else ''))
    return origins


def archive_identity(path: Path) -> str:
    """The archive identity of a file, or '' when it is no archive of objects."""
    try:
        return elf_sections.archive_identity(path)[0]
    except (ValueError, OSError):
        return ''


def replay(manifest_path: Path, repo: Path, compiler: Path, frozen: str, work: Path, jobs: int, extra: list[str]) -> dict:
    state = work / 'compile' / 'replay'
    command = [sys.executable, '-B', str(HERE / 'replay_build.py'), '--manifest', str(manifest_path), '--root', str(repo),
               '--compiler', str(compiler), '--jobs', str(jobs), '--map', f'{frozen}={work}', '--state-dir', str(state), *extra]
    log = work / 'compile' / ('replay-compile.log' if '--no-link' in extra else 'replay-link.log')
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('wb') as out:
        done = subprocess.run(command, stdout=out, stderr=subprocess.STDOUT)
    results = state / 'replay-results.json'
    if done.returncode != 0 or not results.is_file():
        raise RuntimeError(f'the replay ended with {done.returncode}; log: {log}')
    return json.loads(results.read_text(encoding='utf-8'))


def build(manifest_path: Path, frozen: str, repo: Path, work: Path, ndk: Path, stages, supplied, later: set, jobs: int = 4,
          platform: str = sys.platform, host_built: dict | None = None) -> dict:
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    frozen = frozen.replace('\\', '/').strip('/')
    if not below(frozen):
        raise ValueError('--frozen-work wants a path below the first run\'s root')
    host = ndk / 'toolchains' / 'llvm' / 'prebuilt' / host_support.ndk_host_tag(platform)
    suffix = '.exe' if platform == 'win32' else ''
    nm, objcopy = host / 'bin' / ('llvm-nm' + suffix), host / 'bin' / ('llvm-objcopy' + suffix)
    for tool in (nm, objcopy):
        if not tool.is_file():
            raise ValueError(f'the NDK tool is missing: {tool}')
    if REDIRECT in {rel[len(frozen) + 1:] for rel in manifest['inputs'] if rel.startswith(frozen + '/')} and REDIRECT not in later:
        raise ValueError(f'{REDIRECT} is made from this run\'s objects: name it with --made-later')
    host_built = host_built or {}
    origins = lay_out(manifest, frozen, work, stages, supplied, later, host_built)
    recognised = [arg for rel, identity in sorted(host_built.items()) for arg in ('--input-identity', f'{frozen}/{rel}={identity}')]
    report = {'schema': 'portable-compile/1', 'manifest_sha256': sha_file(manifest_path), 'frozen_work': frozen,
              'inputs': len(origins), 'origins': {}, 'supplied_inputs': sorted(r for r, o in origins.items() if o.startswith('supplied:'))}
    for origin in origins.values():
        report['origins'][origin] = report['origins'].get(origin, 0) + 1
    pending = [arg for rel in sorted(later) for arg in ('--pending-input', f'{frozen}/{rel}')]
    compiled = replay(manifest_path, repo, host, frozen, work, jobs, ['--no-link', *pending, *recognised])     # (a unit that fails ends the replay with 1)
    report['units'] = {'total': compiled['units_total'], 'same_bytes_as_first_machine': compiled['same_bytes']}
    link_rel = manifest['link']['library'][len(manifest['repo_root']) + 1:]
    if not link_rel.startswith(frozen + '/'):
        raise ValueError('the manifest links outside the first run\'s work folder')
    linked = work.joinpath(*PurePosixPath(link_rel[len(frozen) + 1:]).parts)
    if REDIRECT in later:
        rehearsal = module('portable_compile_rehearsal', HERE / 'compile_rehearsal.py')
        objects = sorted((linked.parent / 'obj').glob('*.o'))
        text, hooked = rehearsal.redirect_script(nm, objects)
        script = work.joinpath(*PurePosixPath(REDIRECT).parts)
        script.write_text(text, encoding='utf-8', newline='\n')
        want = manifest['inputs'][f'{frozen}/{REDIRECT}']
        report['redirect'] = {'sha256': sha_file(script), 'assignments': text.count('\n') - 1, 'hooked_wrappers_left_alone': hooked,
                              'objects': len(objects)}
        if report['redirect']['sha256'] != want['sha256']:
            raise RuntimeError('the linker script made from this run\'s objects is not the one the link was written for')
    link = replay(manifest_path, repo, host, frozen, work, jobs, recognised)['link']              # (a link that fails ends the replay with 1)
    report['linked'] = {'path': str(linked), 'sha256': link['library_sha256'], 'same_bytes_as_first_machine': link['same_bytes']}
    rehearsal = module('portable_compile_rehearsal', HERE / 'compile_rehearsal.py')
    out = linked.parent / PORTABLE
    rehearsal.portable_library(objcopy, linked, out)
    report['library'] = {'path': str(out), 'sha256': sha_file(out), 'bytes': out.stat().st_size}
    (work / 'compile' / 'portable-compile.json').write_text(json.dumps(report, indent=1, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
    return report


def named(items: list[str], what: str) -> list[tuple[str, Path]]:
    out = []
    for item in items:
        if '=' not in item:
            raise ValueError(f'{what} wants NAME=PATH, got {item!r}')
        name, path = item.split('=', 1)
        out.append((f'{what.lstrip("-")}:{name}', Path(path)))
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--manifest', type=Path, required=True)
    ap.add_argument('--frozen-work', required=True)
    ap.add_argument('--repo', type=Path, required=True)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--ndk', type=Path, required=True)
    ap.add_argument('--stage', action='append', default=[], metavar='NAME=DIR')
    ap.add_argument('--supplied', action='append', default=[], metavar='NAME=PATH')
    ap.add_argument('--made-later', action='append', default=[], metavar='REL')
    ap.add_argument('--host-built', action='append', default=[], metavar='REL=HEX')
    ap.add_argument('--jobs', type=int, default=4)
    a = ap.parse_args(argv)
    try:
        host_built = dict(item.split('=', 1) for item in a.host_built if '=' in item)
        if len(host_built) != len(a.host_built) or any(len(v) != 64 or set(v) - set('0123456789abcdef') for v in host_built.values()):
            raise ValueError('--host-built wants REL=HEX (the archive identity, 64 hexadecimal digits)')
        report = build(a.manifest, a.frozen_work, a.repo.resolve(), a.work.resolve(), a.ndk, named(a.stage, '--stage'),
                       named(a.supplied, '--supplied'), set(a.made_later), a.jobs, host_built=host_built)
    except ValueError as error:
        print('PORTABLE_COMPILE_REFUSED:', error, file=sys.stderr)
        return 2
    except (RuntimeError, OSError) as error:
        print('PORTABLE_COMPILE_FAILED:', error, file=sys.stderr)
        return 1
    print('PORTABLE_COMPILE', f'{report["units"]["total"]} units ({report["units"]["same_bytes_as_first_machine"]} byte-identical to the first machine),',
          'library', report['library']['sha256'], flush=True)
    for origin, count in sorted(report['origins'].items()):
        print('  inputs', count, origin, flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
