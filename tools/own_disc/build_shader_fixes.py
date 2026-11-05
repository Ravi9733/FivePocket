#!/usr/bin/env python3
"""The renderer's later shader fix tables, made from this run's disc translation records.

After the first builder (attempt v507) the renderer gained four tables of SPIR-V modules that replace pack modules
(the Fold and Thor lanes, 2026-10-21/22). Each was written by a private script that re-translated the device pack's
shaders with a changed translator and checked a control first. This stage makes the same four tables from the run's
own records, by the same rules:

  ptx             one HLSL change in the plain-v41 translation of a vertex shader: r0 starts as (SV_VertexID, 0, 0, 0)
  cloth           the co-issue translation of each cloth vertex shader with its 2nd and 3rd POSITION1 read given their
                  own inputs (locations 23 and 24), mapped by the destination register each vfetch writes
  coissue         the co-issue translation (scalar sources read before the paired vector write)
  coissue-reglod  the register-LOD translation with the co-issue order too, for keys the reglod table already owns

Controls, per key: the plain v41 translation (or, for coissue-reglod, the register-LOD translation) compiled by dxc must
give the pack module (the reglod table module) byte for byte; a co-issue translation must hold the same lines as its
control in another order; every new module must differ from the one it replaces and pass spirv-val. A key a table
names that fails any check refuses the stage: nothing is left out silently.

What steers it is a private parameter file (keys and the tables' first lines, which name the scripts the tables came
from): --recipes FILE, {"schema": "shader-fixes/1", "tables": [{"name", "kind", "keys": [hex...], "head": [lines],
"sha256"}]}. The keys are hashes; this tool holds no game data. Output, game-derived, stays under local-analysis/.
"""
from __future__ import annotations
import argparse
import collections
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True

KINDS = ('ptx', 'cloth', 'coissue', 'coissue-reglod')
NEW_LOCATIONS = (23, 24)
SPIRV_MAGIC = b'\x03\x02\x23\x07'


def sha(data: bytes) -> str:
    import hashlib
    return hashlib.sha256(data).hexdigest()


def fnv64(data: bytes) -> int:
    h = 0xcbf29ce484222325
    for x in data:
        h = ((h ^ x) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def pack_entries(data: bytes) -> dict:
    """key -> (stage, module, elements, destination registers) of a GSP2 pack (the layout generic_shader_pack.py writes)."""
    magic, version, count, _ = struct.unpack_from('<4sIII', data, 0)
    if magic != b'GSP2' or version != 2:
        raise ValueError('pack header refused')
    out = {}
    for i in range(count):
        at = 16 + i * 152
        key, stage, offset, words, n = struct.unpack_from('<QIIII', data, at)
        elements = struct.unpack_from('<24I', data, at + 24)[:n]
        dst = struct.unpack_from('<24B', data, at + 24 + 96 + 8)[:n]
        out[key] = (stage, data[offset:offset + 4 * words], elements, dst)
    return out


def table_modules(text: str) -> dict:
    """key -> module bytes of a generated override table ({0x<key>ull,<words>u,<Array>} rows over static arrays)."""
    arrays = {n: [int(x, 16) for x in body.split(',')]
              for n, body in re.findall(r'static const std::uint32_t (\w+)\[\]=\{([^}]*)\};', text)}
    out = {}
    for key, words, name in re.findall(r'\{0x([0-9a-f]{16})ull,(\d+)u,(\w+)[,}]', text):
        if name not in arrays or len(arrays[name]) != int(words):
            raise ValueError('override table row refused')
        out[int(key, 16)] = struct.pack(f'<{len(arrays[name])}I', *arrays[name])
    return out


def spirv_words(spv: bytes):
    return struct.unpack(f'<{len(spv) // 4}I', spv)


def builtins(spv: bytes) -> set:
    w, out, i = spirv_words(spv), set(), 5
    while i < len(w):
        op, n = w[i] & 0xFFFF, w[i] >> 16
        if op == 71 and n >= 4 and w[i + 2] == 11:           # OpDecorate <id> BuiltIn <which>
            out.add(w[i + 3])
        i += max(n, 1)
    return out


def input_locations(spv: bytes) -> set:
    w, loc, storage, i = spirv_words(spv), {}, {}, 5
    while i < len(w):
        op, n = w[i] & 0xFFFF, w[i] >> 16
        if op == 71 and w[i + 2] == 30:                      # OpDecorate Location
            loc[w[i + 1]] = w[i + 3]
        if op == 59:                                         # OpVariable result, storage class
            storage[w[i + 2]] = w[i + 3]
        i += max(n, 1)
    return {l for v, l in loc.items() if storage.get(v) == 1}


def ptx_hlsl(text: str) -> str:
    """The one change: r0 starts with the vertex index in .x (the console's r0.x); nothing else."""
    sig_old = '#else\n\tVertexShaderInput input\n#endif\n)'
    sig_new = '#else\n\tVertexShaderInput input,\n\tuint iVertexIndex : SV_VertexID\n#endif\n)'
    init_old = '\tfloat4 r0 = 0.0;\n'
    init_new = '\tfloat4 r0 = float4((float)(iVertexIndex), 0.0, 0.0, 0.0);\n'
    for old in (sig_old, init_old):
        if text.count(old) != 1:
            raise ValueError('ptx HLSL shape refused')
    after = text[text.index('\t\tcase 0:\n') + len('\t\tcase 0:\n'):]
    if after.split('\n', 1)[0].strip() != 'r0.xy = (float2)((r0.xx * c250.xy));':
        raise ValueError('ptx program start refused')
    return text.replace(sig_old, sig_new).replace(init_old, init_new)


def cloth_hlsl(text: str, reads: list) -> str:
    """reads: (destination register, new location) for the 2nd and 3rd POSITION1 read."""
    lines = text.split('\n')
    pat = re.compile(r'^(\t+)r(\d+)\.([xyzw]+) = \(float3\)\(\(input\.iPosition1\)\)\.xyz;$')
    found = [(i, int(m.group(2))) for i, l in enumerate(lines) if (m := pat.match(l))]
    if len(found) != 3 or len({r for _, r in found}) != 3:
        raise ValueError('cloth POSITION1 reads refused')
    for reg, location in reads:
        hit = [i for i, r in found if r == reg]
        if len(hit) != 1:
            raise ValueError('cloth POSITION1 register refused')
        lines[hit[0]] = lines[hit[0]].replace('input.iPosition1', f'input.iPosition1_l{location}')
    out = '\n'.join(lines)
    for decl, add in (('\tfloat4 iPosition1 [[attribute(1)]];', '\tfloat4 iPosition1_l{0} [[attribute({0})]];'),
                      ('\t[[vk::location(1)]] float4 iPosition1 : POSITION1;', '\t[[vk::location({0})]] float4 iPosition1_l{0} : XENOSPOSITION1_{0};')):
        if out.count(decl) != 1:
            raise ValueError('cloth declaration refused')
        out = out.replace(decl, decl + ''.join('\n' + add.format(l) for _, l in reads))
    return out


def reorder_of(control: str, changed: str) -> bool:
    a, b = control.splitlines(), changed.splitlines()
    return a != b and collections.Counter(a) == collections.Counter(b)


def table_text(kind: str, head: list, rows: list) -> str:
    """The table in the layout the renderer includes (and the lanes' scripts wrote, byte for byte)."""
    array, struct_name, table = {'ptx': ('PtxOverride', 'PtxOverride', 'PtxOverrides'),
                                 'cloth': ('ClothOverride', 'ClothOverride', 'ClothOverrides'),
                                 'coissue': ('CoIssueOverride', 'DenormOverride', 'CoIssueOverrides'),
                                 'coissue-reglod': ('CoIssueRegLodOverride', 'DenormOverride', 'CoIssueRegLodOverrides')}[kind]
    text = list(head)
    for i, row in enumerate(rows):
        code = row[1]
        if len(code) % 4 or code[:4] != SPIRV_MAGIC:
            raise ValueError('override module framing refused')
        text.append(f'static const std::uint32_t {array}{i}[]={{' + ','.join(f'0x{x:08x}' for x in spirv_words(code)) + '};')
    text.append(f'static const {struct_name} {table}[]={{')
    for i, row in enumerate(rows):
        key, code = row[0], row[1]
        tail = ''
        if kind == 'ptx':
            tail = f',0x{row[2]:016x}ull'
        elif kind == 'cloth':
            moved = row[3]
            tail = f',0x{row[2]:016x}ull,{{{{{moved[0][0]}u,{moved[0][2]}u}},{{{moved[1][0]}u,{moved[1][2]}u}}}}'
        text.append(f'    {{0x{key:016x}ull,{len(code) // 4}u,{array}{i}{tail}}},')
    text.append('};')
    return '\n'.join(text) + '\n'


class Toolchain:
    def __init__(self, config: dict, work: Path):
        import shader_toolchain
        self.c, self.work = config, work
        self.env = shader_toolchain.library_environment(config['compiler_library_directory'])

    def run(self, argv, folder: Path, tag: str):
        p = subprocess.run([str(a) for a in argv], cwd=folder, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (folder / (tag + '.log')).write_bytes(p.stdout)
        if p.returncode:
            raise ValueError(tag + ' refused; private log preserved')

    def translate(self, state: str, container: Path, folder: Path, tag: str) -> str:
        out = folder / f'{tag}.hlsl'
        self.run([self.c[state + '_translator'], container, out, self.c[state + '_header']], folder, tag + '-translate')
        return out.read_text(encoding='utf-8')

    def compile(self, stage: str, hlsl_text: str, folder: Path, tag: str) -> bytes:
        """dxc-3.7 with the corpus's flags for the stage (vertex: -fvk-invert-y), then spirv-val."""
        hlsl, spv = folder / f'{tag}.compile.hlsl', folder / f'{tag}.spv'
        hlsl.write_text(hlsl_text, encoding='utf-8', newline='\n')
        argv = [self.c['dxc'], '-T', 'vs_6_0' if stage == 'vertex' else 'ps_6_0', '-E', 'shaderMain', '-HV', '2021',
                '-all-resources-bound', '-spirv', '-fspv-target-env=vulkan1.1', '-fvk-use-dx-layout', '-Qstrip_debug',
                '-Fo', spv, hlsl]
        if stage == 'vertex':
            argv.append('-fvk-invert-y')
        argv += ['-D', 'XENOS_RECOMP_DESCRIPTOR_CONSTANTS=1']
        self.run(argv, folder, tag + '-dxc')
        self.run([self.c['spirv_validator'], '--target-env', 'vulkan1.1', '--relax-block-layout', spv], folder, tag + '-validate')
        return spv.read_bytes()


STATES = ('v41', 'coissue', 'coissue_reglod', 'reglod')


def make_row(kind: str, key: int, rec: dict, pack: dict, reglod: dict, tc: Toolchain, out: Path):
    stage_id, module, elements, dst = pack[key]
    stage = 'vertex' if stage_id == 0 else 'pixel'
    folder = out / 'records' / kind / f'{key:016x}'
    folder.mkdir(parents=True)
    container = rec['folder'] / 'container.xenos'
    if kind == 'coissue-reglod':
        if key not in reglod:
            raise ValueError('coissue-reglod key is not in the reglod table')
        control_hlsl = tc.translate('reglod', container, folder, 'control')
        if tc.compile(stage, control_hlsl, folder, 'control') != reglod[key]:
            raise ValueError('register-LOD control does not reproduce the reglod table module')
        fixed_hlsl = tc.translate('coissue_reglod', container, folder, 'fixed')
        if not reorder_of(control_hlsl, fixed_hlsl):
            raise ValueError('co-issue translation is not a reorder of the control')
        code = tc.compile(stage, fixed_hlsl, folder, 'fixed')
        if code == reglod[key]:
            raise ValueError('override unchanged')
        return key, code
    control_hlsl = tc.translate('v41', container, folder, 'control')
    if tc.compile(stage, control_hlsl, folder, 'control') != module:
        raise ValueError('v41 control does not reproduce the pack module')
    if kind == 'ptx':
        if stage != 'vertex' or sorted(e >> 24 for e in elements) != [13, 14] or builtins(module) & {42, 5}:
            raise ValueError('ptx shader shape refused')
        code = tc.compile(stage, ptx_hlsl(control_hlsl), folder, 'fixed')
        if 42 not in builtins(code):
            raise ValueError('ptx override has no BuiltIn VertexIndex')
        return key, code, fnv64(module)
    fixed_hlsl = tc.translate('coissue', container, folder, 'fixed')
    if kind == 'coissue':
        if not reorder_of(control_hlsl, fixed_hlsl):
            raise ValueError('co-issue translation is not a reorder of the control')
        code = tc.compile(stage, fixed_hlsl, folder, 'fixed')
        if code == module:
            raise ValueError('override unchanged')
        return key, code
    # cloth: the co-issue translation is the control's lines (reordered only where a hazard is), then the POSITION1 inputs
    if stage != 'vertex' or sorted(control_hlsl.split('\n')) != sorted(fixed_hlsl.split('\n')):
        raise ValueError('cloth co-issue translation refused')
    p1 = sorted((e & 0xFFF, j) for j, e in enumerate(elements) if ((e >> 12) & 15, (e >> 16) & 15) == (0, 1))
    if len(p1) != 3 or any(elements[j] >> 24 != 1 for _, j in p1):
        raise ValueError('cloth POSITION1 elements refused')
    moved = [(p1[1][0], dst[p1[1][1]], NEW_LOCATIONS[0]), (p1[2][0], dst[p1[2][1]], NEW_LOCATIONS[1])]
    code = tc.compile(stage, cloth_hlsl(fixed_hlsl, [(reg, loc) for _, reg, loc in moved]), folder, 'fixed')
    if input_locations(code) != {e >> 24 for e in elements} | set(NEW_LOCATIONS):
        raise ValueError('cloth override inputs refused')
    return key, code, fnv64(module), moved


def records_by_key(translations: Path) -> dict:
    """key -> the record the production packer takes for it (by record name, the first validated one)."""
    report = json.loads((translations / 'translation.json').read_text(encoding='utf-8'))
    if report.get('failed') or report.get('diagnostic_limit') is not None:
        raise ValueError('the shader fixes need the full validated translation run')
    out = {}
    for r in sorted(report['records'], key=lambda r: r['record']):
        if r['status'] != 'validated' or r['key'] in out:
            continue
        if not re.fullmatch(r'records-\d+/shader-\d+', r['record']):
            raise ValueError('record path refused')
        folder = (translations / r['record']).resolve(strict=True)
        if not folder.is_relative_to(translations) or sha((folder / 'container.xenos').read_bytes()) != r['container_sha256']:
            raise ValueError('record identity refused')
        out[r['key']] = dict(folder=folder, container_sha256=r['container_sha256'])
    return out


def run(args) -> int:
    started = time.monotonic()
    import shader_toolchain
    recipes = json.loads(args.recipes.read_text(encoding='utf-8'))
    if recipes.get('schema') != 'shader-fixes/1' or [t['kind'] for t in recipes['tables']] != list(KINDS):
        raise ValueError('shader-fix recipes refused')
    config = shader_toolchain.load(args.toolchain, args.host_tools, args.ndk, fixes=True)
    config = {k: str(Path(v).resolve(strict=True)) for k, v in config.items()}
    pack_bytes = args.pack.read_bytes()
    pack = pack_entries(pack_bytes)
    owned = {}
    for name in ('denorm', 'gradient', 'reglod'):
        for key in table_modules((args.overrides / f'generic_{name}_overrides.inc').read_text(encoding='utf-8')):
            owned[key] = name
    reglod = table_modules((args.overrides / 'generic_reglod_overrides.inc').read_text(encoding='utf-8'))
    records = records_by_key(args.translations)
    plan = []
    for t in recipes['tables']:
        keys = [int(k, 16) for k in t['keys']]
        if not keys or len(set(keys)) != len(keys):
            raise ValueError('a table\'s key list refused')
        for key in keys:
            if key not in pack or f'{key:016x}' not in records:
                raise ValueError('a listed key has no pack module or record')
        plan.append((t, keys))
    cloth_keys = set(plan[1][1])
    for key in plan[2][1]:
        if key in owned or key in cloth_keys:
            raise ValueError('a co-issue key is owned by another table')
    for key in plan[0][1] + plan[1][1]:
        if key in owned:
            raise ValueError('a ptx or cloth key is owned by another table')
    if not 1 <= args.jobs <= 4:
        raise ValueError('jobs must be 1..4')
    if args.check:
        print('SHADER_FIXES_CHECK PASS', {t['kind']: len(k) for t, k in plan})
        return 0
    args.output.mkdir(parents=True, exist_ok=False)
    tc = Toolchain(config, args.output)
    outputs = {}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for t, keys in plan:
            rows = list(pool.map(lambda k: make_row(t['kind'], k, records[f'{k:016x}'], pack, reglod, tc, args.output), keys))
            if t['kind'] == 'cloth':
                rows.sort(key=lambda r: r[0])
            text = table_text(t['kind'], t['head'], rows)
            path = args.output / t['name']
            path.write_text(text, encoding='utf-8', newline='\n')
            digest = sha(path.read_bytes())
            if t.get('sha256') and digest != t['sha256']:
                raise ValueError(f'{t["name"]} differs from the hash its recipe names')
            outputs[t['name']] = dict(sha256=digest, bytes=path.stat().st_size, modules=len(rows), kind=t['kind'])
    report = dict(status='complete', outputs=outputs, pack_sha256=sha(pack_bytes), recipes_sha256=sha(args.recipes.read_bytes()),
                  translation_sha256=sha((args.translations / 'translation.json').read_bytes()),
                  tools={k: sha(Path(v).read_bytes()) for k, v in config.items() if Path(v).is_file()},
                  jobs=args.jobs, elapsed_seconds=round(time.monotonic() - started, 1), gpu_execution=False)
    (args.output / 'report.json').write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8', newline='\n')
    print('SHADER_FIXES_RESULT PASS', {n: o['modules'] for n, o in outputs.items()})
    return 0


def main() -> int:
    os.umask(0o077)
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--repo', type=Path, required=True)
    for n in ('translations', 'pack', 'overrides', 'recipes', 'output'):
        p.add_argument('--' + n, type=Path, required=True)
    for n in ('toolchain', 'host-tools', 'ndk'):
        p.add_argument('--' + n, type=Path)
    p.add_argument('--check', action='store_true')
    p.add_argument('--jobs', type=int, default=4)
    args = p.parse_args()
    for n in ('repo', 'translations', 'pack', 'overrides', 'recipes', 'output', 'toolchain', 'host_tools', 'ndk'):
        if getattr(args, n) is not None:
            setattr(args, n, getattr(args, n).resolve())
    sys.path.insert(0, str(args.repo / 'tools/own_disc'))
    from disc_builder import check_private_path
    check_private_path(args.output, 'shader fixes output')
    return run(args)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError) as e:
        print(f'SHADER_FIXES_REFUSED ({type(e).__name__}: {e}); private partial output preserved', file=sys.stderr)
        raise SystemExit(2)
