#!/usr/bin/env python3
"""Make the runtime's "native shader pack" headers from the player's own discs.

A pack header holds, for single shaders of the game, the shader's bytes (the runtime recognises the shader by them)
and SPIR-V made for it. Until 2026-10-08 the compile stage read 57 such headers from earlier private work. This stage
writes them again from what the run itself took from the discs. A recipe (hashes, numbers and this project's own
header text; no game bytes) says, per header:

  shaders   each shader of the header: its stage, the SHA-256 of its canonical form (how it is found among the
            disc's shader containers), for a vertex shader the fetch numbers the game sets when it loads it and the
            instruction slots the game's runtime replaces by its null instruction, and the SHA-256 the resulting
            bytes must have
  modules   each SPIR-V module: either the translator call (a translate_* function of tools/rpf7_inventory, its
            arguments and options) and which of its results is the GLSL, or the GLSL itself where the module is a
            hand-written helper of this project; the kind (vert, frag, comp), the glslc flags, and the SHA-256 the
            module must have. A translated module may name renames: the translator of September numbered a few
            of its temporaries differently, and such a module is today's translation with those names put back
            (names only: [from, to] pairs, whole names, all at once). A call may name a revision: the
            translator as it was then, kept in tools/rpf7_inventory/history/<revision> and run in a process of
            its own
  text      the header's text in order: literal pieces, and where a shader's bytes or a module's words go (with
            the number form, the separator and, where the header breaks its lines by count, the numbers per line)

    build_native_packs.py --inventory DIR --recipes FILE --glslc PATH --output DIR [--only TEXT]

--inventory is the output of `build_shader_pack.py extract`. The output folder must not exist. Every header is
checked against the size and SHA-256 its recipe names before it is written; exit 1 if one differs, 2 if an argument,
the recipe file or the inventory is refused. Nothing is written for a header that differs.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TRANSLATORS = ROOT / 'tools' / 'rpf7_inventory'
SCHEMA = 'native-packs/1'
# the ways the headers write their numbers: 0x1f, 0x1Fu, two digits at least (0x01), eight digits and a u (0x0000001fu)
STYLES = {'hex': lambda v: hex(v), 'hexu': lambda v: '0x%Xu' % v, 'hex2': lambda v: '0x%02x' % v, 'hex8u': lambda v: '0x%08xu' % v}
SEPARATORS = (',', ', ', ',\n')
KINDS = ('vert', 'frag', 'comp')
STAGES = ('vertex', 'pixel')
SPIRV_MAGIC = 0x07230203
# a name in GLSL text (not the tail of a number such as 0x7fu or 1e5)
NAME = re.compile(r'(?<![A-Za-z0-9_.])[A-Za-z_][A-Za-z0-9_]*')
REVISION = re.compile(r'[a-z0-9][a-z0-9-]{0,39}')        # the name of a folder in tools/rpf7_inventory/history
# An earlier revision of a translator has modules of the same names as today's. It runs in a process of its own
# that sees its own folder and the standard library, and nothing else of this project.
RUNNER = '''import importlib, json, sys
folder, module, function = sys.argv[1:4]
sys.path.insert(0, folder)
request = json.load(sys.stdin)
arguments = [bytes.fromhex(a["bytes"]) if isinstance(a, dict) and set(a) == {"bytes"} else a for a in request["arguments"]]
options = {name: tuple(value) if isinstance(value, list) else value for name, value in request["options"].items()}
results = getattr(importlib.import_module(module), function)(*arguments, **options)
json.dump([r if isinstance(r, str) else None for r in (results if isinstance(results, (tuple, list)) else [results])], sys.stdout)
'''
# what the console's runtime writes over a vertex shader instruction it drops when the shader is loaded
NULL_INSTRUCTION = struct.pack('>3I', 0xC8000000, 0, 0x02000000)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def is_hash(value) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(c in '0123456789abcdef' for c in value)


def check_recipes(table) -> list[dict]:
    """Refuses a recipe table that could make the stage write anywhere else, call anything else, or leave a hole."""
    if not isinstance(table, dict) or table.get('schema') != SCHEMA or not isinstance(table.get('headers'), list) or not table['headers']:
        raise ValueError('recipe file refused: it needs the schema ' + SCHEMA + ' and a list of headers')
    seen = set()
    for header in table['headers']:
        path = header.get('path') if isinstance(header, dict) else None
        if not isinstance(path, str) or not path or path.startswith('/') or '\\' in path or '..' in path.split('/') \
                or not path.endswith(('.h', '.hpp')) or path in seen:
            raise ValueError('recipe refused: a header needs its own relative path ending in .h or .hpp')
        seen.add(path)
        if type(header.get('bytes')) is not int or header['bytes'] <= 0 or not is_hash(header.get('sha256')):
            raise ValueError(f'recipe {path}: the expected size and SHA-256 are missing')
        ids, argued = set(), set()
        for shader in header.get('shaders') or []:
            if not isinstance(shader, dict) or shader.get('stage') not in STAGES or not is_hash(shader.get('canonical_sha256')) \
                    or not is_hash(shader.get('sha256')) or not isinstance(shader.get('id'), str) or shader['id'] in ids \
                    or ('bindings' in shader and (shader['stage'] != 'vertex' or not isinstance(shader['bindings'], list))):
                raise ValueError(f'recipe {path}: a shader entry is refused')
            nulled = shader.get('nulled', [])
            if not isinstance(nulled, list) or any(type(n) is not int or n < 0 for n in nulled) or nulled != sorted(set(nulled)) \
                    or (nulled and shader['stage'] != 'vertex'):
                raise ValueError(f'recipe {path}: the nulled instruction slots of a shader are refused')
            ids.add(shader['id'])
        mids = set()
        for module in header.get('modules') or []:
            if not isinstance(module, dict) or ('call' in module) == ('source' in module):
                raise ValueError(f'recipe {path}: a module needs either a translator call or its own GLSL text')
            if 'source' in module:
                if not isinstance(module['source'], str) or '#version' not in module['source'] or 'result' in module:
                    raise ValueError(f'recipe {path}: the GLSL text of a hand-written module is refused')
                call = {'arguments': []}
            else:
                call = module['call']
                if not isinstance(call, dict) or not isinstance(call.get('module'), str) or not call['module'].isidentifier() \
                        or not isinstance(call.get('function'), str) or not call['function'].startswith('translate_') \
                        or not call['function'].isidentifier() or not isinstance(call.get('arguments'), list) \
                        or not isinstance(call.get('options', {}), dict) \
                        or ('revision' in call and (not isinstance(call['revision'], str) or not REVISION.fullmatch(call['revision']))):
                    raise ValueError(f'recipe {path}: a translator call is refused (only translate_* functions of tools/rpf7_inventory)')
                if type(module.get('result')) is not int or module['result'] < 0:
                    raise ValueError(f'recipe {path}: a module entry is refused')
            renames = module.get('rename', [])
            if not isinstance(renames, list) or ('rename' in module and ('source' in module or not renames)) \
                    or any(not isinstance(r, list) or len(r) != 2 or not all(isinstance(n, str) and NAME.fullmatch(n) for n in r)
                           or r[0] == r[1] for r in renames) \
                    or len({r[0] for r in renames}) != len(renames) or len({r[1] for r in renames}) != len(renames):
                raise ValueError(f'recipe {path}: the renames of a module are refused (pairs of differing names, no name twice on a side)')
            for argument in call['arguments']:
                if isinstance(argument, str) and argument.startswith('$'):
                    if argument[1:] not in ids:
                        raise ValueError(f'recipe {path}: a translator argument names a shader the header does not have')
                    argued.add(argument[1:])
            if module.get('kind') not in KINDS \
                    or not isinstance(module.get('glslc'), list) or not all(isinstance(f, str) and f.startswith('-') for f in module['glslc']) \
                    or any(f.startswith('-o') or f.startswith('-I') or '=' in f and not f.startswith('--target-env=') for f in module['glslc']) \
                    or not is_hash(module.get('sha256')) or not isinstance(module.get('id'), str) or module['id'] in mids:
                raise ValueError(f'recipe {path}: a module entry is refused')
            mids.add(module['id'])
        text = header.get('text')
        if not isinstance(text, list):
            raise ValueError(f'recipe {path}: the text must be a list of pieces')
        used = set()
        for piece in text:
            if isinstance(piece, str):
                continue
            if not isinstance(piece, dict) or piece.get('style') not in STYLES or piece.get('separator') not in SEPARATORS \
                    or len([k for k in ('bytes', 'words') if k in piece]) != 1 \
                    or ('per_line' in piece and (type(piece['per_line']) is not int or piece['per_line'] < 1)):
                raise ValueError(f'recipe {path}: a text piece is refused')
            name = piece.get('bytes', piece.get('words'))
            if ('bytes' in piece and name not in ids) or ('words' in piece and name not in mids):
                raise ValueError(f'recipe {path}: the text names a shader or module the header does not have')
            used.add(name)
        # a module must appear in the text; a shader must appear there or be what a module is translated from
        if not mids <= used or not ids <= used | argued:
            raise ValueError(f'recipe {path}: a shader or module is described but not used')
    return table['headers']


def shader_code(spec: dict, items: list[dict], read_code, bind) -> bytes:
    """The shader's bytes as the game has them: a disc container of the same canonical form and stage, with the
    fetch numbers applied for a vertex shader. Every candidate must agree; the result must have the named hash."""
    rows = [r for r in items if r.get('canonical_sha256') == spec['canonical_sha256'] and r.get('stage') == spec['stage']]
    if not rows:
        raise ValueError('a shader of the recipe is not in the disc inventory')
    results = set()
    for row in rows:
        code = read_code(row)
        if 'bindings' in spec:
            code = bind(code, spec['bindings'])
        for slot in spec.get('nulled', []):
            if slot * 12 + 12 > len(code):
                raise ValueError('a nulled instruction slot lies beyond the shader')
            code = code[:slot * 12] + NULL_INSTRUCTION + code[slot * 12 + 12:]
        results.add(code)
    if len(results) != 1:
        raise ValueError('the disc holds differing shaders of one canonical form: the recipe cannot choose')
    code = results.pop()
    if sha(code) != spec['sha256']:
        raise ValueError('a shader made from the disc does not have the hash the recipe names')
    return code


def renamed(text: str, pairs: list) -> str:
    """The translator's text with some of its own names changed: whole names, all at once (so two names can swap).
    Every name must be there, and none may be given a name that another variable keeps."""
    table = {old: new for old, new in pairs}
    present = set(NAME.findall(text))
    if not set(table) <= present:
        raise ValueError('a name the recipe renames is not in the translated shader')
    if any(new in present and new not in table for new in table.values()):
        raise ValueError('a rename would give two things of the translated shader one name')
    return NAME.sub(lambda m: table.get(m.group(0), m.group(0)), text)


def module_words(module: dict, shaders: dict[str, bytes], translate, compile_glsl) -> list[int]:
    """The SPIR-V words of one module: translator call, glslc, hash check."""
    if 'source' in module:
        text = module['source']
    else:
        call = module['call']
        arguments = [shaders[a[1:]] if isinstance(a, str) and a.startswith('$') else a for a in call['arguments']]
        # the recipe is JSON and has no tuples; the translators take their fixed sequences (formats, blend controls) as tuples
        options = {name: tuple(value) if isinstance(value, list) else value for name, value in call.get('options', {}).items()}
        results = translate(call['module'], call['function'], arguments, options, *([call['revision']] if 'revision' in call else []))
        if not isinstance(results, (tuple, list)) or module['result'] >= len(results) or not isinstance(results[module['result']], str):
            raise ValueError('a translator did not return the GLSL the recipe points at')
        text = results[module['result']]
        if module.get('rename'):
            text = renamed(text, module['rename'])
    data = compile_glsl(text, module['kind'], module['glslc'])
    if len(data) % 4 or len(data) < 20 or sha(data) != module['sha256']:
        raise ValueError('a SPIR-V module does not have the hash the recipe names')
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    if words[0] != SPIRV_MAGIC:
        raise ValueError('glslc did not write SPIR-V')
    return words


def render(header: dict, shaders: dict[str, bytes], modules: dict[str, list[int]]) -> str:
    out = []
    for piece in header['text']:
        if isinstance(piece, str):
            out.append(piece)
        else:
            values = shaders[piece['bytes']] if 'bytes' in piece else modules[piece['words']]
            numbers = [STYLES[piece['style']](v) for v in values]
            if 'per_line' in piece:      # so many numbers to a line; the separator also ends each line but the last
                count = piece['per_line']
                out.append((piece['separator'] + '\n').join(piece['separator'].join(numbers[i:i + count]) for i in range(0, len(numbers), count)))
            else:
                out.append(piece['separator'].join(numbers))
    return ''.join(out)


def build_header(header: dict, items: list[dict], read_code, bind, translate, compile_glsl) -> bytes:
    shaders = {s['id']: shader_code(s, items, read_code, bind) for s in header.get('shaders', [])}
    modules = {m['id']: module_words(m, shaders, translate, compile_glsl) for m in header.get('modules', [])}
    data = render(header, shaders, modules).encode('utf-8')
    if len(data) != header['bytes'] or sha(data) != header['sha256']:
        raise ValueError('the header made from the discs is not the header the recipe names')
    return data


class Tools:
    """The real translator, compiler and container reader (the tests hand in stand-ins instead)."""

    def __init__(self, inventory: Path, glslc: Path):
        sys.path[:0] = [str(TRANSLATORS), str(ROOT / 'tools' / 'iter'), str(HERE)]
        import build_boot_shaders
        import generic_shader_pack
        self.inventory, self.glslc = inventory, glslc
        self.bind = build_boot_shaders.bind_vertex
        self.parse = generic_shader_pack.parse_container
        self.codes, self.calls, self.modules = {}, {}, {}
        self.tmp = tempfile.TemporaryDirectory()

    def read_code(self, row: dict) -> bytes:
        key = row['file']
        if key not in self.codes:
            path = (self.inventory / key).resolve()
            if not path.is_relative_to(self.inventory.resolve()):
                raise ValueError('inventory path refused')
            self.codes[key] = self.parse(path.read_bytes(), row['stage'])[0]
        return self.codes[key]

    def translate(self, module: str, function: str, arguments: list, options: dict, revision: str | None = None):
        key = (revision, module, function, json.dumps([a.hex() if isinstance(a, bytes) else a for a in arguments], sort_keys=True),
               json.dumps(options, sort_keys=True))
        if key not in self.calls:
            folder = TRANSLATORS if revision is None else TRANSLATORS / 'history' / revision
            if not (folder / (module + '.py')).is_file():
                raise ValueError('the recipe names a translator that is not in tools/rpf7_inventory')
            if revision is None:
                self.calls[key] = getattr(importlib.import_module(module), function)(*arguments, **options)
            else:
                self.calls[key] = earlier_revision(folder, module, function, arguments, options)
        return self.calls[key]

    def compile_glsl(self, text: str, kind: str, flags: list[str]) -> bytes:
        key = (sha(text.encode('utf-8')), kind, tuple(flags))
        if key not in self.modules:
            source = Path(self.tmp.name) / ('shader.' + kind)
            target = Path(self.tmp.name) / ('shader.' + kind + '.spv')
            source.write_text(text, encoding='utf-8', newline='\n')
            r = subprocess.run([str(self.glslc), *flags, '-o', str(target), str(source)], capture_output=True, text=True)
            if r.returncode != 0:
                raise ValueError('glslc refused a translated shader: ' + r.stderr.strip()[:300])
            self.modules[key] = target.read_bytes()
        return self.modules[key]


def earlier_revision(folder: Path, module: str, function: str, arguments: list, options: dict) -> tuple:
    """What an earlier revision of a translator returns: its texts, and None in the place of anything else."""
    request = json.dumps({'arguments': [{'bytes': a.hex()} if isinstance(a, bytes) else a for a in arguments], 'options': options})
    # -I: no PYTHONPATH, no user site, not the current folder; so a module the revision lacks is not taken from today's
    r = subprocess.run([sys.executable, '-I', '-B', '-c', RUNNER, str(folder), module, function], input=request, capture_output=True, text=True)
    if r.returncode != 0:
        raise ValueError('an earlier translator revision failed: ' + r.stderr.strip()[-300:])
    return tuple(json.loads(r.stdout))


def build(headers: list[dict], items: list[dict], tools, output: Path, only: str | None = None) -> dict:
    """Writes the headers into a new folder with report.json. status is 'differs' when a header could not be made."""
    output.mkdir(parents=True)
    report = {'schema': 1, 'status': 'complete', 'headers': {}}
    for header in headers:
        if only and only not in header['path']:
            continue
        try:
            data = build_header(header, items, tools.read_code, tools.bind, tools.translate, tools.compile_glsl)
        except ValueError as error:
            report['status'] = 'differs'
            report['headers'][header['path']] = {'status': 'refused', 'why': str(error)}
            continue
        target = output / header['path']
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        report['headers'][header['path']] = {'status': 'written', 'bytes': len(data), 'sha256': sha(data),
                                             'shaders': len(header.get('shaders', [])), 'modules': len(header.get('modules', []))}
    (output / 'report.json').write_text(json.dumps(report, indent=1, sort_keys=True) + '\n', encoding='utf-8', newline='\n')
    return report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--inventory', type=Path, required=True)
    ap.add_argument('--recipes', type=Path, required=True)
    ap.add_argument('--glslc', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--only')
    a = ap.parse_args()
    headers = check_recipes(json.loads(a.recipes.read_text(encoding='utf-8')))
    inventory = json.loads((a.inventory / 'inventory.json').read_text(encoding='utf-8'))
    items = inventory.get('containers')
    if not isinstance(items, list) or not items:
        raise ValueError('inventory refused: it lists no shader containers')
    if not a.glslc.is_file():
        raise ValueError('glslc not found')
    report = build(headers, items, Tools(a.inventory, a.glslc), a.output.absolute(), a.only)
    written = [h for h in report['headers'].values() if h['status'] == 'written']
    for path, h in report['headers'].items():
        if h['status'] != 'written':
            print('NATIVE_PACK_REFUSED', path, '-', h['why'], flush=True)
    print('NATIVE_PACKS', report['status'], f'{len(written)} of {len(report["headers"])} headers written,',
          sum(h['bytes'] for h in written), 'bytes', flush=True)
    return 0 if report['status'] == 'complete' else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, OSError) as error:
        print('NATIVE_PACKS_REFUSED:', error, file=sys.stderr)
        sys.exit(2)
