#!/usr/bin/env python3
"""Mutant check for disc_builder.py: each mutant is a copy of the tool with one guard removed or one value changed;
the unit tests (test_disc_builder.py beside this file) must fail on it.

  mutants.py [--jobs N] [--only TEXT]

First a control: the tests must pass on an unchanged copy placed where the mutants are placed. Then one line per
mutant with the first test that failed. Exits 1 if the control fails or any mutant survives or no longer applies to
the source. Two at a time by default. Mutated copies live in a temporary directory; the tool itself is not touched.
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'disc_builder.py'
HELPER = HERE.parent / 'host_support.py'
TESTS = HERE / 'test_disc_builder.py'
TIME_LIMIT = 90         # seconds for one run of the tests (they take about 10): a mutant that hangs counts as killed

MUTANTS = [
    # ---- identification and the disc readers
    ('any executable hash is accepted',
     "g['executable_sha256'] == digest and", "True and"),
    ('file names on a disc folder are matched with their exact case',
     "match = [c for c in cur.iterdir() if c.name.lower() == part.lower()]", "match = [c for c in cur.iterdir() if c.name == part]"),
    ('only trimmed disc images are recognised',
     "XDVDFS_PARTITION_OFFSETS = (0, 0x0FD90000, 0x02080000, 0x18300000)", "XDVDFS_PARTITION_OFFSETS = (0,)"),
    ('the XGD2 partition offset is one sector off',
     "XDVDFS_PARTITION_OFFSETS = (0, 0x0FD90000,", "XDVDFS_PARTITION_OFFSETS = (0, 0x0FD90800,"),
    ('the XGD3 partition offset is one sector off',
     "0x0FD90000, 0x02080000, 0x18300000)", "0x0FD90000, 0x02080800, 0x18300000)"),
    ('the XGD1 partition offset is one sector off',
     "0x02080000, 0x18300000)", "0x02080000, 0x18300800)"),
    ('the volume record need not end with the magic',
     "if head[:20] == XDVDFS_MAGIC and head[-20:] == XDVDFS_MAGIC:", "if head[:20] == XDVDFS_MAGIC:"),
    ('the disc directory tree is not followed to the right',
     "for child in (left, right):", "for child in (left,):"),
    ('the disc directory tree is not followed to the left',
     "for child in (left, right):", "for child in (right,):"),
    ('a directory on the disc image is taken for a file',
     "return None if is_dir else (start, fsize)", "return (start, fsize)"),
    ('a looping directory table is followed for ever',
     "if off in seen or off + 14 > len(data):", "if off + 14 > len(data):"),
    ('a link out of a directory table is followed',
     "if off in seen or off + 14 > len(data):", "if off in seen:"),
    ('a directory table behind the end of the image is read anyway',
     "if size > MAX_DIR_BYTES or self.partition + sector * SECTOR + size > self.image_size:", "if False:"),
    ('a file cut off by the end of the image is read anyway',
     "if self.partition + start * SECTOR + fsize > self.image_size:", "if False:"),
    # ---- the profile
    ('a profile of another schema is accepted',
     "if not isinstance(profile, dict) or profile.get('schema') != PROFILE_SCHEMA:", "if False:"),
    ('stage ids and adapters are not checked',
     "if not STAGE_ID.fullmatch(s.get('id', '')) or s.get('adapter') not in ADAPTERS:", "if False:"),
    ('stage outputs are not checked',
     "if not isinstance(outs, dict) or not outs or not all(OUTPUT_NAME.fullmatch(n) for n in outs):", "if False:"),
    ('a command stage without a command line is accepted',
     "if s['adapter'] == 'command' and not (isinstance(s.get('argv'), list) and s['argv']):", "if False:"),
    ('a copy stage with several outputs is accepted',
     "if s['adapter'] != 'command' and len(outs) != 1:", "if False:"),
    ('a disc stage without a path is accepted',
     "if s['adapter'] == 'disc-file' and not s.get('path'):", "if False:"),
    ('repeated and reserved stage ids are accepted',
     "if len(set(ids)) != len(ids) or 'bundle' in ids:", "if False:"),
    ('a profile without games is accepted',
     "    if not profile.get('games'):", "    if False:"),
    ('a game without a full executable hash is accepted',
     "if not HEX64.fullmatch(str(g.get('executable_sha256', ''))):", "if False:"),
    # ---- the plan: paths, variables, options
    ('an unknown variable expands to nothing',
     "            raise BuilderError(f'profile: unknown variable {m.group(0)}', EXIT_USAGE)", "            return ''"),
    ('what a stage reads is not resolved before the stages run',
     "            ctx.expand(text)                    # what a stage reads comes from earlier stages only", "            pass"),
    ('command lines are not resolved before the stages run',
     "        for text in list(stage.get('argv', [])) + list(stage.get('expect', {}).values()) + [stage.get('cwd') or '']:\n"
     "            ctx.expand(text)",
     "        for text in list(stage.get('argv', [])) + list(stage.get('expect', {}).values()) + [stage.get('cwd') or '']:\n"
     "            pass"),
    ('any stage may write outside the work directory',
     "if stage['adapter'] != 'command' or stage.get('external_outputs') is not True:", "if False:"),
    ('a copy stage may write outside the work directory when it says so',
     "if stage['adapter'] != 'command' or stage.get('external_outputs') is not True:", "if stage.get('external_outputs') is not True:"),
    ('an output may be the work directory itself',
     "            if p == ctx.work:\n                raise BuilderError(f'profile: output {name}", "            if False:\n                raise BuilderError(f'profile: output {name}"),
    ('clean targets and folders to make may lie outside the work directory',
     "if p == ctx.work or not p.is_relative_to(ctx.work):", "if False:"),
    ('a misspelt --stop-after is ignored',
     "if a.stop_after and a.stop_after not in stage_ids:", "if False:"),
    ('a --supply without a path is ignored',
     "        if '=' not in item:", "        if False:"),
    ('the bundle file name is not checked',
     "if not BUNDLE_FILE_NAME.fullmatch(name):", "if False:"),
    ('a profile without runtime_abi is accepted',
     "if not isinstance(spec.get('runtime_abi'), str) or not spec['runtime_abi']:", "if False:"),
    ('an empty or repeating component list is accepted',
     "if not names or len(set(names)) != len(names):", "if False:"),
    ('component names in the profile are not checked',
     "if not ENTRY_NAME.fullmatch(component) or component == MANIFEST_NAME or not known:",
     "if not known:"),
    ('component roles in the profile are not checked',
     "if not ENTRY_NAME.fullmatch(component) or component == MANIFEST_NAME or not known:",
     "if not ENTRY_NAME.fullmatch(component) or component == MANIFEST_NAME:"),
    ('a component of an unknown stage output is accepted',
     "if c.get('output') not in ctx.outputs.get(c.get('stage'), {}):", "if False:"),
    # ---- where the builder writes
    ('the git checkout rule is off everywhere',
     "if not path.is_relative_to(parent / PRIVATE_DIR):", "if False:"),
    ('the git checkout rule does not cover the work directory',
     "    check_private_path(work, 'work directory')", "    pass"),
    ('the git checkout rule does not cover the bundle directory',
     "    check_private_path(out_dir, 'bundle directory')", "    pass"),
    ('the git checkout rule does not cover the report file',
     "        check_private_path(a.report, 'report file')", "        pass"),
    ('the git checkout rule does not cover external stage outputs',
     "                check_private_path(p, f'output {name} of stage {sid}')", "                pass"),
    ('a folder with other files is taken as work directory',
     "if work.exists() and (not work.is_dir() or (any(work.iterdir()) and not (work / WORK_MARKER).is_file())):", "if False:"),
    ('the work directory is not marked as the builder\'s own',
     "        if not (work / WORK_MARKER).exists():", "        if False:"),
    ('a run stopped before its stages still writes last-run.json',
     "target = a.report or (work / 'last-run.json' if started else None)", "target = a.report or work / 'last-run.json'"),
    # ---- prerequisites
    ('a run continues despite missing prerequisites',
     "        if not a.dry_run:\n            print('Stopped before any stage ran.')", "        if False:\n            print('Stopped before any stage ran.')"),
    ('a missing tool or input file is not reported',
     "            failures.append(f'{label}: missing {p}')", "            pass"),
    ('files made by earlier stages are reported as missing prerequisites',
     "        if '${out:' in raw:\n            return", "        if False:\n            return"),
    ('the executable bit of a tool is not checked',
     "elif executable and not os.access(p, os.X_OK):", "elif False:"),
    ('a pinned tool hash is not compared',
     "elif pinned and p.is_file() and sha256_file(p) != pinned:", "elif False:"),
    ('the Python version is not compared',
     "if sys.version_info[:3] < version_tuple(pre.get('min', '3.9')):", "if False:"),
    ('a failing version command is accepted',
     "                if r.returncode != 0:\n                    failures.append(f'{label}: {Path(argv[0]).name} exited",
     "                if False:\n                    failures.append(f'{label}: {Path(argv[0]).name} exited"),
    ('the version command output is not matched',
     "elif pre.get('match') and not re.search(pre['match'], text):", "elif False:"),
    ('an unknown prerequisite kind is ignored',
     "            failures.append(f'{label}: unknown kind {kind!r}')", "            pass"),
    ('free disk space is not compared',
     "        if free < need:", "        if False:"),
    ('a supplied stage with nothing supplied is not reported',
     "                failures.append(f'stage {s[\"id\"]}: nothing supplied (give --supply {s[\"id\"]}=PATH)')", "                pass"),
    ('a --supply for a stage that takes none is ignored',
     "            failures.append(f'--supply {s}: no stage of the \"supplied\" kind has this name')", "            pass"),
    ('a stage that reads a disc that was not given is not reported',
     "            failures.append(f'stage {s[\"id\"]}: needs the {s.get(\"disc\", \"play\")} disc (not given)')", "            pass"),
    ('a listed disc file that is missing is not reported',
     "        if size is None:\n            problems.append", "        if False:\n            problems.append"),
    ('the size of a listed disc file is not compared',
     "elif 'size' in rec and size != rec['size']:", "elif False:"),
    ('the hash of a listed disc file is not compared',
     "elif hash_files and 'sha256' in rec and disc_hash(disc, rec['path'])[0] != rec['sha256']:", "elif False:"),
    # ---- receipts
    ('a matching receipt never skips the stage',
     "        if reason is None:\n            row.update(action='skipped'", "        if False:\n            row.update(action='skipped'"),
    ('a stage key ignores the stage before it',
     "'previous': previous_key,", "'previous': 'x',"),
    ('a stage key ignores the stage definition',
     "'definition': hashlib.sha256(canonical(stage_definition(stage))).hexdigest(),", "'definition': 'x',"),
    ('words for the reader are part of a stage key',
     "return {k: v for k, v in stage.items() if k not in ('description', 'note')}", "return dict(stage)"),
    ('a stage key ignores the identified game',
     "'game': ctx.game['executable_sha256'], 'previous'", "'game': 'x', 'previous'"),
    ('a stage key ignores its input hashes',
     "parts['inputs'][name] = sha256_path(Path(ctx.expand(raw)))['sha256']", "parts['inputs'][name] = 'x'"),
    ('a stage key ignores its tool hashes',
     "parts['tools'][raw] = sha256_path(Path(ctx.expand(raw)))['sha256']", "parts['tools'][raw] = 'x'"),
    ('a stage key ignores the disc file it copies',
     "parts['inputs']['disc:' + stage['path']] = disc_hash(disc, stage['path'])[0]", "parts['inputs']['disc:' + stage['path']] = 'x'"),
    ('a stage key ignores the supplied file',
     "parts['inputs']['supplied'] = sha256_path(supplied_source(stage, ctx, supply))['sha256']", "parts['inputs']['supplied'] = 'x'"),
    ('a tree digest counts interpreter caches',
     "        if TREE_IGNORED.intersection(rel.parts):", "        if False:"),
    ('a receipt of another format is trusted',
     "return r if isinstance(r, dict) and r.get('schema') == RECEIPT_SCHEMA else None", "return r"),
    ('a missing output is not noticed before a skip',
     "        if rec is None or not path.exists():", "        if False:"),
    ('recorded outputs are not re-hashed before a skip',
     "        if sha256_path(path)['sha256'] != rec['sha256']:", "        if False:"),
    # ---- running stages
    ('an expected output hash is not enforced',
     "if want and recorded[name]['sha256'] != ctx.expand(want):", "if False:"),
    ('old outputs are not removed before a stage reruns',
     "        if t.is_dir():\n            shutil.rmtree(t)", "        if t.is_dir():\n            pass"),
    ('folders a tool wants are not made',
     "work_target(ctx, sid, value, 'folder to make').mkdir(parents=True, exist_ok=True)", "pass"),
    ('the exit code of a command is ignored',
     "        if r.returncode != 0:\n            raise BuilderError(f'stage {sid}: command exited",
     "        if False:\n            raise BuilderError(f'stage {sid}: command exited"),
    ('a command that wrote nothing is accepted',
     "        if not p.exists():\n            raise BuilderError(f'stage {sid}: output {name} was not produced ({p})')",
     "        if False:\n            raise BuilderError(f'stage {sid}: output {name} was not produced ({p})')"),
    ('tools are not niced',
     "argv, priority_options = host_support.low_priority(argv, nice)", "argv, priority_options = list(argv), {}"),
    ('tools run with a random hash seed',
     "        env['PYTHONHASHSEED'] = '0'", "        pass"),
    ('the folder a stage asks to run in is ignored',
     "cwd = Path(ctx.expand(stage['cwd'])) if stage.get('cwd') else ctx.repo", "cwd = ctx.repo"),
    ('tools run in whatever folder the builder was started from',
     "cwd = Path(ctx.expand(stage['cwd'])) if stage.get('cwd') else ctx.repo",
     "cwd = Path(ctx.expand(stage['cwd'])) if stage.get('cwd') else None"),
    ('test switches of the project reach the tools',
     "env = {k: v for k, v in os.environ.items() if not k.endswith('_MUTANT')}", "env = {k: v for k, v in os.environ.items()}"),
    ('an older file outside the work directory is accepted as built',
     "if not p.is_file() or stamps.get(name) == file_stamp(p):", "if False:"),
    ('the file an external output replaces is not kept',
     "                shutil.copyfile(p, kept)", "                pass"),
    ('heavy stages run without --allow-heavy',
     "elif row['heavy'] and not allow_heavy:", "elif False:"),
    ('a dry run executes the stages',
     "        elif dry_run:\n            row.update(action='would-run', reason=reason)", "        elif False:\n            row.update(action='would-run', reason=reason)"),
    ('--stop-after does not stop',
     "        if stop_after == sid:", "        if False:"),
    # ---- writing the bundle
    ('the bundle manifest records a wrong component hash',
     "'size': src.stat().st_size, 'sha256': sha256_file(src),", "'size': src.stat().st_size, 'sha256': sha256_file(Path(__file__)),"),
    ('bundle entries carry the time of the build',
     "info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))", "info = zipfile.ZipInfo(name, date_time=time.localtime()[:6])"),
    ('the written bundle is not read back',
     "        verify_bundle(path, ctx.game['executable_sha256'])   # read back what was written, as the app will", "        pass"),
    ('a bundle that failed its read-back is left behind',
     "        path.unlink()                                        # a bundle the app would refuse is not left behind", "        pass"),
    # ---- checking a bundle
    ('the size of a bundle file is not checked first',
     "if size < EOCD.size + SEAL_LENGTH or size > ZIP_LIMIT:", "if False:"),
    ('the shape of the seal record is not checked',
     "if sig != b'PK\\x05\\x06' or comment_len != SEAL_LENGTH or not comment.startswith(SEAL_PREFIX):", "if False:"),
    ('the bundle seal is not compared',
     "if comment[len(SEAL_PREFIX):] != seal.encode():", "if False:"),
    ('anything may stand where a ZIP entry has to start',
     "if len(head) < LOCAL_HEADER.size or head[:4] != b'PK\\x03\\x04':", "if False:"),
    ('encrypted entries and entries without their size are accepted',
     "if flags & ~0x0800 or method != 0 or packed != length:", "if method != 0 or packed != length:"),
    ('an entry flagged as having a UTF-8 name is refused',
     "if flags & ~0x0800 or method != 0 or packed != length:", "if flags or method != 0 or packed != length:"),
    ('compressed entries are accepted',
     "if flags & ~0x0800 or method != 0 or packed != length:", "if flags & ~0x0800 or packed != length:"),
    ('entries with two different sizes are accepted',
     "if flags & ~0x0800 or method != 0 or packed != length:", "if flags & ~0x0800 or method != 0:"),
    ('an entry may run past the end of the file',
     "            if pos > end:", "            if False:"),
    ('the manifest need not be the first entry',
     "                if name != MANIFEST_NAME:", "                if False:"),
    ('a manifest of any size is read',
     "                if length > MAX_MANIFEST_BYTES:", "                if False:"),
    ('the CRC-32 of the manifest is not compared',
     "                if zlib.crc32(data) != crc:", "                if False:"),
    ('a manifest of another schema is accepted',
     "if not isinstance(manifest, dict) or manifest.get('schema') not in (BUNDLE_SCHEMA, BUNDLE_SCHEMA_2):", "if not isinstance(manifest, dict):"),
    ('a manifest without a game executable hash is accepted',
     "        raise bad('the manifest names no game executable hash')", "        pass"),
    ('a manifest without runtime_abi is accepted',
     "        raise bad('the manifest names no runtime interface (runtime_abi)')", "        pass"),
    ('a manifest without components is accepted',
     "        raise bad('the manifest lists no components')", "        pass"),
    ('the shape of a component is not checked',
     "            raise bad('a component in the manifest lacks a name, a size or a SHA-256')", "            pass"),
    ('component names in a manifest are not checked',
     "            raise bad(f'component name {name!r} is not allowed or is listed twice')", "            pass"),
    ('an unknown role is accepted',
     "            raise bad(f'component {name} has the unknown role {c.get(\"role\")!r}')", "            pass"),
    ('a bundled executable other than the named one is accepted',
     "            raise bad('the bundled executable is not the one the manifest names')", "            pass"),
    ('an entry the manifest does not list is accepted',
     "            if name not in comps:\n                raise bad", "            if False:\n                raise bad"),
    ('an entry may appear twice',
     "            if name in seen:\n                raise bad", "            if False:\n                raise bad"),
    ('component sizes are not compared with the manifest',
     "            if length != comps[name]['size']:", "            if False:"),
    ('the CRC-32 of a component is not compared',
     "            if crc_now != crc:", "            if False:"),
    ('component hashes are not compared with the manifest',
     "            if digest != comps[name]['sha256']:", "            if False:"),
    ('a missing component is accepted',
     "        if len(seen) != len(comps):", "        if False:"),
    ('a bundle for another executable is accepted',
     "if executable_sha256 and manifest['game']['executable_sha256'] != executable_sha256:", "if False:"),
    # ---- the discs as a variable
    ('a disc that was not given expands to nothing',
     "                if disc is None:\n                    raise BuilderError(f'profile: {m.group(0)} needs that disc on the command line', EXIT_USAGE)\n", "                if disc is None:\n                    return ''\n"),
    ('the disc variable gives the play disc whatever is asked',
     "                disc = self.discs.get(name[5:])", "                disc = self.discs.get('play')"),
    # ---- format 2
    ('runtime data is accepted in a format-1 manifest',
     "if c.get('role') not in KNOWN_ROLES and not (second and c.get('role') == RUNTIME_ROLE):",
     "if c.get('role') not in KNOWN_ROLES and not (c.get('role') == RUNTIME_ROLE):"),
    ('runtime data may be named anywhere in a manifest',
     "        if (c['role'] == RUNTIME_ROLE) != name.startswith(RUNTIME_PREFIX):", "        if False:"),
    ('a format-2 manifest may lack a main component',
     "            if sum(c['role'] == role for c in by_name.values()) != 1:", "            if sum(c['role'] == role for c in by_name.values()) > 1:"),
    ('a format-2 manifest may hold a main component twice',
     "            if sum(c['role'] == role for c in by_name.values()) != 1:", "            if sum(c['role'] == role for c in by_name.values()) < 1:"),
    ('stage records are accepted in a format-2 manifest',
     "        if 'stages' in manifest or 'versions' in manifest:", "        if 'versions' in manifest:"),
    ('version records are accepted in a format-2 manifest',
     "        if 'stages' in manifest or 'versions' in manifest:", "        if 'stages' in manifest:"),
    ('a format-2 manifest needs no statement of what it is made from',
     "        if not isinstance(manifest.get('portable_provenance'), dict):", "        if False:"),
    ('any bundle format number is accepted in a profile',
     "    if spec.get('format', 1) not in (1, 2):", "    if False:"),
    ('runtime data may be named anywhere in a profile',
     "        if (c['role'] == RUNTIME_ROLE) != component.startswith(RUNTIME_PREFIX):", "        if False:"),
    ('a format-2 profile may lack a main component',
     "            if sum(c['role'] == role for c in spec['components']) != 1:", "            if sum(c['role'] == role for c in spec['components']) > 1:"),
    ('a format-2 profile may name a main component twice',
     "            if sum(c['role'] == role for c in spec['components']) != 1:", "            if sum(c['role'] == role for c in spec['components']) < 1:"),
    ('the statement may hold values that are not texts',
     "or not all(isinstance(k, str) and isinstance(v, str) for k, v in stated.items()) \\\n", "\\\n"),
    ('the statement may be named tools',
     "                or 'tools' in stated:", "                or False:"),
    ('portable tools are not checked to exist',
     "            if not isinstance(tool, str) or not Path(ctx.expand(tool)).is_file():", "            if False:"),
    ('a format-2 manifest keeps stage records',
     "            'portable_provenance': dict(spec.get('provenance', {}), tools=tools),",
     "            'portable_provenance': dict(spec.get('provenance', {}), tools=tools), 'stages': [],"),
    ("tools are named by their path on this machine",
     "tools = [{'name': t, 'sha256': sha256_file(Path(ctx.expand(t)))}", "tools = [{'name': ctx.expand(t), 'sha256': sha256_file(Path(ctx.expand(t)))}"),
    ('the tools are left out of the statement',
     "            'portable_provenance': dict(spec.get('provenance', {}), tools=tools),", "            'portable_provenance': dict(spec.get('provenance', {})),"),
    ("the profile's statement is left out",
     "            'portable_provenance': dict(spec.get('provenance', {}), tools=tools),", "            'portable_provenance': dict(tools=tools),"),
    ('a format-2 profile is written as format 1',
     "    if spec.get('format', 1) == 2:\n        # A tool is named", "    if False:\n        # A tool is named"),
]


def run_one(args) -> tuple[bool, str]:
    index, what, old, new, source, tmp = args
    if source.count(old) != 1:
        return False, f'mutant {index:3d} DOES NOT APPLY ({source.count(old)} matches): {what}'
    mutated = Path(tmp) / f'm{index}' / 'disc_builder.py'
    mutated.parent.mkdir()
    mutated.write_text(source.replace(old, new))
    shutil.copyfile(HELPER, mutated.with_name(HELPER.name))     # the engine imports the host helper from beside itself
    try:
        r = subprocess.run([sys.executable, str(TESTS), '-v', '--failfast'], env=dict(os.environ, DISC_BUILDER=str(mutated)),
                           capture_output=True, text=True, timeout=TIME_LIMIT)
    except subprocess.TimeoutExpired:
        return True, f'mutant {index:3d} killed by the time limit ({TIME_LIMIT} s: the tests did not come to an end): {what}'
    failed = [line.split(' ... ')[0].strip() for line in r.stderr.splitlines() if line.rstrip().endswith(('... FAIL', '... ERROR'))]
    if r.returncode == 0:
        return False, f'mutant {index:3d} SURVIVED: {what}'
    return True, f'mutant {index:3d} killed by {failed[0] if failed else "the test run (it could not start)"}: {what}'


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--only', help='run only the mutants whose description contains this text')
    a = ap.parse_args()
    source = TOOL.read_text()
    chosen = [(i, m) for i, m in enumerate(MUTANTS, 1) if not a.only or a.only in m[0]]
    killed = 0
    with tempfile.TemporaryDirectory() as tmp, concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        # Control: an unchanged copy, placed like the mutants, must pass. If it does not, some test depends on where
        # the tool lies and would "kill" every mutant for that reason alone.
        control = Path(tmp) / 'control' / 'disc_builder.py'
        control.parent.mkdir()
        control.write_text(source)
        shutil.copyfile(HELPER, control.with_name(HELPER.name))
        r = subprocess.run([sys.executable, str(TESTS)], env=dict(os.environ, DISC_BUILDER=str(control)), capture_output=True, text=True)
        if r.returncode != 0:
            print('CONTROL FAILED: the tests do not pass on an unchanged copy of the tool; no mutant result would mean anything\n'
                  + r.stderr[-2000:])
            return 1
        print('control: the tests pass on an unchanged copy of the tool', flush=True)
        for ok, line in pool.map(run_one, [(i, *m, source, tmp) for i, m in chosen]):
            print(line, flush=True)
            killed += ok
    print(f'{killed} of {len(chosen)} mutants killed')
    return 0 if killed == len(chosen) else 1


if __name__ == '__main__':
    raise SystemExit(main())
