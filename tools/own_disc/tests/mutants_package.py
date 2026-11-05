#!/usr/bin/env python3
"""Mutant check for the portable builder: for each check of test_package.py (beside this file) one copy of the tools
with one rule changed; the check of that number must fail on it. First a control: all checks pass on an unchanged copy.
Exits 1 if the control fails, a mutant survives, no longer applies or does not run. The tools are not touched."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
COPIED = ('tools/own_disc', 'tools/fp_codegen', 'tools/iter')     # what test_package.py reads under PACKAGE_SRC
H = 'tools/own_disc/host_support.py'
B = 'tools/own_disc/builder.py'
D = 'tools/own_disc/disc_builder.py'
I = 'tools/iter/iterate.py'
R = 'tools/fp_codegen/recipe_build.py'
G = 'tools/fp_codegen/generate.py'
MUTANTS = [
 (41, B, "not isinstance(d, dict) or not re.fullmatch('[0-9a-f]{64}', str(d.get('sha256'))) or type(d.get('size')) is not int or d['size'] <= 0:", "not isinstance(d, dict) or type(d.get('size')) is not int or d['size'] <= 0:"),
 (41, B, "not isinstance(d, dict) or not re.fullmatch('[0-9a-f]{64}', str(d.get('sha256'))) or type(d.get('size')) is not int or d['size'] <= 0:", "not isinstance(d, dict) or not re.match('[0-9a-f]{63}', str(d.get('sha256'))) or type(d.get('size')) is not int or d['size'] <= 0:"),
 (41, B, "not isinstance(d, dict) or not re.fullmatch('[0-9a-f]{64}', str(d.get('sha256'))) or type(d.get('size')) is not int or d['size'] <= 0:", "not isinstance(d, dict) or not re.fullmatch('[0-9a-f]{64}', str(d.get('sha256'))) or type(d.get('size')) is not int:"),
 (41, B, "not isinstance(d, dict) or not re.fullmatch('[0-9a-f]{64}', str(d.get('sha256'))) or type(d.get('size')) is not int or d['size'] <= 0:", "not isinstance(d, dict) or not re.fullmatch('[0-9a-f]{64}', str(d.get('sha256'))) or int(d.get('size') or 0) <= 0:"),
 (41, B, "if not re.fullmatch(r'https://github\\.com/microsoft/DirectXShaderCompiler/releases/download/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+\\.zip'", "if not re.match(r'https://github\\.com/microsoft/DirectXShaderCompiler/releases/download/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+\\.zip'"),
 (41, B, "r'https://github\\.com/microsoft/DirectXShaderCompiler/releases/download/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+\\.zip'", "r'https?://github\\.com/microsoft/DirectXShaderCompiler/releases/download/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+\\.zip'"),
 (41, B, "r'https://github\\.com/microsoft/DirectXShaderCompiler/releases/download/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+\\.zip'", "r'https://github\\.com/[A-Za-z0-9._-]+/DirectXShaderCompiler/releases/download/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+\\.zip'"),
 (41, B, "r'https://github\\.com/microsoft/DirectXShaderCompiler/releases/download/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+\\.zip'", "r'https://github\\.com/microsoft/DirectXShaderCompiler/releases/(?:latest/)?download/(?:[A-Za-z0-9._-]+/)?[A-Za-z0-9._-]+\\.zip'"),
 (41, B, "    if not str(d.get('license_url', '')).startswith('https://github.com/microsoft/DirectXShaderCompiler/'):", "    if not str(d.get('license_url', '')).startswith('https://'):"),
 (41, B, "    if not str(d.get('license_url', '')).startswith('https://github.com/microsoft/DirectXShaderCompiler/'):", "    if 'license_url' in d and not str(d['license_url']).startswith('https://github.com/microsoft/DirectXShaderCompiler/'):"),
 (41, B, '    if not isinstance(files, list) or not files:', '    if not isinstance(files, list):'),
 (41, B, '    if not isinstance(files, list) or not files:', '    if files is not None and not files:'),
 (41, B, "not isinstance(rec, dict) or not re.fullmatch('[0-9a-f]{64}', str(rec.get('sha256'))) or type(rec.get('size')) is not int or rec['size'] <= 0:", "not isinstance(rec, dict) or type(rec.get('size')) is not int or rec['size'] <= 0:"),
 (41, B, "not isinstance(rec, dict) or not re.fullmatch('[0-9a-f]{64}', str(rec.get('sha256'))) or type(rec.get('size')) is not int or rec['size'] <= 0:", "not isinstance(rec, dict) or not re.fullmatch('[0-9a-f]{64}', str(rec.get('sha256'))) or type(rec.get('size')) is not int:"),
 (41, B, "        safe_name(rec.get('member')); place = str(safe_name(rec.get('path'))).casefold()", "        place = str(safe_name(rec.get('path'))).casefold()"),
 (41, B, "        safe_name(rec.get('member')); place = str(safe_name(rec.get('path'))).casefold()", "        safe_name(rec.get('member')); place = str(rec.get('path')).casefold()"),
 (41, B, "        safe_name(rec.get('member')); place = str(safe_name(rec.get('path'))).casefold()", "        safe_name(rec.get('member')); place = str(safe_name(rec.get('path')))"),
 (41, B, "        if place in places:\n            raise Refusal('shader compiler files share a place", "        if False:\n            raise Refusal('shader compiler files share a place"),
 (41, B, "    d = release.get('dxc')\n    if d is None:\n        return None", "    d = release.get('dxc', {})\n    if d is None:\n        return None"),
 (41, B, "    d = dxc_spec(release)\n    if d is None:\n        return\n    print(f'Shader compiler:", "    d = dxc_spec(release)\n    print(f'Shader compiler:"),
 (41, B, "    d = dxc_spec(release)\n    if d is None:\n        return\n    for rec in d['files']:", "    d = dxc_spec(release)\n    for rec in d['files']:"),
 (42, B, "    if not accepted:\n        raise Refusal('licence not accepted; read those terms, then rerun fetch with --accept-dxc-license if you agree')", "    if False:\n        raise Refusal('licence not accepted; read those terms, then rerun fetch with --accept-dxc-license if you agree')"),
 (42, B, "    if not accepted:\n        raise Refusal('licence not accepted; read those terms, then rerun fetch with --accept-dxc-license if you agree')", "    if not accepted and not archive:\n        raise Refusal('licence not accepted; read those terms, then rerun fetch with --accept-dxc-license if you agree')"),
 (42, B, "then rerun fetch with --accept-dxc-license if you agree')", "then rerun fetch with --accept-ndk-license if you agree')"),
 (42, B, "    if shutil.disk_usage(cache).free < d['size'] + sum(rec['size'] for rec in d['files']):", "    if shutil.disk_usage(cache).free < d['size']:"),
 (42, B, "    if shutil.disk_usage(cache).free < d['size'] + sum(rec['size'] for rec in d['files']):", '    if False:'),
 (42, B, '    print(f\'Its licence terms (Microsoft and LLVM), also inside the archive: {d["license_url"]}\')\n', ''),
 (42, B, '    print(f\'Shader compiler: {d["url"]}; {d["size"]} bytes; SHA-256 {d["sha256"][:12]}\')\n', ''),
 (43, B, "    if total != d['size'] or h.hexdigest() != d['sha256']:", "    if total != d['size']:"),
 (43, B, "    if total != d['size'] or h.hexdigest() != d['sha256']:", "    if h.hexdigest() != d['sha256'] and total != d['size']:"),
 (43, B, "            if total > d['size']:", '            if False:'),
 (43, B, "            if len(data) != rec['size'] or hashlib.sha256(data).hexdigest() != rec['sha256']:", "            if len(data) != rec['size']:"),
 (43, B, "            if info is None or info.file_size != rec['size']:", '            if info is None:'),
 (43, B, "            if info is None or info.file_size != rec['size']:", "            if info is not None and info.file_size != rec['size']:"),
 (43, B, "            taken[rec['path']] = data\n", "            taken[rec['path']] = data\n            if rec['path'] in missing:\n                early = inside(root, rec['path']); early.parent.mkdir(parents=True, exist_ok=True); early.write_bytes(data)\n"),
 (44, B, "names = {info.filename.replace('\\\\', '/'): info for info in z.infolist()}", 'names = {info.filename: info for info in z.infolist()}'),
 (44, B, "    if not missing:\n        print('Shader compiler already verified.'); return", "    if False:\n        print('Shader compiler already verified.'); return"),
 (44, B, "        if db.sha256_file(p) != rec['sha256']:\n            raise Refusal(f'shader compiler file changed:", "        if False:\n            raise Refusal(f'shader compiler file changed:"),
 (44, B, "        if not p.is_file():\n            raise Refusal('shader compiler missing;", "        if False:\n            raise Refusal('shader compiler missing;"),
 (44, B, "            taken[rec['path']] = data\n", "            taken[rec['path']] = data\n    for info in z.infolist() if False else []:\n        pass\n    (root / 'extra.bin').write_bytes(b'x')\n"),
 (45, B, '    for name in missing:                      # only now', '    for name in taken:                        # only now'),
 (45, B, '{len(missing)} of {len(taken)} files put below', '{len(taken)} of {len(taken)} files put below'),
 (45, B, "            if db.sha256_file(p) != rec['sha256']:\n                raise Refusal(f'another file lies where", "            if False:\n                raise Refusal(f'another file lies where"),
 (45, B, "        elif p.exists() or p.is_symlink():\n            raise Refusal(f'something that is no file", "        elif False:\n            raise Refusal(f'something that is no file"),
 (45, B, "            out = dest.open('xb')             # a file that appeared", "            out = dest.open('wb')             # a file that appeared"),
 (46, B, "urllib.request.build_opener(PublisherOnlyRedirect()).open)(d['url'], timeout=60)", "urllib.request.build_opener(PublisherOnlyRedirect()).open)(d['url'], timeout=600)"),
 (46, B, "urllib.request.build_opener(PublisherOnlyRedirect()).open)(d['url'], timeout=60)", "urllib.request.build_opener(PublisherOnlyRedirect()).open)(d['url'].replace('/microsoft/', '/mirror/'), timeout=60)"),
 (46, B, '        if not newurl.startswith(self.HOSTS):', '        if False:'),
 (46, B, "'https://objects.githubusercontent.com/', 'https://release-assets.githubusercontent.com/')", "'https://objects.githubusercontent.com', 'https://release-assets.githubusercontent.com/')"),
 (46, B, "'https://objects.githubusercontent.com/', 'https://release-assets.githubusercontent.com/')", "'https://objects.githubusercontent.com/')"),
 (46, B, "    HOSTS = ('https://github.com/microsoft/DirectXShaderCompiler/',", "    HOSTS = ('https://github.com/',"),
 (46, B, "    HOSTS = ('https://github.com/microsoft/DirectXShaderCompiler/', 'https://objects.githubusercontent.com/',", "    HOSTS = ('https://github.com/microsoft/DirectXShaderCompiler/', 'http://objects.githubusercontent.com/', 'https://objects.githubusercontent.com/',"),
 (47, B, '    existing_dxc(release, root)\n    profile = db.load_profile(', '    profile = db.load_profile('),
 (47, B, '            fetch_dxc(release, root, a.cache.resolve(), a.accept_dxc_license, a.dxc_archive)', '            fetch_dxc(release, root, a.cache.resolve(), a.accept_ndk_license, a.dxc_archive)'),
 (47, B, '            fetch_dxc(release, root, a.cache.resolve(), a.accept_dxc_license, a.dxc_archive)', "            fetch_dxc(release, a.dxc_archive.resolve().parent / 'package', a.cache.resolve(), a.accept_dxc_license, a.dxc_archive)"),
 (47, B, '            fetch_dxc(release, root, a.cache.resolve(), a.accept_dxc_license, a.dxc_archive)\n', ''),
 (47, B, "    f.add_argument('--accept-dxc-license', action='store_true');", "    f.add_argument('--accept-dxc-license', action='store_true', default=True);"),
 (47, B, "    switches = '--accept-ndk-license --accept-dxc-license' if dxc else '--accept-ndk-license'", "    switches = '--accept-ndk-license'"),
 (47, B, "    accept = 'each switch says you accept the one' if dxc else 'the switch says you accept them'", "    accept = 'the switch says you accept them'"),
 (47, B, 'f\'\\n       its terms: {dxc["license_url"]}\') if dxc else \'\'', 'f\'\\n       its terms: {dxc["url"]}\') if dxc else \'\''),
 (47, B, '{dxc["size"] / 1e6:.0f} MB from {dxc["url"]});\'', '{dxc["size"]:.0f} MB from {dxc["url"]});\''),
 (40, B, "    switches = '--accept-ndk-license --accept-dxc-license' if dxc else '--accept-ndk-license'", "    switches = '--accept-ndk-license --accept-dxc-license'"),
 (38, B, "    return root / 'local-analysis' / 'builder' / 'work'", "    return root / 'work'"),
 (38, B, "            work = a.work or default_work(root)", "            work = default_work(root)"),
 (38, B, "            work = a.work or default_work(root)", "            work = a.work or Path('work')"),
 (38, B, "    c = sub.add_parser('check'); c.add_argument('--work', type=Path)", "    c = sub.add_parser('check'); c.add_argument('--work', type=Path, required=True)"),
 (38, B, "    b.add_argument('--work', type=Path); b.add_argument('--play-disc', type=Path, required=True)", "    b.add_argument('--work', type=Path, required=True); b.add_argument('--play-disc', type=Path, required=True)"),
 (39, B, "'--repo', str(root), '--data-root', str(work), '--allow-heavy']", "'--repo', str(root), '--data-root', str(work)]"),
 (39, B, "b.add_argument('--out', type=Path); b.add_argument('--allow-heavy', action='store_true')", "b.add_argument('--out', type=Path)"),
 (40, B, "    if (sys.argv[1:] if argv is None else argv) == []:\n        print(how_to(default_release()))\n        return 0\n", ""),
 (40, B, "        print(how_to(default_release()))\n        return 0\n", "        print(how_to(Path('release.json')))\n        return 0\n"),
 (40, B, "        compiler = f'{ndk[\"size\"] / 1e6:.0f} MB from {ndk[\"url\"]}'", "        compiler = 'from its publisher'"),
 (40, B, "        space = f'{need / 2**30:.0f} GiB of free space'", "        space = f'{need} GiB of free space'"),
 (40, B, "    except (OSError, ValueError, KeyError, TypeError):\n        compiler, space, terms, dxc, shaders =", "    except (ValueError, KeyError, TypeError):\n        compiler, space, terms, dxc, shaders ="),
 (40, B, "    out = default_work(Path()) / 'out'", "    out = Path('out')"),
 (37, B, "            ctx.discs = {role: DiscNamedAtBuild(role) for role in ('play', 'install')}", "            ctx.discs = {'play': None, 'install': None}"),
 (37, B, "            ctx.discs = {role: DiscNamedAtBuild(role) for role in ('play', 'install')}", "            ctx.discs = {role: DiscNamedAtBuild(role) for role in ('play',)}"),
 (37, B, "            ctx.discs = {role: DiscNamedAtBuild(role) for role in ('play', 'install')}", "            ctx.discs = __import__('collections').defaultdict(lambda: DiscNamedAtBuild('any'))"),
 (36, B, "    return Path(root) / 'release.json' if root else Path(__file__).with_name('release.json')", "    return Path(__file__).with_name('release.json')"),
 (36, B, "    return Path(root) / 'release.json' if root else Path(__file__).with_name('release.json')", "    return Path(root or '.') / 'release.json'"),
 (36, B, "    ap.add_argument('--release', type=Path, default=default_release())", "    ap.add_argument('--release', type=Path, default=Path(__file__).with_name('release.json'))"),
 (1, I, "paths = host_support.dep_paths(depfile.read_text(encoding='utf-8'))", "paths = depfile.read_text().split(':', 1)[1].split()"),
 (2, H, "re.search(r':(?:\\s|$)', line)", "re.search(':', line)"),
 (3, H, "{'creationflags': 0x00004000} if amount else {}", "{}"),
 (4, H, "if sys.platform == 'darwin':", "if True:"),
 (5, I, "words[words.index(option) + 1] = str(value)", "words[words.index(option) + 1] = 'BROKEN'"),
 (6, R, "command, options = host_support.low_priority(host_support.python_command(self.tool, *argv))", "command, options = ['nice', '-n', '10', *host_support.python_command(self.tool, *argv)], {}"),
 (7, H, "if not path.is_relative_to(parent / 'local-analysis'):", "if False:"),
 (8, B, "    if not accepted:\n        raise Refusal('licence not accepted; read those terms, then rerun fetch with --accept-ndk-license if you agree')", "    if False:\n        raise Refusal('licence not accepted; read those terms, then rerun fetch with --accept-ndk-license if you agree')"),
 (9, B, "if total != n['size'] or h.hexdigest() != n['sha256']:", "if False:"),
 (10, B, "if not p.is_file() or db.sha256_file(p) != digest:", "if not p.is_file():"),
 (11, B, "raise Refusal('reserved Windows package path; obtain a corrected release')", "pass"),
 (12, B, "if key in seen:", "if False:"),
 (13, B, "if not p.is_file() or db.sha256_file(p) != rec['sha256']:", "if not p.is_file():"),
 (14, B, "if len(str(p)) > 80:", "if False:"),
 (15, D, "reason = receipt_verdict(read_receipt(ctx, sid), parts, outputs)", "reason = 'always rebuild'"),
 (16, D, "if len(match) != 1:", "if False:"),
 (17, D, "return '.exe' if sys.platform == 'win32' else ''", "return ''"),
 (18, 'tools/fp_codegen/leaf_locals.py', "newline='\\n'", "newline=None"),
 (19, D, "'host-support': hashlib.sha256(host_support.__loader__.get_data(host_support.__file__)).hexdigest()", "'host-support': '0' * 64"),
 (20, B, "not re.fullmatch('[0-9a-f]{64}', str(n.get('sha256')))", "False"),
 (21, D, "'key': r['key']", "'key': '0' * 64"),
 (22, G, "generator_revision = args.generator_revision", "generator_revision = None"),
 (23, D, "builder_sha256=hashlib.sha256(__loader__.get_data(__file__)).hexdigest()", "builder_sha256=sha256_file(Path(__file__))"),
 (24, B, "links[str(rel)] = str(linked)", "links[str(rel)] = str(linked) + '.missing'"),
 (25, R, "if name == 'host_support' and not q.exists():", "if False:"),
 (26, H, "return Path(os.environ.get('BUILDER_PACKAGE_ROOT', Path(source).resolve().parents[2])).resolve()", "return Path(source).resolve().parents[2]"),
 (27, 'tools/iter/generic_shader_pack.py', "ROOT = args.root.resolve()", "ROOT = Path(__file__).resolve().parents[2]"),
 (28, 'tools/fp_codegen/direct_calls.py', "encoding='utf-8', check=True", "encoding='utf-8', check=False"),
 (29, 'tools/own_disc/regen_check.py', "command, options = host_support.low_priority([str(a.generator), str(cfg), str(a.context)])", "command, options = ['nice', '-n', '10', str(a.generator), str(cfg), str(a.context)], {}"),
 (30, I, "all(local(p).exists() and digest_memo(local(p)) == h for p, h in meta['deps'].items())", "True"),
 (31, D, "key=lambda q: q.relative_to(directory).as_posix()", "key=lambda q: q"),
 (32, I, "int(os.environ.get('BUILDER_MAX_JOBS') or requested)", "int(requested)"),
 (33, I, "if not cfg.get('ppc_compile_commands') and any('/Documents/' in w for w in words):", "if False:"),
 (33, I, "{} if cfg.get('ppc_compile_commands') else {DOCUMENTS_ROOT: '.'}", "{}"),
 (32, I, "max(1, min(count, int(requested), ", "max(1, min(int(requested), "),
 (17, D, "                return host_support.ndk_host_tag()\n", "                return 'darwin-x86_64'\n"),
 (17, D, "                if not os.environ.get('BUILDER_HOST_TOOLS'):\n                    raise BuilderError('host tools missing: BUILDER_HOST_TOOLS names no folder', EXIT_PREREQ)\n", ""),
 (17, D, "                return os.environ['BUILDER_HOST_TOOLS']\n", "                return str(self.repo)\n"),
 (17, H, "NDK_HOST_TAGS.get('linux' if platform.startswith('linux') else platform)", "NDK_HOST_TAGS.get(platform)"),
 (17, H, "    if tag is None:\n        raise ValueError(f'no NDK host for {platform}')\n", "    if tag is None:\n        return 'linux-x86_64'\n"),
 (17, H, "'win32': 'windows-x86_64'", "'win32': 'windows'"),
 (34, H, "0x00004000: 10, 0x00000020: 0,", "0x00004000: 0, 0x00000020: 0,"),
 (34, H, "    if sys.platform == 'win32':\n        return nice_of_priority_class(_windows_priority_class())\n", ""),
 (34, H, "    if value not in WINDOWS_CLASS_NICE:\n        raise ValueError('unknown process priority class')\n    return WINDOWS_CLASS_NICE[value]", "    return WINDOWS_CLASS_NICE.get(value, 0)"),
 (35, H, "        os.lseek(fd, offset, os.SEEK_SET)\n", ""),
 (35, H, "        while left > 0:\n            chunk = os.read(fd, left)\n            if not chunk:\n                break\n            chunks.append(chunk)\n            left -= len(chunk)\n        return b''.join(chunks)", "        return os.read(fd, min(size, 64))"),
 (35, H, "    with _SEEK_LOCK:\n        os.lseek(fd, offset, os.SEEK_SET)", "    if True:\n        os.lseek(fd, offset, os.SEEK_SET)"),
 (35, H, "pread = os.pread if hasattr(os, 'pread') else pread_by_seek", "pread = pread_by_seek"),
]


def run(overlay, only=None):
    command = [sys.executable, '-B', '-m', 'unittest', 'discover', '-s', str(HERE), '-p', 'test_package.py', '-v']
    if only is not None:
        command += ['-k', f'test_{only:02}_']
    return subprocess.run(command, capture_output=True, text=True, env=dict(os.environ, PACKAGE_SRC=str(overlay), PYTHONDONTWRITEBYTECODE='1'))


def copy_tools(overlay):
    for folder in COPIED:
        shutil.copytree(REPO / folder, overlay / folder, ignore=shutil.ignore_patterns('__pycache__', 'tests'))


def main() -> int:
    killed = 0
    with tempfile.TemporaryDirectory() as tmp:
        overlay = Path(tmp).resolve() / 'control'            # resolved: a check compares the package root with this path
        copy_tools(overlay)
        if run(overlay).returncode != 0:
            print('CONTROL FAILED: the checks do not pass on an unchanged copy')
            return 1
        print('control: the checks pass on an unchanged copy of the tools', flush=True)
        for number, file, old, new in MUTANTS:
            text = (overlay / file).read_text(encoding='utf-8')
            if text.count(old) != 1:
                print(f'mutant {number:3d} DOES NOT APPLY ({text.count(old)} matches): {file}')
                continue
            (overlay / file).write_text(text.replace(old, new), encoding='utf-8', newline='\n')
            result = run(overlay, number)
            (overlay / file).write_text(text, encoding='utf-8', newline='\n')
            body = result.stdout + result.stderr
            # the named check must really have run: an import or syntax failure is no kill
            ran = 'Ran 1 test' in body and '_FailedTest' not in body and 'SyntaxError' not in body
            if not ran:
                print(f'mutant {number:3d} DID NOT RUN its check: {file}')
            elif result.returncode == 0:
                print(f'mutant {number:3d} SURVIVED: {file}: {old[:70]}')
            else:
                killed += 1
                print(f'mutant {number:3d} killed: {file}', flush=True)
    print(f'{killed} of {len(MUTANTS)} mutants killed')
    return 0 if killed == len(MUTANTS) else 1


if __name__ == '__main__':
    sys.exit(main())
