#!/usr/bin/env python3
"""Sign an iOS app folder with the player's own identity, nested code first ("inside-out"), then check it.

    ios_sign.py --app DIR.app --identity ID [--profile FILE.mobileprovision] [--entitlements FILE.plist] [--xcode DIR]

Every Mach-O below the app that is not its main executable (dylibs, frameworks, any nested .appex) is signed first,
deepest path first; a framework is signed as its folder. Then the app is signed with the entitlements (when given) and,
with --profile, the provisioning profile is copied in as embedded.mobileprovision first. Last,
`codesign --verify --deep --strict` must pass. codesign --deep is never used for signing: it would sign nested code
without its own entitlements and hide what was signed. Signing again over an earlier signature is allowed (--force),
so an app can be re-signed after a profile renewal without rebuilding it.

ID is a codesign identity: a certificate's SHA-1 or name in the player's keychain, or "-" for an ad-hoc signature
(which does not install on a device, but checks the layout). Nothing here holds or prints a key: codesign reads the
keychain. codesign comes from --xcode, else DEVELOPER_DIR, else /usr/bin/codesign. Exit 0: signed and verified;
1: codesign refused (its message is printed); 2: refused before signing.
"""
from __future__ import annotations

import argparse
import os
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

MACHO_MAGICS = {b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'}


class Refused(Exception):
    pass


def is_macho(path: Path) -> bool:
    with path.open('rb') as f:
        return f.read(4) in MACHO_MAGICS


def main_executable(app: Path) -> Path:
    info = app / 'Info.plist'
    if not info.is_file():
        raise Refused(f'no Info.plist in {app}')
    with info.open('rb') as f:
        name = plistlib.load(f).get('CFBundleExecutable')
    if not isinstance(name, str) or not name or '/' in name or not (app / name).is_file():
        raise Refused('Info.plist names no executable that is in the app')
    return app / name


def nested_code(app: Path) -> list[Path]:
    """what is signed before the app: bundles (.framework, .appex) as folders, other Mach-O files one by one; deepest
    first, so a bundle's own contents are signed before the bundle"""
    main = main_executable(app)
    items: list[Path] = []
    bundles = [p for p in app.rglob('*') if p.is_dir() and not p.is_symlink() and p.suffix in ('.framework', '.appex')]
    for p in app.rglob('*'):
        if p.is_symlink():
            raise Refused(f'a link in the app: {p}')
        if not p.is_file() or p == main or p.name == 'Info.plist' or '_CodeSignature' in p.parts:
            continue
        owner = next((b for b in bundles if p.is_relative_to(b)), None)
        if owner is not None and p == owner / owner.stem:      # a bundle's own executable: signed with its bundle
            continue
        if is_macho(p):
            items.append(p)
    items += bundles
    return sorted(set(items), key=lambda p: (-len(p.relative_to(app).parts), p.as_posix()))


def codesign_tool(xcode: str | None) -> str:
    base = xcode or os.environ.get('DEVELOPER_DIR')
    if base:
        tool = Path(base) / 'usr/bin/codesign'
        if tool.is_file():
            return str(tool)
    return '/usr/bin/codesign'


def sign(app: Path, identity: str, profile: Path | None = None, entitlements: Path | None = None, xcode: str | None = None,
         run=subprocess.run) -> list[str]:
    """signs and verifies; returns the signed paths (relative to the app, the app itself last as '.')"""
    if app.suffix != '.app' or not app.is_dir():
        raise Refused(f'not an app folder: {app}')
    if not identity:
        raise Refused('no signing identity given')
    for given, what in ((profile, 'profile'), (entitlements, 'entitlements')):
        if given is not None and not given.is_file():
            raise Refused(f'the {what} file is not there: {given}')
    tool = codesign_tool(xcode)
    order = nested_code(app)
    if profile is not None:
        shutil.copyfile(profile, app / 'embedded.mobileprovision')
    done = []
    for item in order + [app]:
        argv = [tool, '--force', '--sign', identity, '--timestamp=none', '--generate-entitlement-der']
        if item == app and entitlements is not None:
            argv += ['--entitlements', str(entitlements)]
        r = run(argv + [str(item)], capture_output=True, text=True)
        if r.returncode:
            raise subprocess.CalledProcessError(r.returncode, argv, r.stdout, r.stderr)
        done.append('.' if item == app else item.relative_to(app).as_posix())
    r = run([tool, '--verify', '--deep', '--strict', str(app)], capture_output=True, text=True)
    if r.returncode:
        raise subprocess.CalledProcessError(r.returncode, 'codesign --verify', r.stdout, r.stderr)
    return done


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--app', type=Path, required=True)
    ap.add_argument('--identity', required=True)
    ap.add_argument('--profile', type=Path)
    ap.add_argument('--entitlements', type=Path)
    ap.add_argument('--xcode')
    a = ap.parse_args(argv)
    try:
        done = sign(a.app, a.identity, a.profile, a.entitlements, a.xcode)
    except Refused as error:
        print('SIGN_REFUSED:', error, file=sys.stderr)
        return 2
    except subprocess.CalledProcessError as error:
        print('SIGN_FAILED:', (error.stderr or '').strip()[-2000:], file=sys.stderr)
        return 1
    print(f'SIGNED {a.app.name}: {len(done)} items, inside-out; codesign --verify --deep --strict passed', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
