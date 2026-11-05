#!/usr/bin/env python3
"""disc_builder.py: desktop builder, "disc images in, code bundle out".

  disc_builder.py build   --profile P.json --play-disc ISO_OR_FOLDER --work DIR [--install-disc ISO_OR_FOLDER]
                          [--dry-run] [--allow-heavy] [--stop-after STAGE] [--supply STAGE=PATH] [--report OUT.json]
                          [--out DIR]
  disc_builder.py identify --profile P.json --play-disc ISO_OR_FOLDER
  disc_builder.py verify  BUNDLE [--executable-sha256 HEX]

What it does, in this order:
  1. Identify. Reads the game executable (default.xex) out of the play disc (an XISO image, or an extracted folder),
     hashes it (SHA-256) and looks the hash up in the profile's "games" table. An unknown executable is refused
     (exit 4) before anything else happens.
  2. Plan. Every path and every ${...} variable of the profile is resolved, the stage named by --stop-after and the
     bundle description are checked. A mistake stops the run here (exit 64), not after some stages have run.
  3. Prerequisites. Every tool file of every stage, every "prerequisites" entry of the profile (files, optionally
     with a pinned SHA-256; version commands with an expected pattern; the Python version) and the free disk space
     are checked. Any failure stops the run before a single stage has run (exit 2). A dry run lists the failures and
     still prints the plan.
  4. Stages, in the profile's order. Each stage is an adapter ("disc-file", "command", "supplied") with declared
     tools, inputs and outputs. Before a stage runs, its key is computed: SHA-256 over the stage definition, the
     SHA-256 of every tool and input, the identified game and the key of the stage before it. If WORK/receipts/
     <stage>.json holds the same key and every recorded output still has its recorded SHA-256, the stage is skipped.
     Because every key contains the previous stage's key, a changed input re-runs its stage and every stage after it.
     Stages marked "heavy" run only with --allow-heavy; without it the run stops in front of the first heavy stage
     that is not covered by a receipt (exit 3).
  5. Bundle. The declared components are written into one file (format: BUNDLE-FORMAT.md next to this tool), with
     a manifest of names, roles, sizes, SHA-256s and versions, then the file is read back and verified.

Stage fields besides tools / inputs / outputs / argv: "clean" (folders of the work directory removed before the stage
runs), "make_dirs" (folders of the work directory created before the command runs, for tools that want an existing
output folder), "expect" (required SHA-256 per output), "heavy", "cwd". A command stage whose tool can only write
outside the work directory says "external_outputs": true; such an output must be a file, the file it replaces is
first copied to WORK/backup/<stage>/, and the stage fails unless its command really wrote the file (an older file
at that place is not accepted as built).

Where the builder writes: the work directory, the bundle directory (--out, default WORK/out), the report (--report)
and declared external outputs. All of it is game-derived (copies, hashes, paths), so inside a git checkout each of
these must lie under that checkout's local-analysis/ (git-ignored). A work directory that already holds files must
have been made by this builder (it carries a .own-disc-work file): the builder removes and rewrites folders in it.

The tool ships nothing and uploads nothing: it has no network code. It contains no game data: the identification
table, the stage commands and every game-derived parameter live in the profile, a private JSON file. Hashes are printed
as 12-hex-digit prefixes; file contents are never printed.

Exit codes: 0 done (or dry run without problems), 1 a stage failed, 2 a prerequisite is missing, 3 stopped in front of a
heavy stage, 4 the executable is not a supported game, 5 a bundle does not verify, 64 bad usage or bad profile.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zipfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))      # the host helper lies beside this file
import host_support  # noqa: E402

TOOL_ROOT = host_support.package_root(__file__)
PROFILE_SCHEMA = 'own-disc-pipeline/1'
RECEIPT_SCHEMA = 'own-disc-receipt/1'
BUNDLE_SCHEMA = 'gta5-code-bundle/1'
# Format 2 (2026-10-07): what the public app reads. Two differences from format 1. It may carry small files the runtime
# reads beside the game data (role runtime-data, names under runtime/). And its manifest holds nothing that differs
# between two machines building from the same discs: no stage keys (they cover local paths and tool builds), no hash
# of this file's host copy; instead the profile's own statement of what the build is made from (portable_provenance).
# So two machines agree when their format-2 bundles are the same file.
BUNDLE_SCHEMA_2 = 'gta5-code-bundle/2'
RUNTIME_ROLE = 'runtime-data'
RUNTIME_PREFIX = 'runtime/'
ENGINE_KEY_VERSION = 3          # bump when the meaning of a stage key changes (2: tree digests skip TREE_IGNORED; 3: the engine's and the host helper's own hashes are part of every key, and a tree's files are ordered by their names with '/', the same on every host)
BLOCK = 1 << 20
PRIVATE_DIR = 'local-analysis'  # the git-ignored folder of a checkout; the only place in one the builder writes to
WORK_MARKER = '.own-disc-work'  # a work directory made by this builder holds this file
TREE_IGNORED = frozenset(('__pycache__', '.DS_Store'))

EXIT_OK, EXIT_STAGE, EXIT_PREREQ, EXIT_HEAVY, EXIT_GAME, EXIT_BUNDLE, EXIT_USAGE = 0, 1, 2, 3, 4, 5, 64


class BuilderError(Exception):
    def __init__(self, message: str, code: int = EXIT_STAGE):
        super().__init__(message)
        self.code = code


# ------------------------------------------------------------------------------------------------------- hashing

def sha256_stream(f, size: int | None = None) -> str:
    h = hashlib.sha256()
    left = size
    while left is None or left > 0:
        block = f.read(BLOCK if left is None else min(BLOCK, left))
        if not block:
            if left:
                raise BuilderError('unexpected end of data while hashing')
            break
        h.update(block)
        if left is not None:
            left -= len(block)
    return h.hexdigest()


def sha256_file(path: Path) -> str:
    with open(path, 'rb') as f:
        return sha256_stream(f)


def sha256_tree(directory: Path) -> tuple[str, int]:
    """One digest for a directory: the sorted list of (relative path, file SHA-256). Returns (digest, file count).
    Interpreter caches (__pycache__) and Finder files (.DS_Store) are left out: they appear by themselves (another
    Python version writes other cache files) and are not content."""
    h = hashlib.sha256()
    count = 0
    for p in sorted((q for q in directory.rglob('*') if q.is_file()), key=lambda q: q.relative_to(directory).as_posix()):
        rel = p.relative_to(directory)
        if TREE_IGNORED.intersection(rel.parts):
            continue
        h.update(rel.as_posix().encode() + b'\0' + sha256_file(p).encode() + b'\n')
        count += 1
    return h.hexdigest(), count


def sha256_crc32(f, size: int) -> tuple[str, int]:
    h, crc, left = hashlib.sha256(), 0, size
    while left > 0:
        block = f.read(min(BLOCK, left))
        if not block:
            raise BuilderError('unexpected end of data while hashing')
        h.update(block)
        crc = zlib.crc32(block, crc)
        left -= len(block)
    return h.hexdigest(), crc


def sha256_path(path: Path) -> dict:
    if path.is_dir():
        digest, count = sha256_tree(path)
        return {'kind': 'dir', 'sha256': digest, 'files': count}
    if path.is_file():
        return {'kind': 'file', 'sha256': sha256_file(path), 'size': path.stat().st_size}
    raise BuilderError(f'missing: {path}')


def canonical(obj) -> bytes:
    return json.dumps(obj, sort_keys=True, separators=(',', ':')).encode()


def short(digest: str) -> str:
    return digest[:12]


def write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


# ------------------------------------------------------------------------------------------------- disc sources

XDVDFS_MAGIC = b'MICROSOFT*XBOX*MEDIA'
SECTOR = 2048
# Start of the game partition inside an image: 0 for a trimmed image ("XISO"), then the full-disc layouts
# (XGD2, XGD3, XGD1). Values from public format descriptions; NOT yet checked against the pinned extract-xiso
# (that submodule is not checked out in this repository).
XDVDFS_PARTITION_OFFSETS = (0, 0x0FD90000, 0x02080000, 0x18300000)
MAX_DIR_BYTES = 16 << 20


class FolderDisc:
    """An extracted disc: a folder. File names are matched without regard to case."""

    kind = 'folder'

    def __init__(self, root: Path):
        self.root = root

    def _resolve(self, rel: str) -> Path | None:
        cur = self.root
        for part in [p for p in rel.replace('\\', '/').split('/') if p]:
            if part in ('.', '..'):
                raise BuilderError(f'bad disc path: {rel}', EXIT_USAGE)
            if not cur.is_dir():
                return None
            match = [c for c in cur.iterdir() if c.name.lower() == part.lower()]
            if not match:
                return None
            if len(match) != 1:
                raise BuilderError('ambiguous case-insensitive disc path: use an unambiguous extracted disc or ISO', EXIT_USAGE)
            cur = match[0]
        return cur if cur.is_file() else None

    def size(self, rel: str) -> int | None:
        p = self._resolve(rel)
        return None if p is None else p.stat().st_size

    def open(self, rel: str):
        p = self._resolve(rel)
        if p is None:
            raise BuilderError(f'{rel}: not on the disc folder')
        return open(p, 'rb'), p.stat().st_size


class XisoDisc:
    """An Xbox 360 disc image holding an XDVDFS volume (full or trimmed image). Read-only."""

    kind = 'xiso'

    def __init__(self, image: Path):
        self.image = image
        self.partition = None
        size = image.stat().st_size
        with open(image, 'rb') as f:
            for off in XDVDFS_PARTITION_OFFSETS:
                if off + 33 * SECTOR > size:
                    continue
                f.seek(off + 32 * SECTOR)
                head = f.read(SECTOR)
                if head[:20] == XDVDFS_MAGIC and head[-20:] == XDVDFS_MAGIC:
                    self.partition = off
                    self.root_sector, self.root_size = struct.unpack_from('<II', head, 20)
                    break
        if self.partition is None:
            raise BuilderError(f'{image.name}: no Xbox disc file system found (not an XISO image)', EXIT_GAME)
        self.image_size = size

    def _read_dir(self, f, sector: int, size: int) -> dict[str, tuple[int, int, bool]]:
        if size == 0:
            return {}
        if size > MAX_DIR_BYTES or self.partition + sector * SECTOR + size > self.image_size:
            raise BuilderError('disc image: directory outside the image')
        f.seek(self.partition + sector * SECTOR)
        data = f.read(size)
        entries: dict[str, tuple[int, int, bool]] = {}
        todo, seen = [0], set()
        while todo:
            off = todo.pop()
            if off in seen or off + 14 > len(data):
                continue
            seen.add(off)
            left, right, start, fsize, attr, nlen = struct.unpack_from('<HHIIBB', data, off)
            if left == 0xFFFF and right == 0xFFFF and start == 0xFFFFFFFF:
                continue                                    # padding
            name = data[off + 14: off + 14 + nlen].decode('latin-1')
            if len(name) != nlen or not name:
                raise BuilderError('disc image: damaged directory entry')
            entries[name.lower()] = (start, fsize, bool(attr & 0x10))
            for child in (left, right):
                if child not in (0, 0xFFFF):
                    todo.append(child * 4)
        return entries

    def _find(self, f, rel: str) -> tuple[int, int] | None:
        sector, size = self.root_sector, self.root_size
        parts = [p for p in rel.replace('\\', '/').split('/') if p]
        for i, part in enumerate(parts):
            entry = self._read_dir(f, sector, size).get(part.lower())
            if entry is None:
                return None
            start, fsize, is_dir = entry
            if i == len(parts) - 1:
                return None if is_dir else (start, fsize)
            if not is_dir:
                return None
            sector, size = start, fsize
        return None

    def size(self, rel: str) -> int | None:
        with open(self.image, 'rb') as f:
            hit = self._find(f, rel)
        return None if hit is None else hit[1]

    def open(self, rel: str):
        f = open(self.image, 'rb')
        hit = self._find(f, rel)
        if hit is None:
            f.close()
            raise BuilderError(f'{rel}: not on the disc image')
        start, fsize = hit
        if self.partition + start * SECTOR + fsize > self.image_size:
            f.close()
            raise BuilderError(f'{rel}: the image is cut off inside this file')
        f.seek(self.partition + start * SECTOR)
        return f, fsize


def open_disc(path: Path):
    if path.is_dir():
        return FolderDisc(path)
    if path.is_file():
        return XisoDisc(path)
    raise BuilderError(f'disc not found: {path}', EXIT_USAGE)


def disc_hash(disc, rel: str) -> tuple[str, int]:
    f, size = disc.open(rel)
    with f:
        return sha256_stream(f, size), size


def disc_copy(disc, rel: str, dest: Path) -> tuple[str, int]:
    f, size = disc.open(rel)
    dest.parent.mkdir(parents=True, exist_ok=True)
    h = hashlib.sha256()
    left = size
    with f, open(dest, 'wb') as out:
        while left > 0:
            block = f.read(min(BLOCK, left))
            if not block:
                raise BuilderError(f'{rel}: short read from the disc')
            h.update(block)
            out.write(block)
            left -= len(block)
    return h.hexdigest(), size


# ------------------------------------------------------------------------------------------------------ profile

VAR = re.compile(r'\$\{([^}]+)\}')
STAGE_ID = re.compile(r'[a-z][a-z0-9-]{0,39}')
OUTPUT_NAME = re.compile(r'[a-z][a-z0-9_-]{0,39}')
HEX64 = re.compile(r'[0-9a-f]{64}')
ADAPTERS = ('disc-file', 'command', 'supplied')


def load_profile(path: Path) -> dict:
    try:
        profile = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as e:
        raise BuilderError(f'profile {path}: {e}', EXIT_USAGE)
    if not isinstance(profile, dict) or profile.get('schema') != PROFILE_SCHEMA:
        raise BuilderError(f'profile {path}: schema must be {PROFILE_SCHEMA}', EXIT_USAGE)
    ids = [s.get('id', '') for s in profile.get('stages', [])]
    for s in profile.get('stages', []):
        if not STAGE_ID.fullmatch(s.get('id', '')) or s.get('adapter') not in ADAPTERS:
            raise BuilderError(f'profile: bad stage {s.get("id")!r} / adapter {s.get("adapter")!r}', EXIT_USAGE)
        outs = s.get('outputs')
        if not isinstance(outs, dict) or not outs or not all(OUTPUT_NAME.fullmatch(n) for n in outs):
            raise BuilderError(f'profile: stage {s["id"]} needs outputs with simple lower-case names', EXIT_USAGE)
        if s['adapter'] == 'command' and not (isinstance(s.get('argv'), list) and s['argv']):
            raise BuilderError(f'profile: command stage {s["id"]} has no argv', EXIT_USAGE)
        if s['adapter'] != 'command' and len(outs) != 1:
            raise BuilderError(f'profile: stage {s["id"]} copies one file, so it declares exactly one output', EXIT_USAGE)
        if s['adapter'] == 'disc-file' and not s.get('path'):
            raise BuilderError(f'profile: disc-file stage {s["id"]} names no path on the disc', EXIT_USAGE)
    if len(set(ids)) != len(ids) or 'bundle' in ids:
        raise BuilderError('profile: stage ids must be unique and "bundle" is reserved', EXIT_USAGE)
    if not profile.get('games'):
        raise BuilderError('profile: no games table', EXIT_USAGE)
    for g in profile['games']:
        if not HEX64.fullmatch(str(g.get('executable_sha256', ''))):
            raise BuilderError('profile: every game needs executable_sha256 (64 hex digits)', EXIT_USAGE)
    profile['_dir'] = path.resolve().parent
    return profile


class Context:
    """Paths and facts a run knows; expands ${...} variables of the profile."""

    def __init__(self, profile: dict, work: Path, repo: Path | None = None, data: Path | None = None):
        self.profile = profile
        self.work = work.resolve()
        paths = profile.get('paths', {})
        base = profile['_dir']
        self.repo = (repo or self._rel(base, paths.get('repo')) or TOOL_ROOT).resolve()
        self.data = (data or self._rel(base, paths.get('data')) or self.repo).resolve()
        self.game: dict = {}
        self.discs: dict = {}
        self.outputs: dict[str, dict[str, Path]] = {}       # filled by plan_paths, stage by stage

    @staticmethod
    def _rel(base: Path, value: str | None) -> Path | None:
        if not value:
            return None
        p = Path(value)
        return p if p.is_absolute() else base / p

    def expand(self, text: str) -> str:
        def sub(m: re.Match) -> str:
            name = m.group(1)
            if name == 'python':
                return os.environ.get('BUILDER_PYTHON', sys.executable)
            if name == 'ndk':
                if not os.environ.get('BUILDER_NDK_ROOT'):
                    raise BuilderError('NDK missing: run builder fetch after reading its licence', EXIT_PREREQ)
                return os.environ['BUILDER_NDK_ROOT']
            if name == 'exe':
                return '.exe' if sys.platform == 'win32' else ''
            if name == 'ndkhost':                           # this host's folder inside the NDK
                return host_support.ndk_host_tag()
            if name in ('xcode', 'iossdk', 'appleclang'):   # the Apple toolchain of an iOS profile (Mac only)
                return str(xcode_path(name))
            if name == 'bundleid':                          # the player's bundle identifier for an iOS app (builder --bundle-id)
                value = os.environ.get('BUILDER_BUNDLE_ID', '')
                if not re.fullmatch(r'[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+', value):
                    raise BuilderError('bundle id missing or malformed: give --bundle-id in reverse-DNS form '
                                       '(for example com.yourname.fivepocket)', EXIT_USAGE)
                return value
            if name == 'hosttools':                         # this host's builds of the project's own tools
                if not os.environ.get('BUILDER_HOST_TOOLS'):
                    raise BuilderError('host tools missing: BUILDER_HOST_TOOLS names no folder', EXIT_PREREQ)
                return os.environ['BUILDER_HOST_TOOLS']
            if name == 'repo':
                return str(self.repo)
            if name == 'data':
                return str(self.data)
            if name == 'work':
                return str(self.work)
            if name == 'profile':
                return str(self.profile.get('id', ''))
            if name == 'flavour':
                return str(self.profile.get('flavour', ''))
            if name.startswith('game:'):
                if name[5:] not in self.game:
                    raise BuilderError(f'profile: unknown game field in {m.group(0)}', EXIT_USAGE)
                return str(self.game[name[5:]])
            if name.startswith('disc:'):                    # the image file (or folder) of a disc named on the command line
                disc = self.discs.get(name[5:])
                if disc is None:
                    raise BuilderError(f'profile: {m.group(0)} needs that disc on the command line', EXIT_USAGE)
                return str(getattr(disc, 'image', None) or disc.root)
            if name.startswith('out:'):
                _, stage, out = (name.split(':') + ['', ''])[:3]
                if stage not in self.outputs or out not in self.outputs[stage]:
                    raise BuilderError(f'profile: {m.group(0)} names no output of an earlier stage', EXIT_USAGE)
                return str(self.outputs[stage][out])
            raise BuilderError(f'profile: unknown variable {m.group(0)}', EXIT_USAGE)
        return VAR.sub(sub, text)


XCODE_PARTS = {'xcode': '', 'iossdk': 'Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk',
               'appleclang': 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang'}


def xcode_path(name: str) -> Path:
    """${xcode} (Xcode's Developer folder), ${iossdk}, ${appleclang}. Taken from BUILDER_XCODE, else DEVELOPER_DIR, never from
    xcode-select: on a Mac where xcode-select names the Command Line Tools (which have no iPhoneOS SDK) the run would otherwise
    pick a toolchain that cannot build for iOS. The folder must hold the iPhoneOS platform."""
    raw = os.environ.get('BUILDER_XCODE') or os.environ.get('DEVELOPER_DIR')
    if not raw:
        raise BuilderError('Xcode missing: set DEVELOPER_DIR (or BUILDER_XCODE) to Xcode\'s Contents/Developer folder, '
                           'e.g. /Applications/Xcode.app/Contents/Developer', EXIT_PREREQ)
    root = Path(raw)
    if root.name != 'Developer' and (root / 'Contents/Developer').is_dir():
        root = root / 'Contents/Developer'               # the .app folder itself was given
    if not (root / XCODE_PARTS['iossdk']).is_dir():
        raise BuilderError(f'Xcode missing: {root} holds no iPhoneOS SDK (the Command Line Tools have none; '
                           'install Xcode and point DEVELOPER_DIR at it)', EXIT_PREREQ)
    return root / XCODE_PARTS[name] if XCODE_PARTS[name] else root


def check_private_path(path: Path, what: str) -> None:
    """What the builder writes is game-derived: inside a git checkout it must stay under its ignored local-analysis/."""
    path = path.resolve()
    for parent in [path, *path.parents]:
        if (parent / '.git').exists():
            if not path.is_relative_to(parent / PRIVATE_DIR):
                raise BuilderError(f'{what} {path} is inside the git checkout {parent} but not under its '
                                   f'{PRIVATE_DIR}/: refusing to write game-derived files there', EXIT_USAGE)
            return


def check_work_dir(work: Path) -> None:
    """The builder removes and rewrites folders in its work directory, so it only takes one that is new, empty or its
    own (marked by WORK_MARKER)."""
    if work.exists() and (not work.is_dir() or (any(work.iterdir()) and not (work / WORK_MARKER).is_file())):
        raise BuilderError(f'work directory {work} already holds files and was not made by this builder (no '
                           f'{WORK_MARKER} file in it): give an empty or new directory', EXIT_USAGE)


def work_target(ctx: Context, stage_id: str, value: str, what: str) -> Path:
    """A folder or file the builder itself removes or creates for a stage: always strictly inside the work directory."""
    p = (ctx.work / ctx.expand(value)).resolve()
    if p == ctx.work or not p.is_relative_to(ctx.work):
        raise BuilderError(f'stage {stage_id}: {what} {value!r} is outside the work directory', EXIT_USAGE)
    return p


def tool_paths(stage: dict) -> list[str]:
    return [t['path'] if isinstance(t, dict) else t for t in stage.get('tools', [])]


def plan_paths(profile: dict, ctx: Context) -> None:
    """Resolves every stage's output paths into ctx.outputs and expands every other ${...} of the stages once, so a
    misspelt variable, a reference to a later stage or a path the builder must not write stops the run before any
    stage has run. Relative outputs live in the work directory."""
    for stage in profile['stages']:
        sid = stage['id']
        outs: dict[str, Path] = {}
        for name, value in stage['outputs'].items():
            p = (ctx.work / ctx.expand(value)).resolve()
            if p == ctx.work:
                raise BuilderError(f'profile: output {name} of stage {sid} is the work directory itself', EXIT_USAGE)
            if not p.is_relative_to(ctx.work):
                if stage['adapter'] != 'command' or stage.get('external_outputs') is not True:
                    raise BuilderError(f'profile: output {name} of stage {sid} ({p}) is outside the work directory; only '
                                       f'a command stage that says "external_outputs": true may write there', EXIT_USAGE)
                check_private_path(p, f'output {name} of stage {sid}')
            outs[name] = p
        for text in tool_paths(stage) + list(stage.get('inputs', {}).values()) + [stage.get('source') or '']:
            ctx.expand(text)                    # what a stage reads comes from earlier stages only
        ctx.outputs[sid] = outs                 # its command line may name its own outputs
        for text in list(stage.get('argv', [])) + list(stage.get('expect', {}).values()) + [stage.get('cwd') or '']:
            ctx.expand(text)
        for value in stage.get('clean', []):
            work_target(ctx, sid, value, 'clean target')
        for value in stage.get('make_dirs', []):
            work_target(ctx, sid, value, 'folder to make')


# ----------------------------------------------------------------------------------------------- identification

def xex_details(path_or_bytes) -> str:
    """Non-secret header facts for the refusal message (title, media id, version), when the reader is available."""
    try:
        sys.path.insert(0, str(TOOL_ROOT / 'tools' / 'xex_info'))
        import xex_info  # type: ignore
        info = xex_info.parse_xex2(path_or_bytes)
        exe = info.get('execution_info') or {}
        facts = [f'{k} {exe[k]}' for k in ('title_id', 'media_id', 'version', 'base_version', 'disc_number') if k in exe]
        return ', '.join(facts)
    except Exception:
        return ''
    finally:
        if sys.path and sys.path[0].endswith('xex_info'):
            sys.path.pop(0)


def identify(profile: dict, play) -> dict:
    """Returns the matching entry of the profile's games table plus the measured hash; refuses anything else."""
    names = sorted({g.get('executable', 'default.xex') for g in profile['games']})
    seen = []
    for name in names:
        if play.size(name) is None:
            continue
        digest, size = disc_hash(play, name)
        seen.append((name, digest, size))
        for g in profile['games']:
            if g.get('executable', 'default.xex') == name and g['executable_sha256'] == digest and \
                    g.get('executable_size', size) == size:
                return dict(g, executable=name, executable_sha256=digest, executable_size=size)
    if not seen:
        raise BuilderError(f'No game executable ({", ".join(names)}) on the play disc: is this the play disc of the '
                           f'game, and is the image complete?', EXIT_GAME)
    name, digest, size = seen[0]
    details = ''
    if size <= 256 << 20:
        f, _ = play.open(name)
        with f:
            details = xex_details(f.read(size))
    raise BuilderError('Unsupported game version: ' + (details + ', ' if details else '') +
                       f'executable hash {short(digest)} ({size} bytes). This build supports only: ' +
                       '; '.join(str(g.get('label', g.get('id', '?'))) for g in profile['games']) + '.', EXIT_GAME)


def check_disc_files(game: dict, discs: dict, hash_files: bool) -> list[str]:
    """Optional presence / size (/ hash) check of other disc files listed for the game. Returns problems."""
    problems = []
    for rec in game.get('disc_files', []):
        disc = discs.get(rec.get('disc', 'play'))
        if disc is None:
            continue                       # that disc was not given; a stage that needs it will say so
        size = disc.size(rec['path'])
        if size is None:
            problems.append(f'{rec.get("disc", "play")} disc: {rec["path"]} is missing')
        elif 'size' in rec and size != rec['size']:
            problems.append(f'{rec.get("disc", "play")} disc: {rec["path"]} has {size} bytes, expected {rec["size"]}')
        elif hash_files and 'sha256' in rec and disc_hash(disc, rec['path'])[0] != rec['sha256']:
            problems.append(f'{rec.get("disc", "play")} disc: {rec["path"]} has a different SHA-256')
    return problems


# ------------------------------------------------------------------------------------------------ prerequisites

def version_tuple(text: str) -> tuple[int, ...]:
    return tuple(int(x) for x in re.findall(r'\d+', text)[:3])


def check_prerequisites(profile: dict, ctx: Context, supply: dict | None = None) -> list[str]:
    """Every check runs (so one report lists everything that is missing). Returns the failures."""
    failures: list[str] = []

    def check_file(label: str, raw: str, pinned: str | None = None, executable: bool = False) -> None:
        if '${out:' in raw:
            return                            # made by an earlier stage during the run
        p = Path(ctx.expand(raw))
        if not p.exists():
            failures.append(f'{label}: missing {p}')
        elif executable and not os.access(p, os.X_OK):
            failures.append(f'{label}: not executable {p}')
        elif pinned and p.is_file() and sha256_file(p) != pinned:
            failures.append(f'{label}: {p.name} is not the pinned version (SHA-256 differs)')

    for s in profile.get('stages', []):
        for tool in s.get('tools', []):
            if isinstance(tool, dict):
                check_file(f'stage {s["id"]} tool', tool['path'], tool.get('sha256'), tool.get('executable', False))
            else:
                check_file(f'stage {s["id"]} tool', tool)
        for name, raw in s.get('inputs', {}).items():
            check_file(f'stage {s["id"]} input {name}', raw)
        if s['adapter'] == 'supplied':
            given = (supply or {}).get(s['id'])
            if given or s.get('source'):
                check_file(f'stage {s["id"]} supplied file', given or s['source'], None if given else s.get('sha256'))
            else:
                failures.append(f'stage {s["id"]}: nothing supplied (give --supply {s["id"]}=PATH)')
        if s['adapter'] == 'disc-file' and s.get('disc', 'play') not in ctx.discs:
            failures.append(f'stage {s["id"]}: needs the {s.get("disc", "play")} disc (not given)')
    for pre in profile.get('prerequisites', []):
        kind, label = pre.get('kind'), f'prerequisite {pre.get("id", "?")}'
        if kind == 'file':
            check_file(label, pre['path'], pre.get('sha256'), pre.get('executable', False))
        elif kind == 'python':
            if sys.version_info[:3] < version_tuple(pre.get('min', '3.9')):
                failures.append(f'{label}: Python {pre.get("min")} or newer needed, this is '
                                f'{".".join(map(str, sys.version_info[:3]))}')
        elif kind == 'command':
            argv = [ctx.expand(a) for a in pre['argv']]
            try:
                r = subprocess.run(argv, capture_output=True, text=True, timeout=pre.get('timeout', 30))
                text = r.stdout + r.stderr
                if r.returncode != 0:
                    failures.append(f'{label}: {Path(argv[0]).name} exited with {r.returncode}')
                elif pre.get('match') and not re.search(pre['match'], text):
                    failures.append(f'{label}: {Path(argv[0]).name} does not report the expected version '
                                    f'(pattern {pre["match"]!r})')
            except (OSError, subprocess.TimeoutExpired) as e:
                failures.append(f'{label}: cannot run {Path(argv[0]).name} ({type(e).__name__})')
        else:
            failures.append(f'{label}: unknown kind {kind!r}')
    need = profile.get('min_free_bytes', 0)
    if need:
        probe = ctx.work
        while not probe.exists():
            probe = probe.parent
        free = shutil.disk_usage(probe).free
        if free < need:
            failures.append(f'disk space: {free / 1e9:.1f} GB free at {probe}, {need / 1e9:.1f} GB needed')
    return failures


# ------------------------------------------------------------------------------------------------------- stages

def stage_definition(stage: dict) -> dict:
    return {k: v for k, v in stage.items() if k not in ('description', 'note')}


def stage_key_parts(stage: dict, ctx: Context, previous_key: str, supply: dict) -> dict:
    """Everything that decides whether a stage's old outputs are still valid. Raises if an input is missing."""
    parts = {'engine': ENGINE_KEY_VERSION, 'definition': hashlib.sha256(canonical(stage_definition(stage))).hexdigest(),
             'game': ctx.game['executable_sha256'], 'previous': previous_key, 'tools': {'builder-engine': hashlib.sha256(__loader__.get_data(__file__)).hexdigest(), 'host-support': hashlib.sha256(host_support.__loader__.get_data(host_support.__file__)).hexdigest()}, 'inputs': {}}
    for raw in tool_paths(stage):
        parts['tools'][raw] = sha256_path(Path(ctx.expand(raw)))['sha256']
    for name, raw in sorted(stage.get('inputs', {}).items()):
        parts['inputs'][name] = sha256_path(Path(ctx.expand(raw)))['sha256']
    if stage['adapter'] == 'disc-file':
        disc = ctx.discs[stage.get('disc', 'play')]
        if stage['path'] == ctx.game['executable'] and stage.get('disc', 'play') == 'play':
            parts['inputs']['disc:' + stage['path']] = ctx.game['executable_sha256']
        else:
            parts['inputs']['disc:' + stage['path']] = disc_hash(disc, stage['path'])[0]
    if stage['adapter'] == 'supplied':
        parts['inputs']['supplied'] = sha256_path(supplied_source(stage, ctx, supply))['sha256']
    return parts


def supplied_source(stage: dict, ctx: Context, supply: dict) -> Path:
    if supply.get(stage['id']):
        return Path(supply[stage['id']])
    if not stage.get('source'):
        raise BuilderError(f'stage {stage["id"]}: nothing supplied (give --supply {stage["id"]}=PATH)', EXIT_PREREQ)
    return Path(ctx.expand(stage['source']))


def key_of(parts: dict) -> str:
    return hashlib.sha256(canonical(parts)).hexdigest()


def receipt_path(ctx: Context, stage_id: str) -> Path:
    return ctx.work / 'receipts' / f'{stage_id}.json'


def read_receipt(ctx: Context, stage_id: str) -> dict | None:
    try:
        r = json.loads(receipt_path(ctx, stage_id).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    return r if isinstance(r, dict) and r.get('schema') == RECEIPT_SCHEMA else None


def receipt_verdict(receipt: dict | None, parts: dict, outputs: dict[str, Path]) -> str | None:
    """None when the receipt covers this stage (skip); otherwise the reason it has to run."""
    if receipt is None:
        return 'no receipt'
    if receipt.get('key') != key_of(parts):
        old = receipt.get('key_parts', {})
        for group in ('inputs', 'tools'):
            for name, digest in parts[group].items():
                if old.get(group, {}).get(name) != digest:
                    return f'{group[:-1]} {name} changed'
        for field in ('definition', 'game', 'previous', 'engine'):
            if old.get(field) != parts[field]:
                return {'previous': 'an earlier stage changed', 'definition': 'the stage definition changed',
                        'game': 'a different game executable', 'engine': 'the builder changed'}[field]
        return 'key differs'
    recorded = receipt.get('outputs', {})
    for name, path in outputs.items():
        rec = recorded.get(name)
        if rec is None or not path.exists():
            return f'output {name} is missing'
        if sha256_path(path)['sha256'] != rec['sha256']:
            return f'output {name} was modified'
    return None


def clean_outputs(stage: dict, ctx: Context, outputs: dict[str, Path]) -> None:
    """Removes what the stage is about to rebuild, only inside the work directory."""
    targets = [work_target(ctx, stage['id'], c, 'clean target') for c in stage.get('clean', [])]
    targets += [p for p in outputs.values() if p.is_relative_to(ctx.work)]
    for t in targets:
        if t.is_dir():
            shutil.rmtree(t)
        elif t.exists():
            t.unlink()


def file_stamp(path: Path) -> tuple[int, int, int]:
    st = path.stat()
    return st.st_ino, st.st_size, st.st_mtime_ns


def run_stage(stage: dict, ctx: Context, outputs: dict[str, Path], supply: dict, nice: int) -> dict:
    """Runs one adapter. Returns extra facts for the receipt. Raises BuilderError on failure."""
    sid, adapter = stage['id'], stage['adapter']
    extra: dict = {}
    clean_outputs(stage, ctx, outputs)
    for value in stage.get('make_dirs', []):
        work_target(ctx, sid, value, 'folder to make').mkdir(parents=True, exist_ok=True)
    # Outputs outside the work directory ("external_outputs"): the file about to be replaced is kept, and afterwards
    # the command must really have written the file.
    outside = {n: p for n, p in outputs.items() if not p.is_relative_to(ctx.work)}
    stamps: dict[str, tuple[int, int, int]] = {}
    for name, p in outside.items():
        if p.is_file():
            stamps[name] = file_stamp(p)
            kept = ctx.work / 'backup' / sid / f'{name}-{short(sha256_file(p))}'
            if not kept.exists():
                kept.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(p, kept)
            extra.setdefault('previous_files', {})[name] = str(kept)
            print(f'  {sid:<16} kept        the {name} file it replaces: {kept}')
    if adapter != 'command':            # a command creates its own output directories (some tools demand a new one)
        for p in outputs.values():
            p.parent.mkdir(parents=True, exist_ok=True)
    if adapter == 'disc-file':
        (name, dest), = outputs.items()
        disc_copy(ctx.discs[stage.get('disc', 'play')], stage['path'], dest)
    elif adapter == 'supplied':
        (name, dest), = outputs.items()
        src = supplied_source(stage, ctx, supply)
        if src.is_dir():
            shutil.copytree(src, dest)
        else:
            shutil.copyfile(src, dest)
    else:
        argv = [ctx.expand(a) for a in stage['argv']]
        argv, priority_options = host_support.low_priority(argv, nice)
        env = {k: v for k, v in os.environ.items() if not k.endswith('_MUTANT')}   # this project's test switches
        env['PYTHONHASHSEED'] = '0'
        log = ctx.work / 'logs' / f'{sid}.log'
        log.parent.mkdir(parents=True, exist_ok=True)
        cwd = Path(ctx.expand(stage['cwd'])) if stage.get('cwd') else ctx.repo
        with open(log, 'wb') as out:
            try:
                r = subprocess.run(argv, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **priority_options)
            except OSError as e:
                raise BuilderError(f'stage {sid}: cannot start {Path(argv[0]).name}: {e}')
        extra['log'] = str(log)
        if r.returncode != 0:
            raise BuilderError(f'stage {sid}: command exited with {r.returncode}; log: {log}')
    for name, p in outputs.items():
        if not p.exists():
            raise BuilderError(f'stage {sid}: output {name} was not produced ({p})')
    for name, p in outside.items():
        if not p.is_file() or stamps.get(name) == file_stamp(p):
            raise BuilderError(f'stage {sid}: output {name} ({p}) is outside the work directory and the command did not '
                               f'write it as a file: an older file there is not accepted as built by this run')
    return extra


def provenance_of(stage: dict) -> str:
    return {'disc-file': 'disc', 'supplied': 'supplied', 'command': 'built'}[stage['adapter']]


def run_pipeline(profile: dict, ctx: Context, *, dry_run: bool, allow_heavy: bool, supply: dict,
                 stop_after: str | None, nice: int) -> tuple[list[dict], int]:
    """Walks the stages in order. Returns (per-stage report rows, exit code)."""
    rows: list[dict] = []
    previous_key = 'start'
    upstream_runs: str | None = None          # dry run: the first earlier stage that would run
    for stage in profile['stages']:
        sid = stage['id']
        outputs = ctx.outputs[sid]
        row = {'stage': sid, 'adapter': stage['adapter'], 'heavy': bool(stage.get('heavy')),
               'provenance': provenance_of(stage)}
        rows.append(row)
        if upstream_runs:
            row.update(action='would-run', reason=f'earlier stage {upstream_runs} runs')
            print(f'  {sid:<16} would run   (earlier stage {upstream_runs} runs)' + ('  [heavy]' if row['heavy'] else ''))
            continue
        try:
            parts = stage_key_parts(stage, ctx, previous_key, supply)
        except BuilderError as e:
            row.update(action='failed', reason=str(e))
            print(f'  {sid:<16} FAILED      {e}')
            return rows, EXIT_STAGE
        key = key_of(parts)
        reason = receipt_verdict(read_receipt(ctx, sid), parts, outputs)
        row['key'] = key
        if reason is None:
            row.update(action='skipped', reason='receipt matches')
            print(f'  {sid:<16} skipped     (receipt {short(key)}: inputs, tools and outputs unchanged)')
        elif dry_run:
            row.update(action='would-run', reason=reason)
            upstream_runs = sid
            print(f'  {sid:<16} would run   ({reason})' + ('  [heavy]' if row['heavy'] else ''))
        elif row['heavy'] and not allow_heavy:
            row.update(action='stopped-before-heavy', reason=reason)
            print(f'  {sid:<16} NOT RUN     heavy stage ({reason}); rerun with --allow-heavy on the build machine')
            return rows, EXIT_HEAVY
        else:
            t0 = time.monotonic()
            try:
                extra = run_stage(stage, ctx, outputs, supply, nice)
                recorded = {}
                for name, p in outputs.items():
                    recorded[name] = dict(sha256_path(p), path=str(p))
                    want = stage.get('expect', {}).get(name)
                    if want and recorded[name]['sha256'] != ctx.expand(want):
                        raise BuilderError(f'stage {sid}: output {name} has SHA-256 {short(recorded[name]["sha256"])}, '
                                           f'the profile expects {short(ctx.expand(want))}')
            except BuilderError as e:
                row.update(action='failed', reason=str(e))
                print(f'  {sid:<16} FAILED      {e}')
                return rows, e.code
            seconds = round(time.monotonic() - t0, 2)
            receipt = dict(schema=RECEIPT_SCHEMA, stage=sid, key=key, key_parts=parts, outputs=recorded,
                           seconds=seconds, provenance=row['provenance'], **extra)
            write_atomic(receipt_path(ctx, sid), json.dumps(receipt, indent=1, sort_keys=True).encode() + b'\n')
            row.update(action='ran', reason=reason, seconds=seconds)
            print(f'  {sid:<16} ran         ({reason}; {seconds} s; receipt {short(key)})')
        previous_key = key
        if stop_after == sid:
            row['stopped_after'] = True
            print(f'  stopped after {sid} as requested')
            return rows, EXIT_OK
    return rows, EXIT_OK


# ------------------------------------------------------------------------------------------------------- bundle

SEAL_PREFIX = b'own-disc-seal sha256='
SEAL_LENGTH = len(SEAL_PREFIX) + 64
ENTRY_NAME = re.compile(r'[a-z0-9][a-z0-9._-]*(/[a-z0-9][a-z0-9._-]*)*')
BUNDLE_FILE_NAME = re.compile(r'[A-Za-z0-9._-]+')
MANIFEST_NAME = 'manifest.json'
MAX_MANIFEST_BYTES = 1 << 20
KNOWN_ROLES = ('game-library', 'title-xex', 'decoded-image', 'shader-pack')
ZIP_LIMIT = (1 << 32) - 2       # no ZIP64: every size and offset must fit 32 bits
EOCD = struct.Struct('<4sHHHHIIH')
LOCAL_HEADER = struct.Struct('<4sHHHHHIIIHH')    # signature, version, flags, method, time, date, CRC-32, sizes, lengths


def _zipinfo(name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    return info


def write_bundle(path: Path, manifest: dict, sources: dict[str, Path]) -> str:
    """Writes the bundle deterministically (same manifest + same component bytes = same file). Returns its seal."""
    names = [c['name'] for c in manifest['components']]
    if sum(c['size'] for c in manifest['components']) > ZIP_LIMIT - (64 << 20):
        raise BuilderError('bundle: larger than 4 GiB is not supported by format 1', EXIT_USAGE)
    tmp = path.with_name(path.name + '.tmp')
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(tmp, 'w', compression=zipfile.ZIP_STORED, allowZip64=False) as zf:
        zf.writestr(_zipinfo(MANIFEST_NAME), json.dumps(manifest, indent=1, sort_keys=True).encode() + b'\n')
        for name in sorted(names):
            with open(sources[name], 'rb') as src, zf.open(_zipinfo(name), 'w') as dst:
                shutil.copyfileobj(src, dst, BLOCK)
        zf.comment = b'\0' * SEAL_LENGTH
    size = tmp.stat().st_size
    with open(tmp, 'r+b') as f:
        seal = sha256_stream(f, size - SEAL_LENGTH)
        f.seek(size - SEAL_LENGTH)
        f.write(SEAL_PREFIX + seal.encode())
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    return seal


def check_manifest(manifest, bad) -> dict[str, dict]:
    """Shape and meaning of manifest.json. Returns the components by name."""
    if not isinstance(manifest, dict) or manifest.get('schema') not in (BUNDLE_SCHEMA, BUNDLE_SCHEMA_2):
        raise bad(f'the manifest schema is not {BUNDLE_SCHEMA} or {BUNDLE_SCHEMA_2}')
    second = manifest['schema'] == BUNDLE_SCHEMA_2
    game = manifest.get('game')
    if not isinstance(game, dict) or not isinstance(game.get('executable_sha256'), str) or \
            not HEX64.fullmatch(game['executable_sha256']):
        raise bad('the manifest names no game executable hash')
    if not isinstance(manifest.get('runtime_abi'), str) or not manifest['runtime_abi']:
        raise bad('the manifest names no runtime interface (runtime_abi)')
    comps = manifest.get('components')
    if not isinstance(comps, list) or not comps:
        raise bad('the manifest lists no components')
    by_name: dict[str, dict] = {}
    for c in comps:
        if not isinstance(c, dict) or not isinstance(c.get('name'), str) or type(c.get('size')) is not int or \
                not isinstance(c.get('sha256'), str) or not HEX64.fullmatch(c['sha256']):
            raise bad('a component in the manifest lacks a name, a size or a SHA-256')
        name = c['name']
        if not ENTRY_NAME.fullmatch(name) or name == MANIFEST_NAME or name in by_name:
            raise bad(f'component name {name!r} is not allowed or is listed twice')
        if c.get('role') not in KNOWN_ROLES and not (second and c.get('role') == RUNTIME_ROLE):
            raise bad(f'component {name} has the unknown role {c.get("role")!r}')
        if (c['role'] == RUNTIME_ROLE) != name.startswith(RUNTIME_PREFIX):
            raise bad(f'component {name}: runtime data, and nothing else, is named under {RUNTIME_PREFIX}')
        if c['role'] == 'title-xex' and c['sha256'] != game['executable_sha256']:
            raise bad('the bundled executable is not the one the manifest names')
        by_name[name] = c
    if second:
        for role in KNOWN_ROLES:
            if sum(c['role'] == role for c in by_name.values()) != 1:
                raise bad(f'a format-2 bundle has exactly one component of role {role}')
        if 'stages' in manifest or 'versions' in manifest:
            raise bad('a format-2 manifest carries no records of the machine that built it (stages, versions)')
        if not isinstance(manifest.get('portable_provenance'), dict):
            raise bad('a format-2 manifest states what the build is made from (portable_provenance)')
    return by_name


def verify_bundle(path: Path, executable_sha256: str | None = None) -> dict:
    """The check of a bundle file that the phone app makes on import (BUNDLE-FORMAT.md), made the same way: the
    seal first, then the entries front to back through their ZIP local file headers, as a reader of a stream sees
    them (the ZIP central directory is not consulted). Returns the manifest (plus "_seal"); raises
    BuilderError(EXIT_BUNDLE) naming the first problem. Whether the app implements the manifest's runtime_abi is
    the one check only the app can make."""
    def bad(why: str):
        return BuilderError(f'bundle {path.name}: {why}', EXIT_BUNDLE)

    try:
        size = path.stat().st_size
    except OSError:
        raise bad('cannot be read')
    if size < EOCD.size + SEAL_LENGTH or size > ZIP_LIMIT:
        raise bad('wrong size for a format-1 bundle')
    with open(path, 'rb') as f:
        # 1. the seal: the last 85 bytes are the ZIP comment "own-disc-seal sha256=<hex>" over everything before it
        end = size - SEAL_LENGTH - EOCD.size
        f.seek(end)
        sig, _, _, _, _, _, _, comment_len = EOCD.unpack(f.read(EOCD.size))
        comment = f.read(SEAL_LENGTH)
        if sig != b'PK\x05\x06' or comment_len != SEAL_LENGTH or not comment.startswith(SEAL_PREFIX):
            raise bad('the seal record is missing or damaged')
        f.seek(0)
        seal = sha256_stream(f, size - SEAL_LENGTH)
        if comment[len(SEAL_PREFIX):] != seal.encode():
            raise bad('the seal does not match the file contents (the file is damaged or was changed)')
        # 2. the entries in file order: the manifest, then exactly the components it lists
        pos, manifest, comps, seen = 0, None, {}, set()
        while True:
            f.seek(pos)
            head = f.read(LOCAL_HEADER.size)
            if manifest is not None and head[:4] in (b'PK\x01\x02', b'PK\x05\x06'):
                break                                               # the central directory: no more entries
            if len(head) < LOCAL_HEADER.size or head[:4] != b'PK\x03\x04':
                raise bad('no ZIP entry where one has to start')
            _, _, flags, method, _, _, crc, packed, length, name_len, extra_len = LOCAL_HEADER.unpack(head)
            name = f.read(name_len).decode('latin-1')
            if flags & ~0x0800 or method != 0 or packed != length:
                raise bad(f'entry {name!r} is compressed, encrypted or written without its size')
            data_at = pos + LOCAL_HEADER.size + name_len + extra_len
            pos = data_at + length
            if pos > end:
                raise bad(f'entry {name!r} runs past the end of the file')
            f.seek(data_at)
            if manifest is None:
                if name != MANIFEST_NAME:
                    raise bad(f'the first entry is not {MANIFEST_NAME}')
                if length > MAX_MANIFEST_BYTES:
                    raise bad('the manifest is larger than 1 MiB')
                data = f.read(length)
                if zlib.crc32(data) != crc:
                    raise bad('the manifest does not match its CRC-32')
                try:
                    manifest = json.loads(data)
                except ValueError:
                    raise bad('the manifest is not JSON')
                comps = check_manifest(manifest, bad)
                continue
            if name not in comps:
                raise bad(f'entry {name!r} is not a component the manifest lists')
            if name in seen:
                raise bad(f'entry {name} appears twice')
            seen.add(name)
            # 3. every component: size, CRC-32 and SHA-256
            if length != comps[name]['size']:
                raise bad(f'component {name}: size differs from the manifest')
            digest, crc_now = sha256_crc32(f, length)
            if crc_now != crc:
                raise bad(f'component {name}: does not match its CRC-32')
            if digest != comps[name]['sha256']:
                raise bad(f'component {name}: SHA-256 differs from the manifest')
        if len(seen) != len(comps):
            raise bad(f'component {sorted(set(comps) - seen)[0]} is missing')
    if executable_sha256 and manifest['game']['executable_sha256'] != executable_sha256:
        raise bad('built for a different game executable than the one given')
    return dict(manifest, _seal=seal)


def plan_output(profile: dict, ctx: Context) -> tuple[str, str, str] | None:
    """A profile whose result is one file a stage makes (the iOS app's .ipa) instead of a code bundle:
    "output": {"name": FILE, "stage": ID, "output": NAME}. Checked before any stage runs; None for a bundle profile."""
    spec = profile.get('output')
    if spec is None:
        return None
    if 'bundle' in profile:
        raise BuilderError('profile: give either "bundle" or "output", not both', EXIT_USAGE)
    name = ctx.expand(str(spec.get('name', '')))
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,120}', name) or name.endswith('.part'):
        raise BuilderError(f'profile: bad output name {name!r}', EXIT_USAGE)
    stage, out = spec.get('stage'), spec.get('output')
    if stage not in ctx.outputs or out not in ctx.outputs[stage]:
        raise BuilderError(f'profile: the output names no output of a stage ({stage}:{out})', EXIT_USAGE)
    return name, stage, out


def plan_bundle(profile: dict, ctx: Context) -> str:
    """Checks the profile's bundle description against the stages before any stage runs. Returns the file name."""
    spec = profile.get('bundle') or {}
    name = ctx.expand(spec.get('name', 'code-${profile}-${flavour}.bundle'))
    if not BUNDLE_FILE_NAME.fullmatch(name):
        raise BuilderError(f'profile: bad bundle name {name!r}', EXIT_USAGE)
    if not isinstance(spec.get('runtime_abi'), str) or not spec['runtime_abi']:
        raise BuilderError('profile: bundle.runtime_abi is missing', EXIT_USAGE)
    second = spec.get('format', 1) == 2
    if spec.get('format', 1) not in (1, 2):
        raise BuilderError('profile: bundle.format must be 1 or 2', EXIT_USAGE)
    names = [c.get('name', '') for c in spec.get('components') or []]
    if not names or len(set(names)) != len(names):
        raise BuilderError('profile: bundle.components is empty or names a component twice', EXIT_USAGE)
    for c, component in zip(spec['components'], names):
        known = c.get('role') in KNOWN_ROLES or (second and c.get('role') == RUNTIME_ROLE)
        if not ENTRY_NAME.fullmatch(component) or component == MANIFEST_NAME or not known:
            raise BuilderError(f'profile: bundle component {component!r} has a bad name or an unknown role '
                               f'{c.get("role")!r}', EXIT_USAGE)
        if (c['role'] == RUNTIME_ROLE) != component.startswith(RUNTIME_PREFIX):
            raise BuilderError(f'profile: bundle component {component!r}: runtime data, and nothing else, is named '
                               f'under {RUNTIME_PREFIX}', EXIT_USAGE)
        if c.get('output') not in ctx.outputs.get(c.get('stage'), {}):
            raise BuilderError(f'profile: bundle component {component} names no output of a stage '
                               f'({c.get("stage")}:{c.get("output")})', EXIT_USAGE)
    if second:
        for role in KNOWN_ROLES:
            if sum(c['role'] == role for c in spec['components']) != 1:
                raise BuilderError(f'profile: a format-2 bundle has exactly one component of role {role}', EXIT_USAGE)
        stated = spec.get('provenance', {})
        if not isinstance(stated, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in stated.items()) \
                or 'tools' in stated:
            raise BuilderError('profile: bundle.provenance must map names to texts (and not use the name "tools")', EXIT_USAGE)
        for tool in spec.get('portable_tools', []):
            if not isinstance(tool, str) or not Path(ctx.expand(tool)).is_file():
                raise BuilderError(f'profile: bundle.portable_tools names no file: {tool!r}', EXIT_USAGE)
    return name


def build_bundle(profile: dict, ctx: Context, rows: list[dict], path: Path) -> str:
    spec = profile['bundle']
    stage_by_id = {r['stage']: r for r in rows}
    comps, sources = [], {}
    for c in spec['components']:
        src = ctx.outputs[c['stage']][c['output']]
        if not src.is_file():
            raise BuilderError(f'bundle: component {c["name"]} needs output {c["output"]} of stage {c["stage"]} (a file)')
        sources[c['name']] = src
        comps.append({'name': c['name'], 'role': c['role'], 'size': src.stat().st_size, 'sha256': sha256_file(src),
                      'stage': c['stage'], 'provenance': stage_by_id[c['stage']]['provenance']})
    game = {'id': ctx.game.get('id', ''), 'executable_sha256': ctx.game['executable_sha256'],
            'executable_size': ctx.game['executable_size']}
    if spec.get('format', 1) == 2:
        # A tool is named as the profile writes it (unexpanded, so without this machine's paths) with its content hash.
        tools = [{'name': t, 'sha256': sha256_file(Path(ctx.expand(t)))} for t in sorted(spec.get('portable_tools', []))]
        manifest = {
            'schema': BUNDLE_SCHEMA_2,
            'profile': profile.get('id', ''),
            'flavour': profile.get('flavour', ''),
            'game': game,
            'runtime_abi': spec['runtime_abi'],
            'portable_provenance': dict(spec.get('provenance', {}), tools=tools),
            'components': sorted(comps, key=lambda c: c['name']),
        }
        return _write_checked(path, manifest, sources, ctx, len(comps))
    manifest = {
        'schema': BUNDLE_SCHEMA,
        'profile': profile.get('id', ''),
        'flavour': profile.get('flavour', ''),
        'game': game,
        'runtime_abi': spec['runtime_abi'],
        'versions': dict(spec.get('versions', {}), builder_sha256=hashlib.sha256(__loader__.get_data(__file__)).hexdigest()),
        'stages': [{'id': r['stage'], 'key': r['key'], 'provenance': r['provenance']} for r in rows],
        'components': sorted(comps, key=lambda c: c['name']),
    }
    return _write_checked(path, manifest, sources, ctx, len(comps))


def _write_checked(path: Path, manifest: dict, sources: dict, ctx: Context, count: int) -> str:
    seal = write_bundle(path, manifest, sources)
    try:
        verify_bundle(path, ctx.game['executable_sha256'])   # read back what was written, as the app will
    except BuilderError:
        path.unlink()                                        # a bundle the app would refuse is not left behind
        raise
    print(f'  {"bundle":<16} written     {path} ({path.stat().st_size} bytes, {count} components, seal {short(seal)})')
    return seal


# ---------------------------------------------------------------------------------------------------------- CLI

def parse_supply(items: list[str]) -> dict:
    out = {}
    for item in items:
        if '=' not in item:
            raise BuilderError(f'--supply wants STAGE=PATH, got {item!r}', EXIT_USAGE)
        stage, path = item.split('=', 1)
        out[stage] = path
    return out


def cmd_build(a) -> int:
    profile = load_profile(a.profile)
    work = a.work.resolve()
    out_dir = (a.out or work / 'out').resolve()
    check_private_path(work, 'work directory')
    check_private_path(out_dir, 'bundle directory')
    if a.report:
        check_private_path(a.report, 'report file')
    check_work_dir(work)
    stage_ids = [s['id'] for s in profile['stages']]
    if a.stop_after and a.stop_after not in stage_ids:
        raise BuilderError(f'--stop-after {a.stop_after}: the profile has no such stage (it has: '
                           f'{", ".join(stage_ids)})', EXIT_USAGE)
    ctx = Context(profile, work, a.repo, a.data_root)
    supply = parse_supply(a.supply)
    report: dict = {'profile': profile.get('id', ''), 'dry_run': a.dry_run, 'work': str(work)}
    started = False                     # True once the work directory is in use

    def finish(code: int) -> int:
        report['exit_code'] = code
        target = a.report or (work / 'last-run.json' if started else None)
        if target:
            write_atomic(target, json.dumps(report, indent=1, sort_keys=True).encode() + b'\n')
        return code

    print(f'Profile {profile.get("id", "?")} (flavour {profile.get("flavour", "-")}); work directory {work}'
          + ('; DRY RUN: no stage runs and nothing is written but the --report file (the version commands of the '
             'prerequisites do run)' if a.dry_run else ''))
    # 1. identify
    ctx.discs['play'] = open_disc(a.play_disc)
    if a.install_disc:
        ctx.discs['install'] = open_disc(a.install_disc)
    ctx.game = identify(profile, ctx.discs['play'])
    report['game'] = {k: ctx.game[k] for k in ('id', 'executable', 'executable_sha256', 'executable_size') if k in ctx.game}
    print(f'Game: {ctx.game.get("label", ctx.game.get("id", "?"))} ({ctx.discs["play"].kind} input, '
          f'{ctx.game["executable"]} {short(ctx.game["executable_sha256"])}, {ctx.game["executable_size"]} bytes)')
    if 'install' not in ctx.discs:
        print('Install disc: not given (the code bundle does not need it unless a stage says so below).')
    # 2. plan: every path and variable of the profile, before anything runs
    plan_paths(profile, ctx)
    output = plan_output(profile, ctx)
    bundle_path = out_dir / (output[0] if output else plan_bundle(profile, ctx))
    # 3. prerequisites (and the other disc files), all before any stage
    failures = check_disc_files(ctx.game, ctx.discs, a.hash_disc_files) + check_prerequisites(profile, ctx, supply)
    for s in supply:
        if s not in [x['id'] for x in profile['stages'] if x['adapter'] == 'supplied']:
            failures.append(f'--supply {s}: no stage of the "supplied" kind has this name')
    report['prerequisite_failures'] = failures
    if failures:
        print(f'Prerequisites: {len(failures)} problem(s)')
        for f in failures:
            print(f'  MISSING  {f}')
        if not a.dry_run:
            print('Stopped before any stage ran.')
            return finish(EXIT_PREREQ)
    else:
        print('Prerequisites: all present.')
    # 4. stages
    print('Stages:')
    if not a.dry_run:
        work.mkdir(parents=True, exist_ok=True)
        if not (work / WORK_MARKER).exists():
            write_atomic(work / WORK_MARKER, b'Work directory of tools/own_disc/disc_builder.py: the builder removes and '
                                             b'rewrites folders in here.\n')
        started = True
    rows, code = run_pipeline(profile, ctx, dry_run=a.dry_run, allow_heavy=a.allow_heavy, supply=supply,
                              stop_after=a.stop_after, nice=a.nice)
    report['stages'] = rows
    if a.dry_run:
        would = [r['stage'] for r in rows if r['action'] == 'would-run']
        print(f'  {"output" if output else "bundle":<16} would be written')
        print(f'Dry run: {len(would)} stage(s) would run ({", ".join(would) or "none"}), '
              f'{sum(r["action"] == "skipped" for r in rows)} covered by receipts.')
        return finish(EXIT_PREREQ if failures else code)
    if code != EXIT_OK or any(r.get('stopped_after') for r in rows):
        return finish(code)
    # 5. bundle (or the one output file of an output profile)
    if output:
        source = ctx.outputs[output[1]][output[2]]
        if not source.is_file():
            print(f'  {"output":<16} FAILED      the stage {output[1]} did not write {source.name}')
            return finish(EXIT_STAGE)
        check_private_path(bundle_path, 'the output')
        bundle_path.parent.mkdir(parents=True, exist_ok=True)
        tmp = bundle_path.with_name(bundle_path.name + '.part')
        shutil.copyfile(source, tmp)
        os.replace(tmp, bundle_path)
        digest = sha256_file(bundle_path)
        report['output'] = {'path': str(bundle_path), 'sha256': digest, 'size': bundle_path.stat().st_size}
        print(f'  {"output":<16} written     {bundle_path}')
        print(f'Done. Output {bundle_path.name}: sha256 {digest}')
        return finish(EXIT_OK)
    try:
        seal = build_bundle(profile, ctx, rows, bundle_path)
    except BuilderError as e:
        print(f'  {"bundle":<16} FAILED      {e}')
        return finish(e.code)
    report['bundle'] = {'path': str(bundle_path), 'seal': seal, 'size': bundle_path.stat().st_size}
    print(f'Done. Bundle seal (compare with what the phone app shows on import): {seal}')
    return finish(EXIT_OK)


def cmd_identify(a) -> int:
    profile = load_profile(a.profile)
    disc = open_disc(a.play_disc)
    game = identify(profile, disc)
    print(f'{game.get("label", game.get("id", "?"))}: {game["executable"]} {short(game["executable_sha256"])}, '
          f'{game["executable_size"]} bytes ({disc.kind} input)')
    return EXIT_OK


def cmd_verify(a) -> int:
    manifest = verify_bundle(a.bundle, a.executable_sha256)
    print(f'{a.bundle.name}: verified. profile {manifest.get("profile")}, flavour {manifest.get("flavour")}, '
          f'runtime interface {manifest["runtime_abi"]}, game executable {short(manifest["game"]["executable_sha256"])}, '
          f'seal {manifest["_seal"]}')
    for c in manifest['components']:
        print(f'  {c["name"]:<28} {c["role"]:<16} {c["size"]:>12} bytes  {short(c["sha256"])}  {c.get("provenance", "")}')
    return EXIT_OK


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split('\n\n', 1)[1])
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build', help='identify, check prerequisites, run the stages, write the bundle')
    b.add_argument('--profile', type=Path, required=True)
    b.add_argument('--play-disc', type=Path, required=True)
    b.add_argument('--install-disc', type=Path)
    b.add_argument('--work', type=Path, required=True)
    b.add_argument('--out', type=Path, help='where the bundle goes (default WORK/out)')
    b.add_argument('--dry-run', action='store_true')
    b.add_argument('--allow-heavy', action='store_true')
    b.add_argument('--stop-after')
    b.add_argument('--supply', action='append', default=[], metavar='STAGE=PATH')
    b.add_argument('--hash-disc-files', action='store_true', help='also hash the other listed disc files (slow)')
    b.add_argument('--report', type=Path)
    b.add_argument('--repo', type=Path)
    b.add_argument('--data-root', type=Path)
    b.add_argument('--nice', type=int, default=10)
    b.set_defaults(fn=cmd_build)
    i = sub.add_parser('identify', help='only identify the game on the play disc')
    i.add_argument('--profile', type=Path, required=True)
    i.add_argument('--play-disc', type=Path, required=True)
    i.set_defaults(fn=cmd_identify)
    v = sub.add_parser('verify', help='verify a bundle file')
    v.add_argument('bundle', type=Path)
    v.add_argument('--executable-sha256')
    v.set_defaults(fn=cmd_verify)
    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except BuilderError as e:
        print(f'ERROR: {e}')
        return e.code


if __name__ == '__main__':
    raise SystemExit(main())
