#!/usr/bin/env python3
"""Check for keys and secrets: console keys, signing keys, tokens and files that must never be published.

    scan_secrets.py [ROOT] [--key-file FILE ...] [--download] [--allow FILE]

Fails on:
- a file type that holds signing keys or builds (.jks .keystore .p12 .pfx .pem .key .mobileprovision .p8 .cer
  .apk .aab .ipa .so .dylib .xex .iso .rpf .bundle);
- a private key block, an access token (GitHub, Google, AWS, Slack) or a password/secret assigned a literal value;
- a known console key: every 16-byte value written as a C byte array or a 32-digit hex string, and every 16-byte
  window of binary files under 32 MB, is hashed and compared with forbidden-key-sha256.txt (only digests are kept);
- a byte array of exactly 16 or 32 bytes whose name says key/aes/iv, which is how an unknown key would look.
--key-file: extra private key files (kept outside the repository) whose 16-byte windows are also forbidden; never
commit such a file. With a key file, binaries over 32 MB are searched with windows.c instead of being skipped.
--filetype-allow (default filetype-allow.txt): lines 'SHA256 PATH REASON'. In a download a .dylib passes only when its
whole-file SHA-256 and its path inside the download (PATH, or a path ending with /PATH) are listed there (publish decision
21: the Mac builder's DXC compiler library and fmt); every other .dylib, or one of these with one byte changed, fails.
--download: ROOT is an unpacked download, which may hold its app (.apk) and native libraries; those types are then
allowed, and package_release.py unpacks the app and scans what is inside it too.
--allow (default upstream-key-allow.txt): whole-file SHA-256 digests of upstream files that carry a key as their
upstream publishes them (the builder's XenonRecomp source and binaries, publish decision 1). Key findings in exactly
those bytes are reported as ALLOWED with the listed reason; one changed byte and they fail again. Our own files
never contain a key, and token, password and file-type checks are never waived.

Exit 0: PASS. 1: FAIL (each finding is printed with file and line). 2: ROOT missing.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BANNED_SUFFIXES = {'.jks', '.keystore', '.p12', '.pfx', '.pem', '.key', '.mobileprovision', '.p8', '.cer',
                   '.apk', '.aab', '.ipa', '.so', '.dylib', '.xex', '.iso', '.rpf', '.bundle'}
PATTERNS = [
    ('private key block', re.compile(r'-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----')),
    ('GitHub token', re.compile(r'\b(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{30,}|\bgithub_pat_[A-Za-z0-9_]{40,}')),
    ('Google API key', re.compile(r'\bAIza[0-9A-Za-z_\-]{35}\b')),
    ('AWS access key', re.compile(r'\bAKIA[0-9A-Z]{16}\b')),
    ('Slack token', re.compile(r'\bxox[abpr]-[0-9A-Za-z-]{10,}')),
    ('literal password', re.compile(r'(?i)\b(?:store|key)?pass(?:word)?\s*[:=]\s*["\'][^"\'\s$%{]{4,}["\']')),
    ('literal secret', re.compile(r'(?i)\b(?:secret|api_?key|token)\s*[:=]\s*["\'][A-Za-z0-9_\-]{16,}["\']')),
]
ARRAY = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*=\s*\{([^{}]{40,2000})\}', re.S)
HEXBYTE = re.compile(r'0x([0-9A-Fa-f]{1,2})\b')
HEXSTR = re.compile(r'(?<![0-9A-Fa-f])([0-9A-Fa-f]{32}|[0-9A-Fa-f]{64})(?![0-9A-Fa-f])')
KEYNAME = re.compile(r'(?i)key|aes|(?:^|_)iv(?:$|_)')
BINARY_LIMIT = 32 << 20
SELF = {'tools/release/scan_secrets.py', 'tools/release/forbidden-key-sha256.txt'}  # they name what they look for


def load_allow(path: Path):
    """{whole-file sha256: 'path as shipped: reason'} from lines 'DIGEST PATH REASON...'."""
    allow = {}
    for line in path.read_text().splitlines():
        if line.strip() and not line.startswith('#'):
            digest, shipped, reason = line.split(None, 2)
            allow[digest] = f'{shipped}: {reason}'
    return allow


def load_digests(extra_files):
    digests = {}
    for line in (HERE / 'forbidden-key-sha256.txt').read_text().splitlines():
        if line and not line.startswith('#'):
            d, _, what = line.partition(' ')
            digests[d] = what
    for f in extra_files:
        data = Path(f).read_bytes()
        for i in range(0, max(len(data) - 15, 0)):
            digests.setdefault(hashlib.sha256(data[i:i + 16]).hexdigest(), f'private key file {Path(f).name}')
    return digests


def files(root: Path):
    if (root / '.git').exists():
        out = subprocess.run(['git', '-C', str(root), 'ls-files', '-z'], capture_output=True, check=True)
        names = [n for n in out.stdout.decode().split('\0') if n]
        if names:
            return names
    return sorted(str(p.relative_to(root)) for p in root.rglob('*') if p.is_file() and '.git' not in p.parts)


def windows16(data: bytes):
    for i in range(0, len(data) - 15):
        yield hashlib.sha256(data[i:i + 16]).hexdigest()


def scan_large(root: Path, large, key_files):
    import tempfile
    with tempfile.TemporaryDirectory(prefix='keys-') as tmp:
        exe = Path(tmp) / 'windows'
        subprocess.run([os.environ.get('CC', 'cc'), '-O2', '-o', str(exe), str(HERE / 'windows.c')], check=True)
        (Path(tmp) / 'index').write_text(''.join(f'{Path(k).resolve()}\n' for k in key_files))
        (Path(tmp) / 'stream').write_text(''.join(f'{(root / r).resolve()}\n' for r in large))
        out = subprocess.run([str(exe), '16', '1', str(Path(tmp) / 'index'), str(Path(tmp) / 'stream')],
                             capture_output=True, text=True, check=True).stdout
    return [f'KEY {large[int(l.split()[1])]}: a private key file\'s bytes are inside this binary'
            for l in out.splitlines() if l.startswith('RUN')]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('root', nargs='?', default=str(HERE.parents[1]))
    ap.add_argument('--key-file', action='append', default=[])
    ap.add_argument('--download', action='store_true')
    ap.add_argument('--allow', default=str(HERE / 'upstream-key-allow.txt'))
    ap.add_argument('--filetype-allow', default=str(HERE / 'filetype-allow.txt'))
    args = ap.parse_args(argv)
    root = Path(args.root)
    if not root.is_dir():
        print(f'scan_secrets: no such folder: {root}', file=sys.stderr)
        return 2
    digests = load_digests(args.key_file)
    allow = load_allow(Path(args.allow))
    filetype_allow = {}
    if Path(args.filetype_allow).exists():
        for line in Path(args.filetype_allow).read_text().splitlines():
            if line.strip() and not line.startswith('#'):
                digest, where, reason = (line.split(None, 2) + ['', ''])[:3]
                filetype_allow[digest.lower()] = (where, reason)
    findings, large, allowed = [], [], []
    banned = BANNED_SUFFIXES - ({'.apk', '.so', '.dylib'} if args.download else set())
    for rel in files(root):
        path = root / rel
        if path.is_symlink() or not path.is_file():
            continue
        if path.suffix.lower() in banned:
            findings.append(f'FILETYPE {rel}: a file type that is never published')
            continue
        data = path.read_bytes()
        if args.download and path.suffix.lower() == '.dylib':
            listed = filetype_allow.get(hashlib.sha256(data).hexdigest())
            if not listed or not ('/' + str(rel).replace(os.sep, '/')).endswith('/' + listed[0].lstrip('/')):
                findings.append(f'FILETYPE {rel}: a .dylib whose exact bytes are not listed in the file-type allowance')
                continue
            allowed.append(f'ALLOWED {rel}: listed library, {listed[1]}')
        upstream = allow.get(hashlib.sha256(data).hexdigest())
        before = len(findings)
        is_text = b'\0' not in data[:8192]
        if not is_text:
            if upstream:
                allowed.append(f'ALLOWED {rel}: upstream key, {upstream}')
            elif len(data) > BINARY_LIMIT:
                large.append(rel)
            elif any(h in digests for h in windows16(data)):
                findings.append(f'KEY {rel}: a forbidden 16-byte key is inside this binary')
            continue
        text = data.decode('utf-8', errors='replace')
        if Path(rel).as_posix() not in SELF:
            for what, rx in PATTERNS:
                for m in rx.finditer(text):
                    line = text.count('\n', 0, m.start()) + 1
                    findings.append(f'SECRET {rel}:{line}: {what}')
        for m in ARRAY.finditer(text):
            values = HEXBYTE.findall(m.group(2))
            blob = bytes(int(v, 16) for v in values)
            line = text.count('\n', 0, m.start()) + 1
            if any(h in digests for h in windows16(blob)):
                findings.append(f'KEY {rel}:{line}: {m.group(1)} holds a forbidden key')
            elif len(values) in (16, 32) and KEYNAME.search(m.group(1)):
                findings.append(f'KEYLIKE {rel}:{line}: {m.group(1)} is a {len(values)}-byte array named like a key')
        if Path(rel).as_posix() not in SELF:
            for m in HEXSTR.finditer(text):
                blob = bytes.fromhex(m.group(1))
                if any(h in digests for h in windows16(blob)):
                    line = text.count('\n', 0, m.start()) + 1
                    findings.append(f'KEY {rel}:{line}: hex string holds a forbidden key')
        if upstream:
            keys = [f for f in findings[before:] if f.startswith(('KEY ', 'KEYLIKE '))]
            findings[before:] = [f for f in findings[before:] if f not in keys]
            allowed.append(f'ALLOWED {rel}: {len(keys)} upstream key finding(s), {upstream}')
    if large and args.key_file:
        findings += scan_large(root, large, args.key_file)
    elif large:
        print(f'NOTE {len(large)} binaries over 32 MB were not searched for keys (no --key-file): {", ".join(large[:5])}')
    for f in allowed + findings:
        print(f)
    print(f'SECRETS {"PASS" if not findings else "FAIL"} findings={len(findings)} known_keys={len(digests)} '
          f'allowed_upstream={len(allowed)}')
    return 0 if not findings else 1


if __name__ == '__main__':
    sys.exit(main())
