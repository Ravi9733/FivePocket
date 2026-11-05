#!/usr/bin/env python3
"""Player entry point: builder check|fetch|build|verify|sign. No shell or device commands.

iOS (a Mac with Xcode, 2026-11-05): a release may name more targets than its default profile in "profiles"
({"ios": REL, "duo": REL}); builder build --ios / --duo builds that target's unsigned .ipa with the player's bundle
identifier (the profiles take it as ${bundleid}): on the first iOS build a random one per player, com.fivepocket.<8 hex>.ios
and .duo, kept in fivepocket-builder-settings.json next to the work folder and used by every later build (re-signing an app
with another identifier makes a new app with empty storage: the import and the saves are lost; and a fixed identifier
would collide between players' free Apple teams). --bundle-id overrides it for one build and is not stored. builder sign --ipa FILE --identity ID [--profile-file
FILE.mobileprovision] [--entitlements FILE.plist] --out NEW.ipa signs it with the player's own identity (ios_sign.py,
codesign reads the player's keychain) and writes the signed .ipa.

A release is an audited payload plus builder-release/1 JSON. This development
lane intentionally supplies no production release pins. Fetch has no usable
publisher URL until an approved release is supplied; tests use local ZIPs only.
"""
from __future__ import annotations
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import posixpath
import re
import subprocess
import shutil
import stat
import sys
import tempfile
import urllib.request
import uuid
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import disc_builder as db
import host_support


class Refusal(Exception):
    pass


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def host_id():
    machine = platform.machine().lower()
    machine = {'amd64': 'x86_64', 'aarch64': 'arm64'}.get(machine, machine)
    return f'{sys.platform}-{machine}'


def safe_name(name):
    # Same archive vocabulary on NTFS, APFS and ext4. No aliases/ADS/drive names.
    if not isinstance(name, str) or '\\' in name or not name or name.startswith('/'):
        raise Refusal('invalid package path; obtain a corrected release')
    parts = name.rstrip('/').split('/')
    for part in parts:
        if part in ('', '.', '..') or part.endswith(('.', ' ')) or re.search(r'[<>:"|?*\x00-\x1f]', part):
            raise Refusal('invalid package path; obtain a corrected release')
        if re.fullmatch(r'(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?', part, re.I):
            raise Refusal('reserved Windows package path; obtain a corrected release')
    return PurePosixPath(*parts)


def inside(root, name):
    result = root.joinpath(*safe_name(name).parts).resolve()
    if not result.is_relative_to(root.resolve()):
        raise Refusal('package path escapes its root; obtain an intact release')
    return result


def load_release(path):
    r = read_json(path)
    root = path.resolve().parent
    if r.get('schema') != 'builder-release/1' or r.get('host') != host_id():
        raise Refusal(f'wrong or missing release for {host_id()}; obtain the matching builder download')
    if not isinstance(r.get('files'), list) or not r['files']:
        raise Refusal('release inventory is missing; obtain a fully populated, approved builder release')
    seen = set()
    for rec in r['files']:
        p = inside(root, rec['path'])
        key = rec['path'].casefold()
        if key in seen or not re.fullmatch('[0-9a-f]{64}', str(rec.get('sha256'))):
            raise Refusal('invalid release inventory; obtain an intact builder download')
        seen.add(key)
        if not p.is_file() or db.sha256_file(p) != rec['sha256']:
            raise Refusal(f'payload missing or changed: {rec["path"]}; re-extract the approved builder download')
        if rec.get('executable') and os.name != 'nt' and not os.access(p, os.X_OK):
            raise Refusal(f'payload is not executable: {rec["path"]}; re-extract preserving executable permissions')
    listed = [f['path'] for f in r['files']]
    for key in ('profile', 'python'):
        if key in r and r[key] not in listed:
            raise Refusal(f'{key} is not hash-bound in the release; obtain a corrected release')
    targets = r.get('profiles', {})
    if not isinstance(targets, dict) or any(t not in TARGETS or v not in listed for t, v in targets.items()):
        raise Refusal('a target profile is not hash-bound in the release; obtain a corrected release')
    if 'profile' not in r:
        raise Refusal('no production profile; obtain the approved B1/B2 profile')
    return r, root


def ndk_spec(release):
    n = release.get('ndk')
    if not isinstance(n, dict) or not re.fullmatch('[0-9a-f]{64}', str(n.get('sha256'))) or not isinstance(n.get('size'), int) or n['size'] <= 0:
        raise Refusal('NDK publisher SHA-256/size not pinned; ask the release publisher for a completed release')
    if not re.fullmatch(r'https://dl\.google\.com/android/repository/android-ndk-[a-zA-Z0-9._-]+\.(zip|dmg)', n.get('url', '')):
        raise Refusal('NDK URL is not a pinned Google archive; obtain a corrected release')
    if n['url'].endswith('.dmg'):
        # Google publishes the macOS NDK only as a disk image: image_root names the NDK folder inside it
        # (AndroidNDK<build>.app/Contents/NDK), which is installed as `directory`
        if not isinstance(n.get('image_root'), str) or not n['image_root']:
            raise Refusal('NDK image root not declared; obtain a corrected release')
        safe_name(n['image_root'])
    if n.get('license_url') != 'https://developer.android.com/ndk/downloads':
        raise Refusal('NDK licence link missing; obtain a corrected release')
    safe_name(n['directory'])
    if len(PurePosixPath(n['directory']).parts) != 1 or type(n.get('unpacked_bytes')) is not int or n['unpacked_bytes'] <= 0:
        raise Refusal('NDK unpacked size/root not declared; obtain a corrected release')
    return n


def existing_ndk(cache, n):
    target = cache / n['sha256'][:16]
    record = target / 'installed.json'
    if not record.is_file():
        raise Refusal('compiler missing; read the NDK terms and run builder fetch --accept-ndk-license')
    data = read_json(record)
    if data.get('archive_sha256') != n['sha256'] or not data.get('files'):
        raise Refusal('compiler receipt differs; use a new cache and run builder fetch again')
    for name, digest in data['files'].items():
        p = inside(target, name)
        if not p.is_file() or db.sha256_file(p) != digest:
            raise Refusal('compiler files changed; use a new cache and run builder fetch again')
    return target / n['directory']


def link_target(rel: PurePosixPath, link: str, directory: str) -> str:
    """the archive path a relative link inside the compiler folder names; refused when it leaves the folder"""
    if link.startswith('/') or '\\' in link or ':' in link:
        raise Refusal('archive link escapes its root; obtain a corrected archive')
    linked = safe_name(posixpath.normpath(str(rel.parent / link)))
    if linked.parts[0] != directory:
        raise Refusal('archive link escapes its root; obtain a corrected archive')
    return str(linked)


def zip_payload(zip_path: Path, extracted: Path, n: dict, inventory: dict, links: dict) -> int:
    """Extracts the NDK folder of the verified ZIP: safe names under the declared root, no case-insensitive duplicates,
    links only to files inside the folder (materialized later as copies), no other special files, the declared size
    bound. Returns the bytes written."""
    expanded, seen = 0, set()
    with zipfile.ZipFile(zip_path) as z:
        for member in z.infolist():
            rel = safe_name(member.filename)
            if rel.parts[0] != n['directory']:
                raise Refusal('archive has unexpected root; obtain the pinned archive')
            key = str(rel).casefold()
            if key in seen:
                raise Refusal('archive has duplicate case-insensitive names; obtain the corrected archive')
            seen.add(key)
            mode = member.external_attr >> 16
            if stat.S_ISLNK(mode):
                if member.file_size > 4096:
                    raise Refusal('oversized archive link; obtain a corrected archive')
                links[str(rel)] = link_target(rel, z.read(member).decode('utf-8'), n['directory'])
                continue
            if stat.S_IFMT(mode) and not (stat.S_ISREG(mode) or stat.S_ISDIR(mode)):
                raise Refusal('archive contains unsupported links/devices; obtain a supported publisher archive')
            dest = inside(extracted, str(rel))
            if member.is_dir():
                dest.mkdir(parents=True, exist_ok=True); continue
            expanded += member.file_size
            if expanded > n['unpacked_bytes']:
                raise Refusal('archive exceeds declared extraction bound; obtain a corrected release')
            dest.parent.mkdir(parents=True, exist_ok=True)
            with z.open(member) as source, dest.open('xb') as out:
                shutil.copyfileobj(source, out)
            if mode & 0o111:
                dest.chmod(0o755)
            inventory[str(rel)] = db.sha256_file(dest)
    return expanded


def dmg_payload(dmg: Path, extracted: Path, n: dict, inventory: dict, links: dict) -> int:
    """Copies the NDK folder out of the verified macOS disk image (attached read-only, not shown in Finder, detached
    again; the mount point is a new folder in the system's temporary folder, since macOS refuses to attach an image on
    a volume without owners, as external drives often are) under the rules of the ZIP path: safe names, no case-insensitive duplicates, links only to files inside the
    folder (materialized later as copies), no other special files, the declared size bound. Returns the bytes copied."""
    if sys.platform != 'darwin':
        raise Refusal('this compiler archive is a macOS disk image; use the release for this computer')
    mount = Path(tempfile.mkdtemp(prefix='builder-ndk-image-'))
    subprocess.run(['/usr/bin/hdiutil', 'attach', '-readonly', '-nobrowse', '-noautoopen', '-mountpoint', str(mount), str(dmg)],
                   check=True, capture_output=True)
    expanded, seen = 0, set()
    try:
        src = mount / n['image_root']
        if src.is_symlink() or not src.is_dir():
            raise Refusal('disk image has no NDK folder at the declared root; obtain the pinned archive')
        for here, dirs, files in os.walk(src, followlinks=False):
            for name in sorted(dirs) + sorted(files):
                p = Path(here) / name
                rel = safe_name(str(PurePosixPath(n['directory']) / p.relative_to(src).as_posix()))
                key = str(rel).casefold()
                if key in seen:
                    raise Refusal('archive has duplicate case-insensitive names; obtain the corrected archive')
                seen.add(key)
                mode = p.lstat().st_mode
                if stat.S_ISLNK(mode):
                    links[str(rel)] = link_target(rel, os.readlink(p), n['directory'])
                    continue
                dest = inside(extracted, str(rel))
                if stat.S_ISDIR(mode):
                    dest.mkdir(parents=True, exist_ok=True); continue
                if not stat.S_ISREG(mode):
                    raise Refusal('archive contains unsupported links/devices; obtain a supported publisher archive')
                expanded += p.stat().st_size
                if expanded > n['unpacked_bytes']:
                    raise Refusal('archive exceeds declared extraction bound; obtain a corrected release')
                dest.parent.mkdir(parents=True, exist_ok=True)
                with p.open('rb') as source, dest.open('xb') as out:
                    shutil.copyfileobj(source, out)
                if mode & 0o111:
                    dest.chmod(0o755)
                inventory[str(rel)] = db.sha256_file(dest)
    finally:
        subprocess.run(['/usr/bin/hdiutil', 'detach', str(mount)], capture_output=True)
        with contextlib.suppress(OSError):
            mount.rmdir()                                # only the empty mount point; never anything with content
    return expanded


class GoogleOnlyRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not newurl.startswith('https://dl.google.com/android/repository/'):
            raise Refusal('publisher redirect left Google; obtain the pinned archive directly')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def fetch(release, cache, accepted, archive=None, opener=None):
    n = ndk_spec(release)
    print(f'Compiler: {n["url"]}; {n["size"]} bytes; SHA-256 {n["sha256"][:12]}')
    print(f'Android SDK License Agreement and component notices: {n["license_url"]}')
    if not accepted:
        raise Refusal('licence not accepted; read those terms, then rerun fetch with --accept-ndk-license if you agree')
    host_support.private_output(cache)
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / n['sha256'][:16]
    if target.exists():
        existing_ndk(cache, n)
        print('Compiler already verified.'); return
    if shutil.disk_usage(cache).free < n['size'] + n['unpacked_bytes']:
        raise Refusal('not enough disk for archive and extracted compiler; free space or choose --cache elsewhere')
    lock = cache / '.fetch-lock'
    try:
        guard = lock.open('x', encoding='utf-8', newline='\n')
    except FileExistsError:
        raise Refusal('another fetch owns the cache; wait for it or inspect the stale lock before removing it')
    try:
        with guard:
            guard.write(str(os.getpid()))
        staged = cache / ('fetch-' + uuid.uuid4().hex)
        staged.mkdir()
        zip_path = staged / ('download.dmg' if n['url'].endswith('.dmg') else 'download.zip')
        # No request is opened until all release/consent/disk checks above passed.
        stream = open(archive, 'rb') if archive else (opener or urllib.request.build_opener(GoogleOnlyRedirect()).open)(n['url'], timeout=60)
        h, total = hashlib.sha256(), 0
        with stream, zip_path.open('xb') as out:
            while block := stream.read(1 << 20):
                total += len(block)
                if total > n['size']:
                    raise Refusal('archive exceeds pinned size; keep failed evidence and obtain the correct archive')
                h.update(block); out.write(block)
        if total != n['size'] or h.hexdigest() != n['sha256']:
            raise Refusal('archive hash/size mismatch; nothing installed; obtain the pinned publisher archive')
        extracted = staged / 'payload'; extracted.mkdir()
        inventory, links = {}, {}
        payload = dmg_payload if zip_path.suffix == '.dmg' else zip_payload
        expanded = payload(zip_path, extracted, n, inventory, links)
        # Materialize only file links after their validated targets exist. This
        # works on Windows without privilege; no symlink can redirect extraction.
        while links:
            progress = False
            for name, target_name in list(links.items()):
                if target_name in inventory:
                    source, dest = inside(extracted, target_name), inside(extracted, name)
                    expanded += source.stat().st_size
                    if expanded > n['unpacked_bytes']:
                        raise Refusal('archive exceeds declared extraction bound; obtain a corrected release')
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, dest)
                    inventory[name] = db.sha256_file(dest)
                    del links[name]; progress = True
            if not progress:
                raise Refusal('archive has dangling, cyclic or directory links; obtain a supported archive')
        if not inventory:
            raise Refusal('archive has no compiler files; obtain the correct archive')
        db.write_atomic(extracted / 'installed.json', json.dumps({'archive_sha256': n['sha256'], 'files': inventory}, sort_keys=True).encode())
        os.replace(extracted, target)
        # Retain the verified publisher archive as evidence; never delete a pre-existing file.
        print(f'Compiler installed and verified: {target / n["directory"]}')
    finally:
        lock.unlink()


class PublisherOnlyRedirect(urllib.request.HTTPRedirectHandler):
    # A release file of the shader compiler is handed out by the publisher's own file hosts; nowhere else is followed.
    HOSTS = ('https://github.com/microsoft/DirectXShaderCompiler/', 'https://objects.githubusercontent.com/', 'https://release-assets.githubusercontent.com/')

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not newurl.startswith(self.HOSTS):
            raise Refusal('publisher redirect left the publisher\'s hosts; obtain the pinned archive directly')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def dxc_spec(release):
    """The shader compiler the builder fetches instead of carrying it (Microsoft's DirectX Shader Compiler release):
    None when the release names none, else the checked pins: address, size and hash of the archive, and for every file
    taken out of it its name in the archive, its place below the package root and its hash."""
    d = release.get('dxc')
    if d is None:
        return None
    if not isinstance(d, dict) or not re.fullmatch('[0-9a-f]{64}', str(d.get('sha256'))) or type(d.get('size')) is not int or d['size'] <= 0:
        raise Refusal('shader compiler archive SHA-256/size not pinned; ask the release publisher for a completed release')
    if not re.fullmatch(r'https://github\.com/microsoft/DirectXShaderCompiler/releases/download/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+\.zip', str(d.get('url', ''))):
        raise Refusal('shader compiler URL is not a pinned Microsoft release file; obtain a corrected release')
    if not str(d.get('license_url', '')).startswith('https://github.com/microsoft/DirectXShaderCompiler/'):
        raise Refusal('shader compiler licence link missing; obtain a corrected release')
    files = d.get('files')
    if not isinstance(files, list) or not files:
        raise Refusal('shader compiler files not listed; obtain a corrected release')
    places = set()
    for rec in files:
        if not isinstance(rec, dict) or not re.fullmatch('[0-9a-f]{64}', str(rec.get('sha256'))) or type(rec.get('size')) is not int or rec['size'] <= 0:
            raise Refusal('shader compiler file not pinned; obtain a corrected release')
        safe_name(rec.get('member')); place = str(safe_name(rec.get('path'))).casefold()
        if place in places:
            raise Refusal('shader compiler files share a place; obtain a corrected release')
        places.add(place)
    return d


def existing_dxc(release, root):
    """Every file of the fetched shader compiler is at its place with its pinned hash, or the build is refused."""
    d = dxc_spec(release)
    if d is None:
        return
    for rec in d['files']:
        p = inside(root, rec['path'])
        if not p.is_file():
            raise Refusal('shader compiler missing; read its terms and run builder fetch --accept-dxc-license')
        if db.sha256_file(p) != rec['sha256']:
            raise Refusal(f'shader compiler file changed: {rec["path"]}; unpack the builder again and run builder fetch --accept-dxc-license')


def fetch_dxc(release, root, cache, accepted, archive=None, opener=None):
    """Fetches the pinned release of the shader compiler from its publisher (or takes a copy of the same archive from
    disk) and puts the listed files at their places below the package root. A file already there is never written
    over: with the pinned hash it stays, with another the run is refused."""
    d = dxc_spec(release)
    if d is None:
        return
    print(f'Shader compiler: {d["url"]}; {d["size"]} bytes; SHA-256 {d["sha256"][:12]}')
    print(f'Its licence terms (Microsoft and LLVM), also inside the archive: {d["license_url"]}')
    missing = []
    for rec in d['files']:
        p = inside(root, rec['path'])
        if p.is_file():
            if db.sha256_file(p) != rec['sha256']:
                raise Refusal(f'another file lies where the shader compiler goes: {rec["path"]}; nothing is written over; unpack the builder again')
        elif p.exists() or p.is_symlink():
            raise Refusal(f'something that is no file lies where the shader compiler goes: {rec["path"]}; unpack the builder again')
        else:
            missing.append(rec['path'])
    if not missing:
        print('Shader compiler already verified.'); return
    if not accepted:
        raise Refusal('licence not accepted; read those terms, then rerun fetch with --accept-dxc-license if you agree')
    host_support.private_output(cache)
    cache.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(cache).free < d['size'] + sum(rec['size'] for rec in d['files']):
        raise Refusal('not enough disk for the shader compiler archive and its files; free space or choose --cache elsewhere')
    staged = cache / ('fetch-dxc-' + uuid.uuid4().hex)
    staged.mkdir()
    zip_path = staged / 'download.zip'
    # No request is opened until all release/consent/disk checks above passed.
    stream = open(archive, 'rb') if archive else (opener or urllib.request.build_opener(PublisherOnlyRedirect()).open)(d['url'], timeout=60)
    h, total = hashlib.sha256(), 0
    with stream, zip_path.open('xb') as out:
        while block := stream.read(1 << 20):
            total += len(block)
            if total > d['size']:
                raise Refusal('archive exceeds pinned size; keep failed evidence and obtain the correct archive')
            h.update(block); out.write(block)
    if total != d['size'] or h.hexdigest() != d['sha256']:
        raise Refusal('archive hash/size mismatch; nothing installed; obtain the pinned publisher archive')
    taken = {}
    with zipfile.ZipFile(zip_path) as z:
        names = {info.filename.replace('\\', '/'): info for info in z.infolist()}      # (the publisher's archive names its members with backslashes)
        for rec in d['files']:
            info = names.get(rec['member'])
            if info is None or info.file_size != rec['size']:
                raise Refusal(f'shader compiler archive lacks {rec["member"]} as pinned; nothing installed; obtain the pinned publisher archive')
            data = z.read(info)
            if len(data) != rec['size'] or hashlib.sha256(data).hexdigest() != rec['sha256']:
                raise Refusal(f'{rec["member"]} is not the pinned file; nothing installed; obtain the pinned publisher archive')
            taken[rec['path']] = data
    for name in missing:                      # only now, with every file of the archive checked, is anything put into the package
        data = taken[name]
        dest = inside(root, name)
        dest.parent.mkdir(parents=True, exist_ok=True)
        try:
            out = dest.open('xb')             # a file that appeared since the look above is not written over either
        except FileExistsError:
            raise Refusal(f'a file appeared where the shader compiler goes: {name}; nothing is written over; unpack the builder again')
        with out:
            out.write(data)
    existing_dxc(release, root)
    print(f'Shader compiler installed and verified: {len(missing)} of {len(taken)} files put below {root}')


def check_paths(work, cache):
    # Until the whole native toolchain has passed long-path tests, refuse early;
    # no administrator/policy changes are requested or made.
    if sys.platform == 'win32':
        for p in (work.resolve(), cache.resolve()):
            if len(str(p)) > 80:
                raise Refusal('Windows build/cache path exceeds 80 characters; choose a short user-owned folder such as C:\\b')
    host_support.private_output(work)
    host_support.private_output(cache)


@contextlib.contextmanager
def environment(values):
    before = {k: os.environ.get(k) for k in values}
    os.environ.update(values)
    try:
        yield
    finally:
        for k, v in before.items():
            if v is None: os.environ.pop(k, None)
            else: os.environ[k] = v


class DiscNamedAtBuild:
    # Stands for a disc while the plan is checked: a command line may name it, nothing of it can be read.
    def __init__(self, role):
        self.image = f'<the {role} disc>'


SETTINGS_NAME = 'fivepocket-builder-settings.json'
TARGETS = ('ios', 'duo', 'thor', 'fold')          # a release's other profiles; ios and duo make apps (a bundle id each)
APP_TARGETS = ('ios', 'duo')
ANDROID_ELSEWHERE = 'The Android editions are built with the Windows builder in this release.'


def bundle_id_for(target: str, work: Path, given: str | None = None) -> str:
    """the player's own identifier for target: --bundle-id when given, else the one kept next to the work folder (made once)"""
    if given:
        return given
    settings = work.parent / SETTINGS_NAME
    data = read_json(settings) if settings.is_file() else {}
    player = data.get('player_key')
    if not (isinstance(player, str) and re.fullmatch(r'[0-9a-f]{8}', player)):
        player = uuid.uuid4().hex[:8]
        data['player_key'] = player
        settings.parent.mkdir(parents=True, exist_ok=True)
        tmp = settings.with_name(settings.name + '.part')
        tmp.write_text(json.dumps(data, indent=1, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
        os.replace(tmp, settings)
    return f'com.fivepocket.{player}.{target}'


def say_bundle_id(bundle_id: str, target: str) -> None:
    print(f'Bundle identifier of your {target} app: {bundle_id}\n'
          'ALWAYS SIGN WITH THIS IDENTIFIER. Signing with another one makes a new app with empty storage: '
          'the imported discs and your saves would be gone.')


def profile_of(release, target):
    if target is None:
        return release['profile']
    if target not in release.get('profiles', {}):
        raise Refusal(f'this builder release makes no {target} app; obtain the Mac builder release that does')
    return release['profiles'][target]


def check(release, root, work, cache, target=None):
    check_paths(work, cache)
    estimate = release.get('estimates', {})
    need = estimate.get('min_free_bytes')
    probe = work
    while not probe.exists(): probe = probe.parent
    free = shutil.disk_usage(probe).free
    print(f'Disk available: {free} bytes.')
    if type(need) is not int or need <= 0:
        raise Refusal('disk requirement unmeasured; obtain a release with the B1/B7 disk budget')
    print(f'Disk required: {need} bytes. Build time ESTIMATE: {estimate.get("build_time", "UNKNOWN; B7 measurement pending")}.')
    if free < need:
        raise Refusal('not enough free disk; free space or choose another --work directory')
    ndk = existing_ndk(cache, ndk_spec(release))
    existing_dxc(release, root)
    profile = db.load_profile(inside(root, profile_of(release, target)))
    # Check every supported game's plan without pretending either disc was identified.
    with environment({'BUILDER_PYTHON': str(inside(root, release['python'])) if release.get('python') else sys.executable,
                      'BUILDER_NDK_ROOT': str(ndk), 'BUILDER_PACKAGE_ROOT': str(root)}):
        failures = []
        for game in profile['games']:
            ctx = db.Context(profile, work, root, work); ctx.game = game
            ctx.discs = {role: DiscNamedAtBuild(role) for role in ('play', 'install')}  # names only; no stage runs, no disc is read
            db.plan_paths(profile, ctx)
            if 'output' in profile:
                db.plan_output(profile, ctx)
            else:
                db.plan_bundle(profile, ctx)
            failures.extend(db.check_prerequisites(profile, ctx))
    if failures:
        raise Refusal('; '.join(dict.fromkeys(failures)) + '; obtain the complete payload or fix the profile; no stage ran')
    print('Payload, compiler and stage prerequisites verified. Discs still require identification at build time.')
    return ndk


def default_release():
    # The package's inventory lies at the package root, which the launcher names; without a launcher, beside this file.
    root = os.environ.get('BUILDER_PACKAGE_ROOT')
    return Path(root) / 'release.json' if root else Path(__file__).with_name('release.json')


def default_work(root):
    # Inside the package's private folder: the stage tools accept a work folder nowhere else below a package.
    return root / 'local-analysis' / 'builder' / 'work'


def ios_only_text(release, targets):
    ndk, need = release['ndk'], release['estimates']['min_free_bytes']
    android = [t for t in targets if t not in APP_TARGETS]
    targets = [t for t in targets if t in APP_TARGETS]
    which = ' or '.join('--' + t for t in targets)
    out = default_work(Path()) / 'out'
    return f'''This program makes the FivePocket app for iPhone and iPad ({', '.join(targets)}) from your own two disc images,
on a Mac with Xcode (DEVELOPER_DIR, or Xcode in /Applications). Open Terminal in this folder and run, in this order:

  1. ./builder fetch --accept-ndk-license
       Once. Fetches the compiler the shader stages use (Android NDK, {ndk["size"] / 1e6:.0f} MB from {ndk["url"]}).
       Read its terms first ({ndk["license_url"]}); the switch says you accept them.
  2. ./builder build {which} --play-disc PLAY-DISC.iso --install-disc INSTALL-DISC.iso
       Needs {need / 2**30:.0f} GiB of free space. Takes about an hour; the second app reuses most of the first one's work.
       Writes an unsigned .ipa to {out} in this folder. The game's data is not in it: the app imports it from your discs.
       Your app's bundle identifier is made once and printed at the end: ALWAYS SIGN WITH THAT IDENTIFIER
       (another one makes a new app with empty storage). --bundle-id ID chooses your own for one build.
  3. ./builder sign --ipa UNSIGNED.ipa --identity "Apple Development: you@example.com" --profile-file YOUR.mobileprovision --out SIGNED.ipa
       Signs it with your own identity and profile (or sign and install it with your usual tools).

{android_text(android, out)}A build that was interrupted goes on where it stopped when step 2 is run again.
Apart from short-lived files in the system's temporary folder, nothing is written outside this folder and the
compiler's cache folder; nothing is installed.'''


def android_text(android, out):
    if not android:
        return ANDROID_ELSEWHERE + '\n\n'
    names = {'thor': 'the Thor Edition app (AYN Thor, two screens)', 'fold': 'the single-screen app (Fold and other phones)'}
    lines = ''.join(f'  ./builder build --{t} --play-disc PLAY-DISC.iso --install-disc INSTALL-DISC.iso\n'
                    f'       The code bundle for {names.get(t, t)}, written to {out}.\n' for t in android)
    return f'''
It also makes the code bundle for the Android apps, which import it from the phone's storage:
{lines}  ./builder verify BUNDLE
       Checks a bundle and prints its seal: the app shows the same seal when it imports the bundle.

'''


def ios_text(targets):
    if not targets:
        return ''
    which = ' or '.join('--' + t for t in targets)
    return f'''
On a Mac with Xcode (DEVELOPER_DIR, or Xcode in /Applications) it also makes the iOS app ({', '.join(targets)}):
  builder build {which} --bundle-id com.yourname.fivepocket --play-disc PLAY-DISC.iso --install-disc INSTALL-DISC.iso
       Writes an unsigned .ipa to the same folder. The game's data is not in it: the app imports it from your discs.
  builder sign --ipa UNSIGNED.ipa --identity "Apple Development: you@example.com" --profile-file YOUR.mobileprovision --out SIGNED.ipa
       Signs it with your own identity and profile (or sign and install it with your usual tools).

'''


def how_to(release_path):
    # What a player sees when the program is started without a word (a double click). Numbers come from the
    # package's own inventory when it can be read; nothing is checked or started here.
    try:
        release = read_json(release_path)
        ndk, need = release['ndk'], release['estimates']['min_free_bytes']
        compiler = f'{ndk["size"] / 1e6:.0f} MB from {ndk["url"]}'
        space = f'{need / 2**30:.0f} GiB of free space'
        terms = ndk['license_url']
        dxc = release.get('dxc')
        targets = sorted(release.get('profiles', {}))
        if release['profile'] in release.get('profiles', {}).values():
            return ios_only_text(release, targets)
        shaders = (f'\n       And the shader compiler (DirectX Shader Compiler, {dxc["size"] / 1e6:.0f} MB from {dxc["url"]});'
                   f'\n       its terms: {dxc["license_url"]}') if dxc else ''
    except (OSError, ValueError, KeyError, TypeError):
        compiler, space, terms, dxc, shaders, targets = 'from its publisher', 'free space as the check step reports', 'named by the fetch step', None, '', []
    switches = '--accept-ndk-license --accept-dxc-license' if dxc else '--accept-ndk-license'
    accept = 'each switch says you accept the one' if dxc else 'the switch says you accept them'
    out = default_work(Path()) / 'out'
    return f'''This program makes the game's code bundle for the phone app from your own two disc images.
Open a command prompt in this folder and run, in this order:

  1. builder fetch {switches}
       Once. Fetches the compiler (Android NDK, {compiler}).
       Read its terms first ({terms}); {accept}.{shaders}
  2. builder build --play-disc PLAY-DISC.iso --install-disc INSTALL-DISC.iso
       Needs {space}. Takes from half an hour to a few hours.
       The bundle is written to {out} in this folder.
  3. builder verify BUNDLE
       Checks a bundle and prints its seal: the app shows the same seal when it imports the bundle.

{ios_text(targets)}A build that was interrupted goes on where it stopped when step 2 is run again.
Apart from short-lived files in the system's temporary folder, nothing is written outside this folder and the
compiler's cache folder; nothing is installed.'''


def sign_ipa(ipa: Path, identity: str, profile: Path | None, entitlements: Path | None, out: Path) -> int:
    """The player's own signature on a builder .ipa: unpacked into a private temporary folder, signed inside-out and verified
    (ios_sign.py), written again (ipa_writer.py). The unsigned .ipa is left as it is."""
    import ios_sign, ipa_writer
    if out.exists() or out.suffix != '.ipa':
        raise Refusal(f'{out} exists or is not an .ipa name')
    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(ipa) as z:
            for name in z.namelist():
                inside(Path(tmp), name)                     # the same refusals as a package path: no .., no absolute names
            z.extractall(tmp)
            for info in z.infolist():
                if (info.external_attr >> 16) & 0o111:
                    (Path(tmp) / info.filename).chmod(0o755)
        apps = sorted((Path(tmp) / 'Payload').glob('*.app'))
        if len(apps) != 1:
            raise Refusal('the .ipa holds no single Payload/<name>.app')
        try:
            done = ios_sign.sign(apps[0], identity, profile, entitlements)
        except ios_sign.Refused as error:
            raise Refusal(str(error))
        except subprocess.CalledProcessError as error:
            print(f'REFUSED: codesign failed: {(error.stderr or error.stdout or "").strip()[-600:]}')
            return 1
        ipa_writer.write_ipa(apps[0], out)
        print(f'{len(done)} signed and verified (codesign --verify --deep --strict)')
    print(f'Signed: {out}')
    return 0


def main(argv=None):
    if (sys.argv[1:] if argv is None else argv) == []:
        print(how_to(default_release()))
        return 0
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--release', type=Path, default=default_release())
    ap.add_argument('--cache', type=Path, default=Path(os.environ.get('LOCALAPPDATA', str(Path.home() / '.cache'))) / 'own-disc-builder')
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('check'); c.add_argument('--work', type=Path)
    f = sub.add_parser('fetch'); f.add_argument('--accept-ndk-license', action='store_true'); f.add_argument('--archive', type=Path)
    f.add_argument('--accept-dxc-license', action='store_true'); f.add_argument('--dxc-archive', type=Path)
    b = sub.add_parser('build')
    b.add_argument('--work', type=Path); b.add_argument('--play-disc', type=Path, required=True)
    b.add_argument('--install-disc', type=Path); b.add_argument('--out', type=Path); b.add_argument('--allow-heavy', action='store_true')
    b.add_argument('--report', type=Path); b.add_argument('--stop-after')
    target = b.add_mutually_exclusive_group()
    target.add_argument('--ios', dest='target', action='store_const', const='ios')
    target.add_argument('--duo', dest='target', action='store_const', const='duo')
    target.add_argument('--thor', dest='target', action='store_const', const='thor')
    target.add_argument('--fold', dest='target', action='store_const', const='fold')
    b.add_argument('--bundle-id')
    v = sub.add_parser('verify'); v.add_argument('bundle', type=Path); v.add_argument('--executable-sha256')
    s = sub.add_parser('sign'); s.add_argument('--ipa', type=Path, required=True); s.add_argument('--identity', required=True)
    s.add_argument('--profile-file', type=Path); s.add_argument('--entitlements', type=Path); s.add_argument('--out', type=Path, required=True)
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'verify':
            return db.main(['verify', str(a.bundle)] + (['--executable-sha256', a.executable_sha256] if a.executable_sha256 else []))
        if a.cmd == 'sign':
            return sign_ipa(a.ipa, a.identity, a.profile_file, a.entitlements, a.out)
        release, root = load_release(a.release)
        if a.cmd == 'fetch':
            check_paths(a.cache, a.cache)
            fetch(release, a.cache.resolve(), a.accept_ndk_license, a.archive)
            fetch_dxc(release, root, a.cache.resolve(), a.accept_dxc_license, a.dxc_archive)
        else:
            work = a.work or default_work(root)
            target = getattr(a, 'target', None)
            profiles = release.get('profiles', {})
            if target not in APP_TARGETS + (None,) and target not in profiles and any(t in APP_TARGETS for t in profiles):
                print(ANDROID_ELSEWHERE)               # an iOS release without the Android packs (PUBLISH-DECISIONS 23)
                return 2
            if target is None:                          # a release whose default profile is one of its iOS targets (the Mac release)
                target = next((t for t, rel in sorted(release.get('profiles', {}).items()) if rel == release['profile']), None)
            # check plans the target with a stand-in identifier (no settings are written before a build)
            bundle_id = ((bundle_id_for(target, work.resolve(), a.bundle_id) if a.cmd == 'build' else f'com.fivepocket.check.{target}')
                         if target in APP_TARGETS else None)
            with environment({'BUILDER_BUNDLE_ID': bundle_id} if bundle_id else {}):
                ndk = check(release, root, work.resolve(), a.cache.resolve(), target)
            if a.cmd == 'build':
                # A player who starts a build has asked for the long stages too: the engine's switch for them is
                # always given (--allow-heavy is still taken on the command line, and changes nothing).
                command = ['build', '--profile', str(inside(root, profile_of(release, target))), '--work', str(work),
                           '--play-disc', str(a.play_disc), '--repo', str(root), '--data-root', str(work), '--allow-heavy']
                for name in ('install_disc', 'out', 'report', 'stop_after'):
                    if getattr(a, name): command += ['--' + name.replace('_', '-'), str(getattr(a, name))]
                with environment(dict({'BUILDER_PYTHON': str(inside(root, release['python'])) if release.get('python') else sys.executable,
                                       'BUILDER_NDK_ROOT': str(ndk), 'BUILDER_PACKAGE_ROOT': str(root)},
                                      **({'BUILDER_BUNDLE_ID': bundle_id} if bundle_id else {}))):
                    code = db.main(command)
                if bundle_id:
                    say_bundle_id(bundle_id, target)
                return code
        return 0
    except (Refusal, db.BuilderError, OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f'REFUSED: {error}. No automatic retry. Correct the input or use an intact release, then run check again.')
        return getattr(error, 'code', 2)


if __name__ == '__main__':
    raise SystemExit(main())
