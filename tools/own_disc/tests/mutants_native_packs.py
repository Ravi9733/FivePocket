#!/usr/bin/env python3
"""Mutant check for build_native_packs.py: each mutant is a copy of the tool with one rule removed or changed;
test_build_native_packs.py (beside this file) must fail on it. First a control on an unchanged copy. Exits 1 if the
control fails, a mutant survives, or a mutant no longer applies. The tool itself is not touched.

  mutants_native_packs.py [--only TEXT]
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent / 'build_native_packs.py'
TESTS = HERE / 'test_build_native_packs.py'

MUTANTS = [
    # ---- the shader from the disc
    ('a container of any stage is taken', " and r.get('stage') == spec['stage']]", "]"),
    ('the fetch numbers are not applied', "        if 'bindings' in spec:\n            code = bind(code, spec['bindings'])\n", ""),
    ('the shader made from the disc is not checked', "    if sha(code) != spec['sha256']:\n        raise ValueError('a shader made from the disc does not have the hash the recipe names')\n", ""),
    ('differing disc shaders: any one is taken', "    if len(results) != 1:\n        raise ValueError('the disc holds differing shaders of one canonical form: the recipe cannot choose')\n", ""),
    ('a shader missing from the disc is not reported as such', "    if not rows:\n        raise ValueError('a shader of the recipe is not in the disc inventory')\n", ""),
    ('the runtime\'s null instruction is not written', "            code = code[:slot * 12] + NULL_INSTRUCTION + code[slot * 12 + 12:]\n", "            pass\n"),
    ('the null instruction has another value', "NULL_INSTRUCTION = struct.pack('>3I', 0xC8000000, 0, 0x02000000)", "NULL_INSTRUCTION = struct.pack('>3I', 0xC8000000, 0, 0)"),
    ('a nulled slot beyond the shader lengthens it', "            if slot * 12 + 12 > len(code):\n                raise ValueError('a nulled instruction slot lies beyond the shader')\n", ""),
    ('nulled slots may come in any order or twice', " or nulled != sorted(set(nulled)) \\\n", " \\\n"),
    ('a nulled slot may be any value', "any(type(n) is not int or n < 0 for n in nulled)", "False"),
    ('a pixel shader may have nulled slots', "                    or (nulled and shader['stage'] != 'vertex'):", "                    or False:"),
    ('nulled slots need not be a list', "            if not isinstance(nulled, list) or any(", "            if any("),
    # ---- numbers per line; names put back
    ('the numbers per line are ignored', "            if 'per_line' in piece:", '            if False:'),
    ('lines are not broken', "out.append((piece['separator'] + '\\n').join(", "out.append((piece['separator']).join("),
    ('a line holds one number more', "piece['separator'].join(numbers[i:i + count]) for i in range(0, len(numbers), count)", "piece['separator'].join(numbers[i:i + count + 1]) for i in range(0, len(numbers), count + 1)"),
    ('a line ends without the separator', "out.append((piece['separator'] + '\\n').join(", "out.append('\\n'.join("),
    ('any value is taken as the numbers per line', " \\\n                    or ('per_line' in piece and (type(piece['per_line']) is not int or piece['per_line'] < 1)):", ':'),
    ('zero numbers per line are accepted', "piece['per_line'] < 1)):", "piece['per_line'] < 0)):"),
    ('a true is taken as one number per line', "(type(piece['per_line']) is not int or", "(not isinstance(piece['per_line'], int) or"),
    ('the renames are not applied', "        if module.get('rename'):\n            text = renamed(text, module['rename'])\n", ''),
    ('names are not changed', '    return NAME.sub(lambda m: table.get(m.group(0), m.group(0)), text)', '    return text'),
    ('a name that is not there is passed over', "    if not set(table) <= present:\n        raise ValueError('a name the recipe renames is not in the translated shader')\n", ''),
    ('a rename may give two things one name', "    if any(new in present and new not in table for new in table.values()):\n        raise ValueError('a rename would give two things of the translated shader one name')\n", ''),
    ('a swap counts as giving two things one name', '    if any(new in present and new not in table for new in table.values()):', '    if any(new in present for new in table.values()):'),
    ('the tail of a number counts as a name', "NAME = re.compile(r'(?<![A-Za-z0-9_.])[A-Za-z_][A-Za-z0-9_]*')", "NAME = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')"),
    ('what follows a dot counts as a name', "NAME = re.compile(r'(?<![A-Za-z0-9_.])[A-Za-z_][A-Za-z0-9_]*')", "NAME = re.compile(r'(?<![A-Za-z0-9_])[A-Za-z_][A-Za-z0-9_]*')"),
    ('names are changed one after the other', '    return NAME.sub(lambda m: table.get(m.group(0), m.group(0)), text)', '    for old, new in pairs:\n        text = NAME.sub(lambda m: new if m.group(0) == old else m.group(0), text)\n    return text'),
    ('a hand-written module may carry renames', "('rename' in module and ('source' in module or not renames))", "('rename' in module and not renames)"),
    ('an empty list of renames is accepted', "('rename' in module and ('source' in module or not renames))", "('rename' in module and 'source' in module)"),
    ('a rename need not be a pair', 'not isinstance(r, list) or len(r) != 2 or not all(', 'not isinstance(r, list) or not all('),
    ('a rename need not be a list', 'or any(not isinstance(r, list) or len(r) != 2 or', 'or any(len(r) != 2 or'),
    ('a rename may be of anything', 'not all(isinstance(n, str) and NAME.fullmatch(n) for n in r)', 'False'),
    ('a rename to the same name is accepted', '\n                           or r[0] == r[1] for r in renames)', ' for r in renames)'),
    ('one name may be renamed two ways', '                    or len({r[0] for r in renames}) != len(renames) or len({r[1]', '                    or len({r[1]'),
    ('two names may be renamed to one', ' or len({r[1] for r in renames}) != len(renames):', ':'),
    ('the renames need not be a list', "            if not isinstance(renames, list) or ('rename' in module", "            if ('rename' in module"),
    ('a list among the options stays a list', "tuple(value) if isinstance(value, list) else value for name, value in call.get('options', {}).items()}", "value for name, value in call.get('options', {}).items()}"),
    ('every option is made a tuple', "options = {name: tuple(value) if isinstance(value, list) else value for name, value in call.get('options', {}).items()}", "options = {name: tuple(value) if isinstance(value, (list, str)) else value for name, value in call.get('options', {}).items()}"),
    ('the options of the recipe are not handed over', "results = translate(call['module'], call['function'], arguments, options, *(", "results = translate(call['module'], call['function'], arguments, {}, *("),
    # ---- an earlier revision of a translator
    ('any text is accepted as a revision', " \\\n                        or ('revision' in call and (not isinstance(call['revision'], str) or not REVISION.fullmatch(call['revision']))):", ':'),
    ('a revision may be a path', "REVISION = re.compile(r'[a-z0-9][a-z0-9-]{0,39}')", "REVISION = re.compile(r'.*')"),
    ('a revision name may be of any length', "REVISION = re.compile(r'[a-z0-9][a-z0-9-]{0,39}')", "REVISION = re.compile(r'[a-z0-9][a-z0-9-]*')"),
    ('a revision name may begin with a dash', "REVISION = re.compile(r'[a-z0-9][a-z0-9-]{0,39}')", "REVISION = re.compile(r'[a-z0-9-]{1,40}')"),
    ('a revision of forty letters is refused', "REVISION = re.compile(r'[a-z0-9][a-z0-9-]{0,39}')", "REVISION = re.compile(r'[a-z0-9][a-z0-9-]{0,38}')"),
    ('a revision need not be text', "(not isinstance(call['revision'], str) or not REVISION.fullmatch(call['revision']))", "(not REVISION.fullmatch(str(call['revision'])))"),
    ('the revision a call names is not handed on', ", *([call['revision']] if 'revision' in call else []))", ')'),
    ('every call is handed a revision', ", *([call['revision']] if 'revision' in call else []))", ", call.get('revision'), None)"),
    ('a revision is looked for among the translators of today', "            folder = TRANSLATORS if revision is None else TRANSLATORS / 'history' / revision\n", '            folder = TRANSLATORS\n'),
    ('a revision is run in this process with the modules of today', '            if revision is None:\n                self.calls[key] = getattr(importlib.import_module(module), function)(*arguments, **options)\n            else:\n                self.calls[key] = earlier_revision(folder, module, function, arguments, options)\n', '            self.calls[key] = getattr(importlib.import_module(module), function)(*arguments, **options)\n'),
    ('the result of today is given for a revision (one cache for both)', '        key = (revision, module, function, json.dumps(', '        key = (module, function, json.dumps('),
    ('the revision sees the environment of the caller', "[sys.executable, '-I', '-B', '-c', RUNNER,", "[sys.executable, '-B', '-c', RUNNER,"),
    ('a revision that failed is not noticed', "    if r.returncode != 0:\n        raise ValueError('an earlier translator revision failed: ' + r.stderr.strip()[-300:])\n", ''),
    ('the reason a revision failed is not passed on', "raise ValueError('an earlier translator revision failed: ' + r.stderr.strip()[-300:])", "raise ValueError('an earlier translator revision failed')"),
    ('the revision gets its shader bytes as text', 'arguments = [bytes.fromhex(a["bytes"]) if isinstance(a, dict) and set(a) == {"bytes"} else a for a in request["arguments"]]', 'arguments = [a["bytes"] if isinstance(a, dict) and set(a) == {"bytes"} else a for a in request["arguments"]]'),
    ('the revision gets its sequences as lists', 'options = {name: tuple(value) if isinstance(value, list) else value for name, value in request["options"].items()}', 'options = request["options"]'),
    ('the revision gets its list arguments as tuples', 'arguments = [bytes.fromhex(a["bytes"]) if isinstance(a, dict) and set(a) == {"bytes"} else a for a in request["arguments"]]', 'arguments = [bytes.fromhex(a["bytes"]) if isinstance(a, dict) and set(a) == {"bytes"} else tuple(a) for a in request["arguments"]]'),
    ('what a revision returns besides text is passed on', 'json.dump([r if isinstance(r, str) else None for r in (results', 'json.dump([r if isinstance(r, (str, dict)) else None for r in (results'),
    ('a single text from a revision is taken letter by letter', '(results if isinstance(results, (tuple, list)) else [results])], sys.stdout)', '(results)], sys.stdout)'),
    ("the revision finds today's modules before its own", 'sys.path.insert(0, folder)\n', 'sys.path.insert(0, folder + "/../..")\nsys.path.append(folder)\n'),
    # ---- the module
    ('a hand-written module is sent to the translator', "    if 'source' in module:\n        text = module['source']\n    else:\n", "    if False:\n        text = module['source']\n    else:\n"),
    ('a module may have both a call and a text, or neither', "            if not isinstance(module, dict) or ('call' in module) == ('source' in module):", "            if not isinstance(module, dict):"),
    ('any text is accepted as a hand-written module', "or '#version' not in module['source'] or 'result' in module:", "or 'result' in module:"),
    ('a hand-written module may carry a result index', "or '#version' not in module['source'] or 'result' in module:", "or '#version' not in module['source']:"),
    ('the text of a hand-written module may be anything but a string', "                if not isinstance(module['source'], str) or '#version'", "                if '#version'"),
    ('the module is not checked against its hash', " or sha(data) != module['sha256']:", ":"),
    ('a result index beyond the translator\'s results is accepted', "module['result'] >= len(results) or ", ""),
    ('a result that is not text is accepted', " or not isinstance(results[module['result']], str):", ":"),
    ('what glslc wrote need not be SPIR-V', "    if words[0] != SPIRV_MAGIC:\n        raise ValueError('glslc did not write SPIR-V')\n", ""),
    ('a module of a length that is not whole words is accepted', "    if len(data) % 4 or len(data) < 20 or", "    if len(data) < 20 or"),
    ('the shader bytes are not handed to the translator', "arguments = [shaders[a[1:]] if isinstance(a, str) and a.startswith('$') else a for a in call['arguments']]", "arguments = list(call['arguments'])"),
    # ---- the text
    ('the upper-case style writes lower case', "'hexu': lambda v: '0x%Xu' % v,", "'hexu': lambda v: '0x%xu' % v,"),
    ('the two-digit style writes one digit for small numbers', "'hex2': lambda v: '0x%02x' % v,", "'hex2': lambda v: '0x%x' % v,"),
    ('the eight-digit style writes fewer digits', "'hex8u': lambda v: '0x%08xu' % v}", "'hex8u': lambda v: '0x%xu' % v}"),
    ('the eight-digit style leaves the u out', "'hex8u': lambda v: '0x%08xu' % v}", "'hex8u': lambda v: '0x%08x' % v}"),
    ('every array is joined with a plain comma', "                out.append(piece['separator'].join(numbers))", "                out.append(','.join(numbers))"),
    ('the header is not compared with its hash', " or sha(data) != header['sha256']:", ":"),
    ('the header is not compared with its size', "    if len(data) != header['bytes'] or ", "    if "),
    # ---- the recipe file
    ('an absolute header path is accepted', "or path.startswith('/') or '\\\\' in path", "or '\\\\' in path"),
    ('a header path with a backslash is accepted', "or path.startswith('/') or '\\\\' in path", "or path.startswith('/')"),
    ('a header path may climb out with ..', " or '..' in path.split('/') \\\n", " \\\n"),
    ('any file name is accepted for a header', "                or not path.endswith(('.h', '.hpp')) or path in seen:", "                or path in seen:"),
    ('two recipes may write one path', "or not path.endswith(('.h', '.hpp')) or path in seen:", "or not path.endswith(('.h', '.hpp')):"),
    ('the schema is not checked', "or table.get('schema') != SCHEMA ", ""),
    ('the expected hash may be missing', " or not is_hash(header.get('sha256')):", ":"),
    ('the expected size may be zero', " or header['bytes'] <= 0", ""),
    ('any function may be called', " or not call['function'].startswith('translate_') \\\n", " \\\n"),
    ('a module name may be a path', " or not call['module'].isidentifier() \\\n", " \\\n"),
    ('arguments need not be a list', " or not isinstance(call.get('arguments'), list) \\\n", " \\\n"),
    ('an argument may name an unknown shader', "                    if argument[1:] not in ids:\n                        raise ValueError(f'recipe {path}: a translator argument names a shader the header does not have')\n", ""),
    ('glslc may be told where to write', "any(f.startswith('-o') or f.startswith('-I') or", "any(f.startswith('-I') or"),
    ('glslc may be given include folders', "any(f.startswith('-o') or f.startswith('-I') or", "any(f.startswith('-o') or"),
    ('glslc may be given any option with a value', " or '=' in f and not f.startswith('--target-env=') for f in module['glslc'])", " for f in module['glslc'])"),
    ('a glslc word need not be an option', "not all(isinstance(f, str) and f.startswith('-') for f in module['glslc'])", "not all(isinstance(f, str) for f in module['glslc'])"),
    ('a pixel shader may carry fetch numbers', "('bindings' in shader and (shader['stage'] != 'vertex' or not isinstance(shader['bindings'], list)))", "('bindings' in shader and not isinstance(shader['bindings'], list))"),
    ('two shaders may share an id', " or shader['id'] in ids \\\n", " \\\n"),
    ('two modules may share an id', " or module['id'] in mids:", ":"),
    ('a stage of any name is accepted', "shader.get('stage') not in STAGES or ", ""),
    ('a module of any kind is accepted', "            if module.get('kind') not in KINDS \\\n", "            if False \\\n"),
    ('a negative result index is accepted', "                if type(module.get('result')) is not int or module['result'] < 0:", "                if type(module.get('result')) is not int:"),
    ('an unknown number style is accepted', "piece.get('style') not in STYLES or ", ""),
    ('an unknown separator is accepted', " or piece.get('separator') not in SEPARATORS \\\n", " \\\n"),
    ('the text may name what the header does not have', "            if ('bytes' in piece and name not in ids) or ('words' in piece and name not in mids):", "            if False:"),
    ('a described shader may stay unused', "        if not mids <= used or not ids <= used | argued:", "        if not mids <= used:"),
    ('a described module may stay unwritten', "        if not mids <= used or not ids <= used | argued:", "        if not ids <= used | argued:"),
    ('a shader that is only translated from counts as unused', "        if not mids <= used or not ids <= used | argued:", "        if not mids <= used or not ids <= used:"),
    ('the text need not be a list of pieces', "        if not isinstance(text, list):", "        if False:"),
    # ---- writing
    ('a refused header does not fail the build', "            report['status'] = 'differs'\n", ""),
    ('a refused header is written all the same', "            report['headers'][header['path']] = {'status': 'refused', 'why': str(error)}\n            continue\n", "            report['headers'][header['path']] = {'status': 'refused', 'why': str(error)}\n            data = b''\n"),
    ('--only is ignored', "        if only and only not in header['path']:\n            continue\n", ""),
    ('an existing output folder is written into', "    output.mkdir(parents=True)\n", "    output.mkdir(parents=True, exist_ok=True)\n"),
    ('the exit code is 0 whatever happened', "    return 0 if report['status'] == 'complete' else 1", "    return 0"),
    ('an inventory without containers is accepted', "    if not isinstance(items, list) or not items:\n        raise ValueError('inventory refused: it lists no shader containers')\n", ""),
    ('a missing glslc is not reported', "    if not a.glslc.is_file():\n        raise ValueError('glslc not found')\n", ""),
    # ---- the real tools
    ('a translator module from anywhere is imported', "            if not (folder / (module + '.py')).is_file():\n                raise ValueError('the recipe names a translator that is not in tools/rpf7_inventory')\n", ''),
    ('a container outside the inventory is read', "            if not path.is_relative_to(self.inventory.resolve()):\n                raise ValueError('inventory path refused')\n", ""),
]


def run(tool: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, '-B', str(TESTS), '-v', '--failfast'], env=dict(os.environ, BUILD_NATIVE_PACKS=str(tool)),
                          capture_output=True, text=True, timeout=300)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--only')
    a = ap.parse_args()
    source = TOOL.read_text()
    chosen = [(i, m) for i, m in enumerate(MUTANTS, 1) if not a.only or a.only in m[0]]
    killed = 0
    # A copy has to lie in the tool's own folder: the tool finds the translators and its sister stages from where it is.
    copy = TOOL.with_name(f'_mutant_native_packs_{os.getpid()}.py')
    try:
        copy.write_text(source)
        r = run(copy)
        if r.returncode != 0:
            print('CONTROL FAILED: the tests do not pass on an unchanged copy\n' + r.stderr[-2000:])
            return 1
        print('control: the tests pass on an unchanged copy of the tool', flush=True)
        for index, (what, old, new) in chosen:
            if source.count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({source.count(old)} matches): {what}')
                continue
            copy.write_text(source.replace(old, new))
            r = run(copy)
            failed = [line.split(' ... ')[0].strip() for line in r.stderr.splitlines() if line.rstrip().endswith(('... FAIL', '... ERROR'))]
            if r.returncode == 0:
                print(f'mutant {index:3d} SURVIVED: {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed by {failed[0].split(" (")[0] if failed else "the test run (it could not start)"}: {what}')
    finally:
        copy.unlink(missing_ok=True)
    print(f'{killed} of {len(chosen)} mutants killed')
    return 0 if killed == len(chosen) else 1


if __name__ == '__main__':
    sys.exit(main())
