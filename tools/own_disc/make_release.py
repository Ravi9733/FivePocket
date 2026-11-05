#!/usr/bin/env python3
"""Write the inventory of a builder package (release.json, builder-release/1): what builder.py checks before it runs.

A package is a folder: the launcher, a Python, the tools, the host programs, the profile and the files the profile
names. The front end (builder.py) refuses to run unless every file of the inventory is there with the recorded hash,
the profile and the Python are among them, and the compiler archive is pinned by address, size and hash. This tool
writes that inventory for a finished package folder and reads it back the way the front end does.

    make_release.py --package DIR --host HOST --profile REL [--python REL]
                    --ndk-url URL --ndk-sha256 HEX --ndk-size N --ndk-directory NAME --ndk-unpacked-bytes N
                    [--ndk-image-root PATH]
                    --min-free-bytes N [--build-time TEXT] [--dxc PINS.json] [--output FILE]
                    [--target ios=REL] [--target duo=REL] [--target thor=REL] [--target fold=REL]

Every regular file below DIR is listed (name with forward slashes, SHA-256, whether it is a program), except
Python's own cache folders (the inventory is written after the files are read, so it never lists itself). A link or a name the front end would refuse stops the whole run
(two names that differ only by case are refused by the front end's reader). The output (default DIR/release.json)
must not exist. HOST is the host the package
is for, as builder.py names it (win32-x86_64, darwin-arm64, ...); when it is this host, the inventory is loaded once
with the front end's own reader before the tool reports success. Nothing here reads the game.

--ndk-image-root: for the macOS NDK, which Google publishes only as a disk image (.dmg), the NDK folder inside the
image (AndroidNDK<build>.app/Contents/NDK); it is installed as --ndk-directory.

Without --python the package carries no Python: the launcher runs the host's own (the Mac release: Xcode's Python 3.9 with
tools/own_disc/macos/python-compat), and the front end gives its stages the Python it runs in.

--target NAME=REL (2026-11-05, the Mac release with the iOS apps): another profile of the package, built by builder build
--NAME (ios, duo); written as "profiles" and, like the default profile, it must be a file of the package.

--dxc names the pins of a shader compiler release the player's builder fetches from its publisher (address, size and
hash of the archive, the licence address, and for every file taken out of it its name in the archive, its place in
the package, its size and hash; tools/own_disc/windows/dxc-v1.9.2607.json). They are written into the inventory as
they are. A package that already holds a file at one of those places is refused: what the builder fetches is not
handed out with the package.
Exit 0: written; 2: refused, nothing written.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import builder  # noqa: E402
import disc_builder as db  # noqa: E402

SKIPPED_FOLDERS = ('__pycache__',)
PROGRAM_SUFFIXES = ('.exe',)
LICENSE_URL = 'https://developer.android.com/ndk/downloads'


def is_program(path: Path, host: str) -> bool:
    """A file the package starts: by its suffix in a Windows package, by its mode elsewhere."""
    if host.startswith('win32'):
        return path.suffix.lower() in PROGRAM_SUFFIXES
    return bool(path.stat().st_mode & 0o111)


def inventory(package: Path, host: str) -> list:
    files = []
    for path in sorted(package.rglob('*'), key=lambda p: p.relative_to(package).as_posix()):
        rel = path.relative_to(package)
        if any(part in SKIPPED_FOLDERS for part in rel.parts):
            continue
        if path.is_symlink():
            raise ValueError(f'a link is in the package: {rel.as_posix()}')
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError(f'not a plain file: {rel.as_posix()}')
        name = rel.as_posix()
        try:
            builder.safe_name(name)
        except builder.Refusal:
            raise ValueError(f'the front end would refuse this name: {name}') from None
        record = {'path': name, 'sha256': db.sha256_file(path)}
        if is_program(path, host):
            record['executable'] = True
        files.append(record)
    if not files:
        raise ValueError('the package is empty')
    return files


def fetched_compiler(dxc, listed) -> dict:
    """The pins of the shader compiler the builder fetches, as the front end wants them, and none of its files in the
    package (names compared without regard to case, as the front end compares them)."""
    try:
        builder.dxc_spec({'dxc': dxc})
    except builder.Refusal as error:
        raise ValueError(f'the shader compiler is not pinned as the front end wants it: {error}') from None
    held = {name.casefold() for name in listed}
    for record in dxc['files']:
        if record['path'].casefold() in held:
            raise ValueError(f'the package holds a file the builder is to fetch from its publisher: {record["path"]}')
    return dxc


def make(package: Path, host: str, profile: str, python: str, ndk: dict, min_free_bytes: int, build_time: str | None, output: Path, dxc: dict | None = None,
         targets: dict | None = None) -> dict:
    package = package.resolve()
    output = output.resolve()
    if not package.is_dir():
        raise ValueError('the package folder is not there')
    if output.exists():
        raise ValueError('the inventory exists already: nothing is written over')
    if output.parent != package:
        raise ValueError('the inventory lies at the package root (the files are named from its folder)')
    if type(min_free_bytes) is not int or min_free_bytes <= 0:
        raise ValueError('the free space a build needs is a positive number of bytes')
    release = {'schema': 'builder-release/1', 'host': host, 'profile': profile, **({'python': python} if python else {}),
               'ndk': dict(ndk, license_url=LICENSE_URL),
               'estimates': {'min_free_bytes': min_free_bytes, **({'build_time': build_time} if build_time else {})},
               'files': inventory(package, host)}
    listed = {record['path'] for record in release['files']}
    if targets:
        if set(targets) - set(builder.TARGETS):
            raise ValueError(f'unknown target: {sorted(set(targets) - set(builder.TARGETS))} ({", ".join(builder.TARGETS)})')
        release['profiles'] = dict(targets)
    for what, name in [('profile', profile)] + ([('Python', python)] if python else []) + [(f'{t} profile', r) for t, r in sorted((targets or {}).items())]:
        if name not in listed:
            raise ValueError(f'the {what} is not a file of the package: {name}')
    try:
        builder.ndk_spec(release)
    except builder.Refusal as error:
        raise ValueError(f'the compiler archive is not pinned as the front end wants it: {error}') from None
    if dxc is not None:
        release['dxc'] = fetched_compiler(dxc, listed)
    with output.open('x', encoding='utf-8', newline='\n') as stream:
        stream.write(json.dumps(release, indent=1, sort_keys=True) + '\n')
    if host == builder.host_id():           # the consumer's own reader, where it can run
        try:
            builder.load_release(output)
        except builder.Refusal as error:
            output.unlink()
            raise ValueError(f'the front end refuses the inventory just written: {error}') from None
    return release


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--package', type=Path, required=True)
    ap.add_argument('--host', required=True)
    ap.add_argument('--profile', required=True)
    ap.add_argument('--python')
    ap.add_argument('--ndk-url', required=True)
    ap.add_argument('--ndk-sha256', required=True)
    ap.add_argument('--ndk-size', type=int, required=True)
    ap.add_argument('--ndk-directory', required=True)
    ap.add_argument('--ndk-unpacked-bytes', type=int, required=True)
    ap.add_argument('--ndk-image-root')
    ap.add_argument('--min-free-bytes', type=int, required=True)
    ap.add_argument('--build-time')
    ap.add_argument('--dxc', type=Path)
    ap.add_argument('--output', type=Path)
    ap.add_argument('--target', action='append', default=[], metavar='NAME=REL')
    a = ap.parse_args(argv)
    ndk = {'url': a.ndk_url, 'sha256': a.ndk_sha256, 'size': a.ndk_size, 'directory': a.ndk_directory, 'unpacked_bytes': a.ndk_unpacked_bytes}
    if a.ndk_image_root:
        ndk['image_root'] = a.ndk_image_root
    try:
        dxc = json.loads(a.dxc.read_text(encoding='utf-8')) if a.dxc else None
        if a.dxc and not isinstance(dxc, dict):
            raise ValueError('the shader compiler is not pinned as the front end wants it: the pins file holds no record')
        if any('=' not in item for item in a.target):
            raise ValueError('--target wants NAME=REL')
        targets = dict(item.split('=', 1) for item in a.target)
        release = make(a.package, a.host, a.profile, a.python, ndk, a.min_free_bytes, a.build_time, a.output or a.package / 'release.json', dxc,
                       targets)
    except (ValueError, OSError) as error:
        print('RELEASE_REFUSED:', error, file=sys.stderr)
        return 2
    programs = sum(1 for record in release['files'] if record.get('executable'))
    fetched = f", {len(release['dxc']['files'])} shader compiler files fetched by the builder" if 'dxc' in release else ''
    print('RELEASE', len(release['files']), 'files,', programs, 'programs, host', release['host'] + fetched, flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
