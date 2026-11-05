#!/usr/bin/env python3
"""Mutant check for make_release.py: copies of the tool with one rule changed; test_make_release.py (beside this file)
must fail on each. First two controls: the tool as it is, and an unchanged copy. Exits 1 if a control fails, a mutant
survives or no longer applies. The tool itself is not touched."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'make_release.py'
TESTS = HERE / 'test_make_release.py'
MUTANTS = [
    ('Python\'s cache folders are listed', "        if any(part in SKIPPED_FOLDERS for part in rel.parts):\n", "        if False:\n"),
    ('a link is listed as the file it points to', "        if path.is_symlink():\n            raise ValueError(f'a link is in the package: {rel.as_posix()}')\n", ""),
    ('a name the front end refuses is listed', "        except builder.Refusal:\n            raise ValueError(f'the front end would refuse this name: {name}') from None\n", "        except builder.Refusal:\n            pass\n"),
    ('names keep the host\'s order', "    for path in sorted(package.rglob('*'), key=lambda p: p.relative_to(package).as_posix()):", "    for path in sorted(package.rglob('*'), key=lambda p: p.relative_to(package).as_posix(), reverse=True):"),
    ('the hash is of the name', "        record = {'path': name, 'sha256': db.sha256_file(path)}\n", "        record = {'path': name, 'sha256': __import__('hashlib').sha256(name.encode()).hexdigest()}\n"),
    ('no file is marked as a program', "        if is_program(path, host):\n            record['executable'] = True\n", ""),
    ('every file is marked as a program', "        if is_program(path, host):\n            record['executable'] = True\n", "        record['executable'] = True\n"),
    ('a Windows package goes by the mode', "    if host.startswith('win32'):\n        return path.suffix.lower() in PROGRAM_SUFFIXES\n", ""),
    ('every package goes by the suffix', "    if host.startswith('win32'):\n        return path.suffix.lower() in PROGRAM_SUFFIXES\n    return bool(path.stat().st_mode & 0o111)", "    return path.suffix.lower() in PROGRAM_SUFFIXES"),
    ('an empty package is written', "    if not files:\n        raise ValueError('the package is empty')\n", ""),
    ('a missing package folder is walked', "    if not package.is_dir():\n        raise ValueError('the package folder is not there')\n", ""),
    ('an existing inventory is written over', "    if output.exists():\n        raise ValueError('the inventory exists already: nothing is written over')\n", "    if output.exists():\n        output.unlink()\n"),
    ('the inventory may lie anywhere', "    if output.parent != package:\n        raise ValueError('the inventory lies at the package root (the files are named from its folder)')\n", ""),
    ('any free-space figure is taken', "    if type(min_free_bytes) is not int or min_free_bytes <= 0:\n", "    if False:\n"),
    ('zero free space is taken', "    if type(min_free_bytes) is not int or min_free_bytes <= 0:\n", "    if type(min_free_bytes) is not int or min_free_bytes < 0:\n"),
    ('the profile need not be in the package', "    for what, name in (('profile', profile), ('Python', python)):\n", "    for what, name in (('Python', python),):\n"),
    ('the Python need not be in the package', "    for what, name in (('profile', profile), ('Python', python)):\n", "    for what, name in (('profile', profile),):\n"),
    ('the compiler pins are not checked', "    try:\n        builder.ndk_spec(release)\n    except builder.Refusal as error:\n        raise ValueError(f'the compiler archive is not pinned as the front end wants it: {error}') from None\n", ""),
    ('the licence address is left out', "'ndk': dict(ndk, license_url=LICENSE_URL),", "'ndk': dict(ndk),"),
    ('the time estimate is always written', "**({'build_time': build_time} if build_time else {})", "'build_time': build_time"),
    ('the time estimate is never written', "**({'build_time': build_time} if build_time else {})", ""),
    ('the inventory is not read back', "    if host == builder.host_id():           # the consumer's own reader, where it can run\n", "    if False:\n"),
    ('the inventory is read back for every host', "    if host == builder.host_id():           # the consumer's own reader, where it can run\n", "    if True:\n"),
    ('a refused inventory stays', "        except builder.Refusal as error:\n            output.unlink()\n", "        except builder.Refusal as error:\n"),
    ('the default output is the current folder', "a.output or a.package / 'release.json', dxc)", "a.output or Path('release.json'), dxc)"),
    ('a refusal ends with success', "        print('RELEASE_REFUSED:', error, file=sys.stderr)\n        return 2\n", "        print('RELEASE_REFUSED:', error, file=sys.stderr)\n        return 0\n"),
    ('the unpacked size is the archive size', "'unpacked_bytes': a.ndk_unpacked_bytes}", "'unpacked_bytes': a.ndk_size}"),
    ('the shader compiler pins are not written', "    if dxc is not None:\n        release['dxc'] = fetched_compiler(dxc, listed)\n", ""),
    ('the shader compiler pins are written unchecked', "        release['dxc'] = fetched_compiler(dxc, listed)\n", "        release['dxc'] = dxc\n"),
    ('pins the front end refuses are written', "    try:\n        builder.dxc_spec({'dxc': dxc})\n    except builder.Refusal as error:\n        raise ValueError(f'the shader compiler is not pinned as the front end wants it: {error}') from None\n", ""),
    ('a fetched file may lie in the package', "        if record['path'].casefold() in held:\n", "        if False:\n"),
    ('a fetched file in the package is found by exact spelling only', "    held = {name.casefold() for name in listed}\n    for record in dxc['files']:\n        if record['path'].casefold() in held:", "    held = set(listed)\n    for record in dxc['files']:\n        if record['path'] in held:"),
    ('an empty field is written without pins', "    if dxc is not None:\n        release['dxc'] = fetched_compiler(dxc, listed)\n", "    release['dxc'] = fetched_compiler(dxc, listed) if dxc is not None else None\n"),
    ('the pins file is not read', "        dxc = json.loads(a.dxc.read_text(encoding='utf-8')) if a.dxc else None\n", "        dxc = None\n"),
    ('a pins file that holds no record is passed on', "        if a.dxc and not isinstance(dxc, dict):\n            raise ValueError('the shader compiler is not pinned as the front end wants it: the pins file holds no record')\n", ""),
    ('the report does not name the fetched files', "    fetched = f\", {len(release['dxc']['files'])} shader compiler files fetched by the builder\" if 'dxc' in release else ''\n", "    fetched = ''\n"),
]
# The tracked pins of Microsoft's release are data: copies with one field changed, read by the test through DXC_PINS.
PINS = HERE.parent / 'windows' / 'dxc-v1.9.2607.json'
PINS_TEST = 'test_the_tracked_pins_are_what_the_front_end_and_the_shader_stages_expect'
PIN_MUTANTS = [
    ('the compiler goes to another name', '"path": "host-tools/dxc/bin/dxc-3.7.exe"', '"path": "host-tools/dxc/bin/dxc.exe"'),
    ('the compiler library goes beside the program', '"path": "host-tools/dxc/lib/dxcompiler.dll"', '"path": "host-tools/dxc/bin/dxcompiler.dll"'),
    ('the signing library goes beside the program', '"path": "host-tools/dxc/lib/dxil.dll"', '"path": "host-tools/dxc/bin/dxil.dll"'),
    ('the archive of another release', '/releases/download/v1.9.2607/dxc_2026_07_29.zip', '/releases/download/v1.8.2505/dxc_2025_05_24.zip'),
    ('the archive hash of another file', '"sha256": "a1dfb116', '"sha256": "b1dfb116'),
    ('the archive size is one less', '"size": 41625275', '"size": 41625274'),
    ('the terms of another revision', '/blob/v1.9.2607/LICENSE.TXT', '/blob/main/LICENSE.TXT'),
    ('Microsoft\'s terms are not taken along', '  {\n   "member": "LICENSE-MS.txt",\n   "path": "host-tools/dxc/LICENSE-MS.txt",\n   "size": 13163,\n   "sha256": "734f72f239fe7b07b4c7203f294c1a7ce27095687278bab7e56d630d7c672963"\n  },\n', ''),
    ('a file hash is not a hash', '"sha256": "980a3a4c6e5c88f5737dde321e548860021d58fa2349790d4e572805cb298293"', '"sha256": "980a3a4c"'),
    ('the arm64 compiler is taken', '"member": "bin/x64/dxc.exe"', '"member": "bin/arm64/dxc.exe"'),
]

def run(tool):
    return subprocess.run([sys.executable, '-B', str(TESTS)], capture_output=True, text=True, env=dict(os.environ, MAKE_RELEASE=str(tool)))


def run_pins(pins):
    return subprocess.run([sys.executable, '-B', str(TESTS), '-k', PINS_TEST], capture_output=True, text=True, env=dict(os.environ, DXC_PINS=str(pins)))


def main() -> int:
    source = TOOL.read_text(encoding='utf-8')
    if run(TOOL).returncode != 0:
        print('CONTROL FAILED: the tests do not pass on the tool as it is')
        return 1
    print('control: the tests pass on the tool as it is', flush=True)
    killed = 0
    with tempfile.TemporaryDirectory() as folder:
        same = Path(folder) / 'unchanged.py'
        same.write_text(source, encoding='utf-8', newline='\n')
        if run(same).returncode != 0:
            print('CONTROL FAILED: an unchanged copy of the tool does not pass (a mutant would be killed by being a copy)')
            return 1
        print('control: an unchanged copy passes too', flush=True)
        for index, (what, old, new) in enumerate(MUTANTS, 1):
            if source.count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({source.count(old)} matches): {what}')
                continue
            copy = Path(folder) / f'mutant_{index}.py'
            copy.write_text(source.replace(old, new), encoding='utf-8', newline='\n')
            if run(copy).returncode == 0:
                print(f'mutant {index:3d} SURVIVED: {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed: {what}')
        pins = PINS.read_text(encoding='utf-8')
        same = Path(folder) / 'pins-unchanged.json'
        same.write_text(pins, encoding='utf-8', newline='\n')
        result = run_pins(same)
        if result.returncode != 0 or 'Ran 1 test' not in result.stdout + result.stderr:
            print('CONTROL FAILED: the pins test does not pass on an unchanged copy of the tracked pins')
            return 1
        print('control: an unchanged copy of the tracked pins passes its test', flush=True)
        for index, (what, old, new) in enumerate(PIN_MUTANTS, len(MUTANTS) + 1):
            if pins.count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({pins.count(old)} matches): {what}')
                continue
            copy = Path(folder) / f'pins_{index}.json'
            copy.write_text(pins.replace(old, new), encoding='utf-8', newline='\n')
            if run_pins(copy).returncode == 0:
                print(f'mutant {index:3d} SURVIVED: {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed: {what}')
    total = len(MUTANTS) + len(PIN_MUTANTS)
    print(f'{killed} of {total} mutants killed')
    return 0 if killed == total else 1


if __name__ == '__main__':
    sys.exit(main())
