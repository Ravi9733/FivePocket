#!/usr/bin/env python3
"""Write the sources folder that goes with a builder package: the sources of the host programs inside it, as they were
when those programs were built.

    make_sources.py --generator TREE --generator-commit HEX --generator-diff-sha256 HEX
                    --host-src TAR --host-src-sha256 HEX [--leave-out PREFIX ...] [--forbid TEXT ...] --out DIR

Two inputs, both checked against what the build recorded:
  TREE  the git checkout the generator (XenonRecomp) was built from. Its HEAD must be the given commit and the
        SHA-256 of `git diff` (this project's patches to tracked files) the given one. Taken: every tracked file as
        it stands in the checkout, and every untracked file git does not ignore (this project's added tests). Build
        folders and anything else git ignores stay out.
  TAR   the archive of the other host programs' sources that the build machine was given (the shader translator's
        base tree and the changed files of its states, the two small tools, the build script and its support
        files). Its SHA-256 must be the given one. Members below a --leave-out prefix stay out (files that describe
        one machine).

Nothing is fetched and nothing is compared with an upstream copy: the claim is "these are the bytes that were built",
and the two hashes are what ties it to the build records. A link is not written; it is listed in the record. A file
that is text and holds one of the --forbid strings (a user name, a machine's folder) stops the run, and so does a
member name that leaves the folder.

Written into DIR (which must not exist): XenonRecomp/..., host-programs/..., sources.json (the identities above,
every file with its SHA-256, the links and the members left out) and CHANGES.txt: which files of the generator this
project changed or added (read from the checkout: `git diff --name-only` and the untracked files, each with the day
the file was last written), and which files of the archive lie outside its base tree (--base-prefix, default
xenos/base: everything else in the archive is this project's or a changed file of a state). --note FILE: a text
that is put at the end of CHANGES.txt as it is (what the tool cannot read from its inputs, such as the base tree's
own changes against its upstream); it is searched for the --forbid strings like every other text. Exit 0: written;
2: refused, nothing kept.
Nothing here reads the game.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tarfile
import time
from pathlib import Path, PurePosixPath

SCHEMA = 'builder-sources/1'


class Refusal(Exception):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git(tree: Path, *args: str) -> bytes:
    done = subprocess.run(['git', '-C', str(tree), *args], capture_output=True)
    if done.returncode != 0:
        raise Refusal(f'git {" ".join(args)} failed in {tree}: {done.stderr.decode(errors="replace").strip()[:200]}')
    return done.stdout


def safe_name(name: str) -> PurePosixPath:
    path = PurePosixPath(name)
    parts = [part for part in path.parts if part != '.']
    if path.is_absolute() or not parts or any(part == '..' for part in parts):
        raise Refusal(f'a name leaves the folder: {name}')
    return PurePosixPath(*parts)


def check_text(name: str, data: bytes, forbidden: list) -> None:
    if b'\0' in data[:8192]:
        return                                          # not text: a picture, an archive
    for word in forbidden:
        if word.encode('utf-8') in data:
            raise Refusal(f'{name} holds "{word}"')


def generator_files(tree: Path, commit: str, diff_sha256: str) -> tuple[dict, list]:
    head = git(tree, 'rev-parse', 'HEAD').decode().strip()
    if head != commit:
        raise Refusal(f'the generator checkout is at {head[:12]}, not at {commit[:12]}')
    found = sha256(git(tree, 'diff'))
    if found != diff_sha256:
        raise Refusal(f'the generator checkout\'s changes are not the recorded ones (found {found[:16]}, recorded {diff_sha256[:16]})')
    names = git(tree, 'ls-files', '-z', '--cached', '--others', '--exclude-standard').split(b'\0')
    files, links = {}, []
    for raw in sorted(set(name for name in names if name)):
        name = safe_name(raw.decode('utf-8')).as_posix()
        path = tree / name
        if path.is_symlink():
            links.append(name)
        elif path.is_file():
            files[name] = path.read_bytes()
        elif not path.exists():
            raise Refusal(f'a tracked file is missing from the generator checkout: {name}')
    day = lambda name: time.strftime('%Y-%m-%d', time.gmtime((tree / name).stat().st_mtime))
    changed = sorted(name.decode('utf-8') for name in git(tree, 'diff', '--name-only', '-z').split(b'\0') if name)
    added = sorted(name.decode('utf-8') for name in git(tree, 'ls-files', '-z', '--others', '--exclude-standard').split(b'\0') if name)
    changes = {'changed': [{'path': name, 'last_written': day(name)} for name in changed if name in files],
               'added': [{'path': name, 'last_written': day(name)} for name in added if name in files]}
    return files, links, changes


def changes_text(commit: str, changes: dict, host: dict, base_prefix: str, note: str = '') -> str:
    base = base_prefix.rstrip('/') + '/'
    out = ['Changes made by this project to the sources in this folder', '=' * 58, '',
           f'XenonRecomp/ is the upstream tree at commit {commit} with these files changed by this project',
           '(the day is when the file was last written in the checkout the program was built from):']
    out += [f'  changed  {entry["last_written"]}  {entry["path"]}' for entry in changes['changed']] or ['  (none)']
    out += ['and these files added by this project:']
    out += [f'  added    {entry["last_written"]}  {entry["path"]}' for entry in changes['added']] or ['  (none)']
    out += ['', f'host-programs/{base} is the base tree of the other programs as it was built. Every file of host-programs/',
            'outside it is this project\'s own or a file of the base tree as this project changed it for one build:']
    out += [f'  {name}' for name in sorted(host) if not name.startswith(base)] or ['  (none)']
    if note:
        out += ['', note.rstrip('\n')]
    return '\n'.join(out) + '\n'


def host_files(archive: Path, archive_sha256: str, leave_out: list) -> tuple[dict, list, list]:
    found = sha256(archive.read_bytes())
    if found != archive_sha256:
        raise Refusal(f'{archive.name} is not the recorded archive (found {found[:16]}, recorded {archive_sha256[:16]})')
    files, links, left = {}, [], []
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            if member.isdir():
                continue
            name = safe_name(member.name).as_posix()
            if any(name == prefix.rstrip('/') or name.startswith(prefix.rstrip('/') + '/') for prefix in leave_out):
                left.append(name)
            elif member.issym() or member.islnk():
                links.append(name)
            elif member.isfile():
                files[name] = tar.extractfile(member).read()
            else:
                raise Refusal(f'{archive.name} holds something that is neither a file nor a link: {name}')
    return files, sorted(links), sorted(left)


def make(generator: Path, commit: str, diff_sha256: str, host_src: Path, host_sha256: str, leave_out: list, forbidden: list, out: Path,
         base_prefix: str = 'xenos/base', note: Path | None = None) -> dict:
    if out.exists():
        raise Refusal(f'the output folder exists already: {out}')
    gen_files, gen_links, changes = generator_files(generator, commit, diff_sha256)
    host, host_links, left = host_files(host_src, host_sha256, leave_out)
    if not gen_files or not host:
        raise Refusal('one of the two source sets is empty')
    note_text = note.read_text(encoding='utf-8') if note else ''
    check_text(f'the note {note.name if note else ""}', note_text.encode('utf-8'), forbidden)
    written = {}
    for folder, files in (('XenonRecomp', gen_files), ('host-programs', host)):
        for name, data in files.items():
            check_text(f'{folder}/{name}', data, forbidden)
            written[f'{folder}/{name}'] = data
    record = {'schema': SCHEMA,
              'generator': {'commit': commit, 'diff_sha256': diff_sha256, 'files': len(gen_files), 'links_not_written': gen_links, **changes},
              'host_programs': {'archive_sha256': host_sha256, 'files': len(host), 'links_not_written': host_links, 'left_out': left},
              'files': [{'path': name, 'sha256': sha256(data), 'size': len(data)} for name, data in sorted(written.items())]}
    try:
        for name, data in written.items():
            target = out / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (out / 'sources.json').write_text(json.dumps(record, indent=1) + '\n', encoding='utf-8')
        (out / 'CHANGES.txt').write_text(changes_text(commit, changes, host, base_prefix, note_text), encoding='utf-8')
    except OSError:
        shutil.rmtree(out, ignore_errors=True)
        raise
    return record


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description='Write the sources folder of a builder package from the checked build inputs.')
    parser.add_argument('--generator', required=True, type=Path)
    parser.add_argument('--generator-commit', required=True)
    parser.add_argument('--generator-diff-sha256', required=True)
    parser.add_argument('--host-src', required=True, type=Path)
    parser.add_argument('--host-src-sha256', required=True)
    parser.add_argument('--leave-out', action='append', default=[])
    parser.add_argument('--forbid', action='append', default=[])
    parser.add_argument('--base-prefix', default='xenos/base')
    parser.add_argument('--note', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        record = make(args.generator, args.generator_commit, args.generator_diff_sha256, args.host_src, args.host_src_sha256,
                      args.leave_out, args.forbid, args.out, args.base_prefix, args.note)
    except (Refusal, OSError, tarfile.TarError, UnicodeDecodeError) as error:
        print(f'sources: refused: {error}', file=sys.stderr)
        return 2
    print(f'sources: {record["generator"]["files"]} generator files and {record["host_programs"]["files"]} host-program files written to {args.out}; '
          f'{len(record["host_programs"]["left_out"])} left out, {len(record["generator"]["links_not_written"]) + len(record["host_programs"]["links_not_written"])} links not written')
    return 0


if __name__ == '__main__':
    sys.exit(main())
