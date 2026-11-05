#!/usr/bin/env python3
"""Write an iOS app folder as an .ipa, deterministically: the same folder gives the same bytes on every run and host.

    ipa_writer.py --app DIR.app --output NEW.ipa [--add APP_REL=SRC_DIR ...]

The archive holds Payload/<name>.app/ and everything below it, plus each --add folder placed at APP_REL inside the app
(the builder's outputs: catalogs, modules, shader pack, template run, without copying them into the app first; never the
disc files: the game's data is always imported from the discs on the device, owner 2026-11-05). Entries
are stored (no compression: most of an iOS game is already compressed or must be mapped), sorted by name, with folder
entries, a fixed time (1980-01-01 00:00), the Unix creator and mode 0755 for a file with any execute bit (the app's
executable, dylibs) and 0644 for the rest; ZIP64 records only where a file or the archive passes 4 GiB (an app should not). AppleDouble files
(._*) and .DS_Store are left out; a link, a device or a name that is not plain UTF-8 relative text is refused, as is a
second entry of one name (also when two names differ only by case: iOS file systems are case-insensitive by default).
The output must not exist. Nothing here reads the game; what it packs may be game-derived and then stays private.
Exit 0: written; 2: refused, nothing left behind.
"""
from __future__ import annotations

import argparse
import os
import stat
import sys
import zipfile
from pathlib import Path, PurePosixPath

FIXED_TIME = (1980, 1, 1, 0, 0, 0)
SKIPPED = ('.DS_Store',)


class Refused(Exception):
    pass


def entries(app: Path, adds: list[tuple[str, Path]]) -> list[tuple[str, Path | None, int]]:
    """(name in the archive, source file or None for a folder, mode), sorted by name"""
    if app.suffix != '.app' or not app.is_dir() or app.is_symlink():
        raise Refused(f'not an app folder: {app}')
    top = PurePosixPath('Payload') / app.name
    out: dict[str, tuple[Path | None, int]] = {}
    folded: dict[str, str] = {}

    def put(name: PurePosixPath, src: Path | None, mode: int) -> None:
        text = name.as_posix() + ('/' if src is None else '')
        key = text.casefold()
        if key in folded:
            if folded[key] == text and src is None:
                return                                    # a folder reached twice (an --add inside an existing folder)
            raise Refused(f'two entries named {text!r} and {folded[key]!r}')
        folded[key] = text
        out[text] = (src, mode)

    def walk(root: Path, at: PurePosixPath) -> None:
        for parent in list(reversed(at.parents))[1:] + [at]:
            put(parent, None, 0o755)
        for here, dirs, files in os.walk(root, followlinks=False):
            dirs.sort()
            rel = Path(here).relative_to(root)
            for name in sorted(dirs) + sorted(files):
                if name.startswith('._') or name in SKIPPED:
                    continue
                if name in ('.', '..') or '/' in name or '\\' in name or not name.isprintable():
                    raise Refused(f'unsafe name in {here}: {name!r}')
                try:
                    name.encode('utf-8')
                except UnicodeEncodeError:
                    raise Refused(f'name is not UTF-8 text in {here}') from None
                p = Path(here) / name
                mode = p.lstat().st_mode
                target = at / PurePosixPath(rel.as_posix()) / name if str(rel) != '.' else at / name
                if stat.S_ISLNK(mode):
                    raise Refused(f'a link is not packed: {p}')
                if stat.S_ISDIR(mode):
                    put(target, None, 0o755)
                elif stat.S_ISREG(mode):
                    put(target, p, 0o755 if mode & 0o111 else 0o644)
                else:
                    raise Refused(f'not a file or folder: {p}')

    walk(app, top)
    for rel, src in adds:
        r = PurePosixPath(rel)
        if r.is_absolute() or '..' in r.parts or not r.parts or str(r) in ('.', ''):
            raise Refused(f'--add place must be a relative path inside the app: {rel!r}')
        if not src.is_dir() or src.is_symlink():
            raise Refused(f'--add source is not a folder: {src}')
        walk(src, top / r)
    return [(name, *out[name]) for name in sorted(out)]


def write_ipa(app: Path, output: Path, adds: list[tuple[str, Path]] | None = None) -> int:
    if output.exists():
        raise Refused(f'the output exists: {output}')
    listed = entries(app, adds or [])
    output.parent.mkdir(parents=True, exist_ok=True)
    partial = output.with_name(output.name + '.partial')
    if partial.exists():
        raise Refused(f'a partial output is in the way: {partial}')
    try:
        with zipfile.ZipFile(partial, 'x', compression=zipfile.ZIP_STORED, allowZip64=True) as z:
            for name, src, mode in listed:
                info = zipfile.ZipInfo(name, FIXED_TIME)
                info.create_system = 3                     # Unix, so the modes are read
                info.compress_type = zipfile.ZIP_STORED
                if src is None:
                    info.external_attr = (stat.S_IFDIR | mode) << 16 | 0x10
                    z.writestr(info, b'')
                    continue
                info.external_attr = (stat.S_IFREG | mode) << 16
                info.file_size = src.stat().st_size
                with src.open('rb') as source, z.open(info, 'w', force_zip64=info.file_size > zipfile.ZIP64_LIMIT) as sink:
                    while block := source.read(1 << 22):
                        sink.write(block)
        os.replace(partial, output)
    except BaseException:
        partial.unlink(missing_ok=True)
        raise
    return len(listed)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--app', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--add', action='append', default=[], metavar='APP_REL=SRC_DIR')
    a = ap.parse_args(argv)
    try:
        adds = []
        for item in a.add:
            if '=' not in item:
                raise Refused(f'--add needs APP_REL=SRC_DIR: {item!r}')
            rel, src = item.split('=', 1)
            adds.append((rel, Path(src)))
        count = write_ipa(a.app, a.output, adds)
    except (Refused, OSError) as error:
        print('IPA_REFUSED:', error, file=sys.stderr)
        return 2
    print(f'IPA {a.output.name}: {count} entries, {a.output.stat().st_size} bytes', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
