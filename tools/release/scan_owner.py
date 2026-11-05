#!/usr/bin/env python3
"""Check that no personal or machine detail of a developer is published.

    scan_owner.py [ROOT] [--extra FILE] [--exact-allow FILE]

Fails on, in any text file:
- a home folder (/Users/NAME, /home/NAME, C:\\Users\\NAME) or a named external disk (/Volumes/NAME), except the
  placeholders in PLACEHOLDER_USERS and one-letter names (test fixtures use /Users/a);
- a private network address (10.x, 172.16-31.x, 192.168.x) or an ssh user@host;
- an e-mail address not listed in owner-scan-allow.txt (third-party authors named in licence notices) and not
  at an example domain;
- an Apple Team ID setting, a provisioning profile id, an adb device serial or an ANDROID_SERIAL value;
- with --extra FILE: any literal line of FILE (case-insensitive). Keep that file outside the repository: it holds
  the details being searched for (machine and host names, device serials, team ids).
Inside a third-party folder (a path part named thirdparty or third_party: upstream code as its authors publish it),
authors' e-mail addresses and the home folders and private network addresses of test fixtures are counted in a
NOTE, not as findings; every other check, and the --extra literals that identify our own people, still apply.
--exact-allow (default owner-exact-allow.txt): lines 'SHA256 PATH REASON'. A file whose path inside ROOT is PATH (or
ends with /PATH) AND whose bytes have that SHA-256 is reported as ALLOWED with the reason instead of its findings
(publish decision 19: the frozen compile manifests of the reference build). The same detail in any other file, or
in that file with one byte changed, still fails.

Exit 0: PASS. 1: FAIL (file:line and the kind of detail, never the matched text for --extra). 2: ROOT missing.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PLACEHOLDER_USERS = {'you', 'yourname', 'somebody', 'someone', 'user', 'username', 'runner', 'name', 'me', 'example', 'shared', '<you>',
                     '$user', '${user}', '%username%', 'builder', 'ci'}
CHECKS = [
    ('home folder', re.compile(r'(?:/Users/|/home/|[A-Za-z]:\\\\?Users\\\\?)([A-Za-z0-9._<>${}%-]+)')),
    ('external disk', re.compile(r'/Volumes/([^/\s"\'`]+)')),
    ('private network address', re.compile(r'(?<![\d.])(?:10\.\d{1,3}|192\.168|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}(?![\d.])')),
    ('ssh user@host', re.compile(r'\bssh\s+(?:-\S+\s+)*[A-Za-z0-9._-]+@[A-Za-z0-9._-]+')),
    ('Apple Team ID', re.compile(r'(?i)(?:DEVELOPMENT_TEAM|TeamIdentifier|team[-_ ]?id)["\'\s:=>]+(?:<string>)?[A-Z0-9]{10}\b')),
    ('provisioning profile id', re.compile(r'(?i)PROVISIONING_PROFILE(?:_SPECIFIER)?\s*[:=]\s*\S+')),
    ('adb device serial', re.compile(r'\badb\s+-s\s+(?!\$|<|"\$|127\.0\.0\.1|localhost|emulator)[A-Za-z0-9:._-]{6,}')),
    ('ANDROID_SERIAL value', re.compile(r'ANDROID_SERIAL\s*=\s*["\']?(?!\$|<)[A-Za-z0-9:._-]{6,}')),
]
SELF = {'tools/release/scan_owner.py', 'tools/release/owner-scan-allow.txt'}  # they describe what they look for
EXAMPLE_DOMAINS = ('@example.invalid', '@example.com', '@example.org', '@example.net', '@users.noreply.github.com')
EMAIL = re.compile(r'[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}')
THIRD_PARTY = {'thirdparty', 'third_party'}
UPSTREAM_GENERIC = {'home folder', 'private network address'}  # what upstream fixtures and notices hold


def files(root: Path):
    if (root / '.git').exists():
        out = subprocess.run(['git', '-C', str(root), 'ls-files', '-z'], capture_output=True, check=True)
        names = [n for n in out.stdout.decode().split('\0') if n]
        if names:
            return names
    return sorted(str(p.relative_to(root)) for p in root.rglob('*') if p.is_file() and '.git' not in p.parts)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('root', nargs='?', default=str(HERE.parents[1]))
    ap.add_argument('--extra')
    ap.add_argument('--exact-allow', default=str(HERE / 'owner-exact-allow.txt'))
    args = ap.parse_args(argv)
    root = Path(args.root)
    if not root.is_dir():
        print(f'scan_owner: no such folder: {root}', file=sys.stderr)
        return 2
    allowed_emails = set()
    allow = HERE / 'owner-scan-allow.txt'
    if allow.exists():
        allowed_emails = {l.split()[0].lower() for l in allow.read_text().splitlines() if l.strip() and not l.startswith('#')}
    extra = []
    if args.extra:
        extra = [l.strip().lower() for l in Path(args.extra).read_text().splitlines() if l.strip() and not l.startswith('#')]
    exact = {}
    if Path(args.exact_allow).exists():
        for line in Path(args.exact_allow).read_text().splitlines():
            if line.strip() and not line.startswith('#'):
                digest, where, reason = line.split(None, 2)
                exact[(digest, where)] = reason
    findings, waived, allowed = [], 0, []
    for rel in files(root):
        path = root / rel
        if path.is_symlink() or not path.is_file() or Path(rel).as_posix() in SELF:
            continue
        data = path.read_bytes()
        if exact:
            digest, posix = hashlib.sha256(data).hexdigest(), Path(rel).as_posix()
            reason = next((r for (d, w), r in exact.items() if d == digest and (posix == w or posix.endswith('/' + w))), None)
            if reason:
                allowed.append(f'ALLOWED {rel}: exact file, {reason}')
                continue
        if b'\0' in data[:8192]:
            low = data.lower()
            for i, word in enumerate(extra):
                if word.encode() in low:
                    findings.append(f'PRIVATE {rel}: binary contains private literal #{i + 1}')
            continue
        text = data.decode('utf-8', errors='replace')
        lower = text.lower()
        upstream = bool(THIRD_PARTY & set(Path(rel).parts))
        for what, rx in CHECKS:
            for m in rx.finditer(text):
                if m.groups() and (len(m.group(1)) == 1 or m.group(1).lower() in PLACEHOLDER_USERS):
                    continue
                if upstream and what in UPSTREAM_GENERIC:
                    waived += 1
                    continue
                findings.append(f'OWNER {rel}:{text.count(chr(10), 0, m.start()) + 1}: {what}')
        for m in EMAIL.finditer(text):
            if upstream:
                waived += 1
            elif m.group(0).lower() not in allowed_emails and not m.group(0).lower().endswith(EXAMPLE_DOMAINS):
                findings.append(f'EMAIL {rel}:{text.count(chr(10), 0, m.start()) + 1}: {m.group(0)}')
        for i, word in enumerate(extra):
            at = lower.find(word)
            if at >= 0:
                findings.append(f'PRIVATE {rel}:{text.count(chr(10), 0, at) + 1}: private literal #{i + 1}')
    for f in allowed + findings:
        print(f)
    if waived:
        print(f'NOTE {waived} author e-mails or fixture paths/addresses in third-party folders, not counted')
    print(f'OWNER {"PASS" if not findings else "FAIL"} findings={len(findings)}')
    return 0 if not findings else 1


if __name__ == '__main__':
    sys.exit(main())
