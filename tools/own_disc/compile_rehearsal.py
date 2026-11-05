#!/usr/bin/env python3
"""The builder's heavy stages, each consuming what the stage before it produced in the same work folder:

  codegen   the generator on the executable extracted from the disc
  recipe    the recorded passes on that generated code, checked chunk by chunk against the recipe's hashes
  compile   every translation unit from this run's recipe, the shader tables from this run's shader stage,
            a private object cache, then the link; with --portable-build-id the link drops debug sections and takes
            that build id, and the compiler's name strings are removed, so the library is the same file on any host
            and in any folder (shown for macOS and Windows on 2026-10-07, DISCOVERIES)

Uses the generator, the recipe passes and tools/iter/iterate.py's build unchanged. No device or network API. Writes
only under the work folder (which must lie in local-analysis/builder). From the builder-rehearsal and builder-shaders
lanes of 2026-10-07; the portable link was added by the lead.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True


def sha(p):
    h = hashlib.sha256()
    with Path(p).open('rb') as f:
        for block in iter(lambda: f.read(1048576), b''):
            h.update(block)
    return h.hexdigest()


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    obj = importlib.util.module_from_spec(spec)
    sys.modules[name] = obj
    spec.loader.exec_module(obj)
    return obj


def write_json(p, data):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8', newline='\n')


def clone(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if sys.platform == 'darwin':
        subprocess.run(['/bin/cp', '-c', str(src), str(dst)], check=True)     # an APFS clone: no bytes are copied
    else:
        shutil.copyfile(src, dst)                                               # a host without clones: the same bytes, copied


def generator_config(text, original_dir, xex):
    """Change the actual producer input, preserving the recorded switch tables."""
    out = []
    seen = set()
    for line in text.splitlines():
        m = re.fullmatch(r'(file_path|switch_table_file_path) = "(.*)"', line)
        if m:
            name, old = m.groups()
            seen.add(name)
            target = xex if name == 'file_path' else (original_dir / old).resolve()
            line = f'{name} = {json.dumps(str(target))}'
        out.append(line)
    if seen != {'file_path', 'switch_table_file_path'}:
        raise ValueError('configuration lacks exact input path fields')
    return '\n'.join(out) + '\n'


def connect_recipe(recipe, root, work):
    out = copy.deepcopy(recipe)
    out['target'] = 'v507'
    out['corpus'] = str(work / 'recipe/corpus')
    out['inputs']['image']['path'] = str(work / 'decode/default.image.bin')
    out['generator_base']['dir'] = str(work / 'codegen/ppc')
    for c in out['chunks'].values():
        base = c['base']
        parent = work / ('codegen/ppc' if base['layer'] == 'generator+passes' else 'recipe/corpus')
        base['path'] = str(parent / Path(base['path']).name)
        if 'corpus' in base:
            base['corpus']['path'] = str(work / 'recipe/corpus' / Path(base['corpus']['path']).name)
    return out


PORTABLE_ID = re.compile(r'[0-9a-f]{40}')


def portable_link_words(build_id):
    """Link words that make the library independent of host and folder: no debug sections, a fixed build id."""
    if not PORTABLE_ID.fullmatch(build_id or ''):
        raise ValueError('the portable build id must be 40 lower-case hex digits (the size of the base link\'s sha1 id)')
    return ['-Wl,--strip-debug', '-Wl,--build-id=0x' + build_id]


def pack_include_words(command, root, packs):
    """The words that put the native-packs stage's folders first on a unit's include path.

    The runtime finds a shader pack header through include folders of the private data folder
    (-I<root>/local-analysis/<folder>). The native-packs stage writes its headers under the same folder names. For
    every such include folder of the command that the stage's output has a counterpart for, one word naming the
    counterpart, in the command's order and once each. Folders the stage did not write stay as they are."""
    prefix = str(Path(root) / 'local-analysis') + '/'
    words = []
    for word in command:
        if word.startswith('-I') and word[2:].startswith(prefix):
            mirror = Path(packs) / word[2:][len(prefix):]
            if mirror.is_dir() and '-I' + str(mirror) not in words:
                words.append('-I' + str(mirror))
    return words


def connected_spec(spec, attempt, chunks, pinned, link_words=(), first_includes=None, generated=None):
    out = copy.deepcopy(spec)
    out['build_jobs'] = 4
    out['link_extra'] = list(out.get('link_extra', [])) + list(link_words)
    if generated is not None:
        # game code chunks include the generator's headers by name in quotes: a quote folder is searched before the
        # plan's include folders, so this run's copies are found before the old corpus folder's
        out['ppc_defines'] = list(out.get('ppc_defines', [])) + ['-iquote', str(generated)]
    for f in out['files']:
        if 'ppc_chunk' in f:
            if f.get('source'):
                f['source'] = str(pinned / f['name'])
            else:
                f.pop('source', None)
        elif first_includes and first_includes.get(f['name']):
            # the builder puts a file's "defines" directly behind the compiler's name, ahead of the plan's own words
            f['defines'] = list(f.get('defines', [])) + list(first_includes[f['name']])
    return out


REDIRECT_HEAD = '/* code-reach: bind unhooked generated weak wrappers to their bodies (layout-only, see redirect_script.py) */\n'
WRAPPER = re.compile(r'_Z(\d+)([A-Za-z0-9_]+)R10PPCContextPh')


def redirect_script(nm, objects):
    """The linker script that binds every generated weak wrapper to its body, made from a build's own objects.

    The rule of tools/fp_codegen/codereach (weak_census.py, redirect_script.py): a wrapper is a weak definition
    `name(ctx, base)` whose first definer in name order is a game code object; it is bound to `__imp__name` unless some
    object defines the same name strongly (a hook of the runtime: that one keeps its binding). Returns the text and
    the number of hooked wrappers left alone."""
    weak, strong = {}, set()
    for obj in sorted(objects, key=lambda p: p.name):
        done = subprocess.run([str(nm), '--defined-only', str(obj)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if done.returncode != 0:
            raise ValueError('nm failed on ' + obj.name + ': ' + done.stdout[-300:])
        for line in done.stdout.splitlines():
            fields = line.split()
            if len(fields) != 3:
                continue
            if fields[1] == 'W':
                weak.setdefault(fields[2], obj.name)
            elif fields[1] == 'T':
                strong.add(fields[2])
    lines, hooked = [], 0
    for name, owner in sorted(weak.items()):
        m = WRAPPER.fullmatch(name)
        if not m or not owner.startswith('ppc_recomp.') or len(m.group(2)) != int(m.group(1)):
            continue
        if name in strong:
            hooked += 1
            continue
        lines.append(f'{name} = __imp__{m.group(2)};')
    return REDIRECT_HEAD + '\n'.join(lines) + '\n', hooked


def without_script(spec, script):
    out = copy.deepcopy(spec)
    out['link_extra'] = [w for w in out.get('link_extra', []) if w != script]
    return out


def with_script(spec, script, made):
    out = copy.deepcopy(spec)
    out['link_extra'] = [str(made) if w == script else w for w in out.get('link_extra', [])]
    return out


PROFILE_WORDS = ('-fprofile-use=', '-fprofile-instr-use=', '-fprofile-sample-use=')     # name a profile file
PROFILE_COMPANIONS = ('-Wno-profile-instr-unprofiled', '-Wno-profile-instr-out-of-date')  # only mean something with one
ORDER_WORDS = ('-Wl,--symbol-ordering-file=',)
ORDER_COMPANIONS = ('-Wl,--no-warn-symbol-ordering',)


def without_measurements(spec):
    """The attempt without what was measured on an earlier build of the game: the optimisation profile (among the
    extra flags of the chunks and of the single files that use it) and the symbol order made from the same
    measurements (among the link words). Every other word stays, in its place. Returns the attempt and the words that
    named the two files.

    Afterwards no text anywhere in the attempt may still name a profile or a symbol order: a place this function
    does not know would leave one in, and the result would not be what its name says (seen 2026-10-08: nineteen
    runtime files carry the profile in a key of their own)."""
    out = copy.deepcopy(spec)
    named = {'profile': [], 'symbol_order': []}

    def strip(words, names, companions, into):
        kept = []
        for word in words:
            if isinstance(word, str) and word.startswith(names):
                if word not in into:
                    into.append(word)
            elif word not in companions:
                kept.append(word)
        return kept
    extra = out.get('ppc_chunk_extra')
    if extra and 'flags' in extra:
        extra['flags'] = strip(extra['flags'], PROFILE_WORDS, PROFILE_COMPANIONS, named['profile'])
    for f in out.get('files', []):
        if 'extra_flags' in f:
            f['extra_flags'] = strip(f['extra_flags'], PROFILE_WORDS, PROFILE_COMPANIONS, named['profile'])
    out['link_extra'] = strip(out.get('link_extra', []), ORDER_WORDS, ORDER_COMPANIONS, named['symbol_order'])

    def texts(value):
        if isinstance(value, str):
            yield value
        elif isinstance(value, dict):
            for item in value.values():
                yield from texts(item)
        elif isinstance(value, list):
            for item in value:
                yield from texts(item)
    left = [t for t in texts(out) if t.startswith(PROFILE_WORDS + ORDER_WORDS)]
    if left:
        raise ValueError('the attempt still names a measurement of an earlier build in a place this stage does not know: ' + left[0][:60])
    return out, named


DECODER_HEADERS = 'local-analysis/native-xma/ffmpeg'      # the private copy of FFmpeg's public headers the plans name


def kept_runtime_files(root, spec):
    """The runtime files of the base plan that the attempt does not build: their objects were kept from the base
    build of September and linked as they were. Returns (name, source relative to the repository, kept object) each.
    The mapping unit is not among them (the stage compiles it from this run's generated code)."""
    cfg = json.loads((root / 'local-analysis/attempts/config.json').read_text())
    plan = json.loads((root / cfg['base_build'] / 'plan.json').read_text())
    built = {f.get('template', f['name']) for f in spec['files'] if 'ppc_chunk' not in f}
    kept = []
    for item in plan['plans']:
        source = Path(item['source'])
        if source.name in built or source.name == 'ppc_func_mapping.cpp':
            continue
        if not source.is_absolute() or not source.is_relative_to(root) or source.is_relative_to(root / 'local-analysis'):
            raise ValueError('a kept runtime file is not a source of the repository: ' + source.name)
        kept.append((source.name, str(source.relative_to(root)), str(root / item['output'])))
    return kept


XMA_ARCHIVES = ('libavcodec.a', 'libavutil.a')     # the audio decoder: FFmpeg, built by build_xma_decoder.sh


def with_archives(link, folder):
    """The link plan with this run's audio decoder archives in the place of the supplied ones.

    `folder` is the build folder of tools/host_guest_trace/build_xma_decoder.sh (libavcodec/libavcodec.a,
    libavutil/libavutil.a). Each archive must be the one the plan is pinned to; then only its path changes.
    Returns the plan and, per archive, what replaced what."""
    out, record, words = copy.deepcopy(link), {}, {}
    for name in XMA_ARCHIVES:        # the plan is looked at as a whole before any file is
        old = [w for w in out['command'] if not w.startswith('-') and Path(w).name == name]
        if len(old) != 1 or old[0] not in out['inputs']:
            raise ValueError('the link plan must name ' + name + ' exactly once, with its hash')
        words[name] = old[0]
    for name, old in words.items():
        made = Path(folder) / name[:-2] / name
        if not made.is_file():
            raise ValueError('this run has no ' + name + ': the decoder stage did not finish')
        require_hash(made, out['inputs'][old])
        out['command'] = [str(made) if w == old else w for w in out['command']]
        out['inputs'][str(made)] = out['inputs'].pop(old)
        record[name] = {'made': str(made), 'sha256': out['inputs'][str(made)], 'replaces_link_word': old}
    return out, record


def require_hash(path, wanted):
    if sha(path) != wanted:
        raise ValueError('input/output hash differs: ' + Path(path).name)


GENERATOR = 'local-analysis/xenonrecomp-build/build/XenonRecomp/XenonRecomp'      # this Mac's build of the generator
GENERATOR_CONTEXT = 'local-analysis/xenonrecomp-build/src/XenonUtils/ppc_context.h'


def run_codegen(root, work, generator=None, context=None):
    """generator, context: another host's build of the generator and the header it ships with (default: this Mac's)."""
    cfg = root / 'local-analysis/regen/flags-off/recomp.toml'
    dst = work / 'codegen-input/recomp.toml'
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(generator_config(cfg.read_text(), cfg.parent, work / 'input/default.xex'), encoding='utf-8', newline='\n')
    subprocess.run([sys.executable, '-B', str(root / 'tools/own_disc/regen_check.py'),
                    '--generator', str(generator or root / GENERATOR),
                    '--context', str(context or root / GENERATOR_CONTEXT),
                    '--config-from', str(dst), '--work', str(work / 'codegen'),
                    '--report', str(work / 'codegen-report.json'), '--keep'], check=True)


RECIPE = 'local-analysis/recipes/recipe-v455.json'


def recipe_file(root, named=None):
    """The recipe record: the one named (the same recipe with its tools' hashes recorded anew), else this Mac's."""
    return Path(named) if named else root / RECIPE


def recipe_build_arguments(cfg, work, root):
    """What the recipe tool is asked: all chunks written, each checked against the recipe's recorded hash and nothing
    else. expect_only: the earlier build's own chunk files (attempts/v507/src) are not read. Until 2026-10-08 20:39
    they were, as a second comparison; a run from a root that held only the declared inputs found it."""
    return argparse.Namespace(cmd='build', recipe=str(cfg), out=str(work / 'recipe/chunks'), data_root=str(root),
                              attempt='v507', chunks=None, write='all', stitch_cache=None, expect_only=True)


def run_recipe(root, work, recipe=None):
    rb = module('m09_recipe', root / 'tools/fp_codegen/recipe_build.py')
    recipe = json.loads(recipe_file(root, recipe).read_text(encoding='utf-8'))
    connected = connect_recipe(recipe, root, work)
    require_hash(work / 'decode/default.image.bin', recipe['inputs']['image']['sha256'])
    problems = rb.check_hashes(connected, root)
    if problems:
        raise ValueError('recipe tool/parameter/input check failed: ' + '; '.join(problems))
    corpus = work / 'recipe/corpus'
    corpus.mkdir(parents=True)
    tmp = work / 'recipe/base-tmp'
    tmp.mkdir()
    passes = {p['id']: rb.Pass(p, root, tmp) for p in connected['base_passes']}
    checked = []
    for n, c in sorted(connected['chunks'].items(), key=lambda v: int(v[0])):
        base = c['base']
        source = work / 'codegen/ppc' / Path(base['path']).name
        require_hash(source, base['sha256'])
        text = rb.read_chunk(source)
        for step in base.get('passes', []):
            text = passes[step['pass']].apply(int(n), text)
            if rb.sha_text(text) != step['sha256']:
                raise ValueError('base pass hash differs for chunk ' + n)
        expected = base.get('corpus', base)['sha256']
        if rb.sha_text(text) != expected:
            raise ValueError('reconstructed corpus hash differs for chunk ' + n)
        (corpus / source.name).write_text(text, encoding='utf-8', newline='\n')
        checked.append(int(n))
    # The stitcher consumes only mapping and chunks; do not substitute runtime headers.
    clone(work / 'codegen/ppc/ppc_func_mapping.cpp', corpus / 'ppc_func_mapping.cpp')
    cfg = work / 'recipe/connected-recipe.json'
    write_json(cfg, connected)
    write_json(work / 'recipe/base-check.json', {'chunks_checked': checked, 'source': 'this-run-codegen'})
    code = rb.build(recipe_build_arguments(cfg, work, root))
    if code:
        raise SystemExit(code)


def portable_library(objcopy, library, out):
    """Remove the compiler's and linker's name strings (.comment): the last host-dependent bytes of the library."""
    done = subprocess.run([str(objcopy), '--remove-section=.comment', str(library), str(out)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if done.returncode or not Path(out).is_file():
        raise ValueError('objcopy failed: ' + done.stdout.decode(errors='replace')[-400:])
    return sha(out)


GENERATED_HEADERS = ('ppc_recomp_shared.h', 'ppc_config.h')    # written by the generator; this run has its own
# This project's support header (supplied, not generated) includes the generated ones by a quoted name, and the
# compiler looks beside the including file first: read from the old corpus folder it pulls that folder's copies in.
# So a copy of it stands beside this run's generated headers.
BESIDE_GENERATED = ('ppc_context.h',)


def plan_commands(root, spec):
    """Per runtime source file of the attempt: the compile command its plan entry gives (as the build tool picks it)."""
    cfg = json.loads((root / 'local-analysis/attempts/config.json').read_text())
    plan = json.loads((root / cfg['base_build'] / 'plan.json').read_text())
    commands = {}
    for f in spec['files']:
        if 'ppc_chunk' in f:
            continue
        items = [i for i in plan['plans'] if Path(i['source']).name == f.get('template', f['name'])]
        if len(items) == 1:
            commands[f['name']] = list(json.loads((root / items[0]['spec']).read_text())['command'])
    return commands


def native_pack_includes(root, commands, packs):
    """Per runtime source file of the attempt: the include words for the native-packs stage's output (may be empty)."""
    report = json.loads((Path(packs) / 'report.json').read_text())
    if report.get('status') != 'complete' or not report.get('headers'):
        raise ValueError('the native-packs stage did not finish: its output is not used')
    for name, header in report['headers'].items():
        require_hash(Path(packs) / name, header['sha256'])
    first = {}
    for name, command in commands.items():
        words = pack_include_words(command, root, packs)
        if words:
            first[name] = words
    return first


def run_compile(root, work, shader_extras, portable_build_id=None, native_packs=None, xma_archives=None, measurements=True,
                kept_objects=False, attempt_name='v507', shader_fixes=None):
    """attempt_name: the attempt whose build this is (its attempt.json, runtime sources and version script); shader_fixes: the
    output of build_shader_fixes.py, whose tables then come from this run too, like the four of shader_extras."""
    from shader_compile_inputs import SHADER_INCLUDES, SHADER_FIXES, install_shader_inputs
    link_words = portable_link_words(portable_build_id) if portable_build_id is not None else []
    # Import only; call cmd_build, never iterate.main or any device function.
    sys.path.insert(0, str(root / 'tools/iter'))
    it = module('m09_iterate', root / 'tools/iter/iterate.py')
    reference = root / 'local-analysis/attempts' / attempt_name
    spec = json.loads((reference / 'attempt.json').read_text())
    left_out = None
    if not measurements:
        spec, left_out = without_measurements(spec)
    # The runtime files whose objects were kept from the base build are compiled in this run too: each from the
    # source the plan names, with the plan's command; afterwards each object must be the kept one.
    kept = kept_runtime_files(root, spec) if kept_objects else []
    spec['files'] = list(spec['files']) + [{'name': name, 'origin': source, 'source': source} for name, source, _ in kept]
    attempts = work / 'compile/attempts'
    attempt = attempts / attempt_name
    src = attempt / 'src'
    src.mkdir(parents=True)
    made_here = SHADER_INCLUDES + (SHADER_FIXES if shader_fixes is not None else ())
    # Clone only runtime/support files. Every PPC translation unit comes from this run's recipe.
    for p in (reference / 'src').iterdir():
        if not re.match(r'ppc_recomp\.\d+\.', p.name) and p.name not in made_here:
            clone(p, src / p.name)
    shader_inputs = install_shader_inputs(shader_extras, src)
    if shader_fixes is not None:
        shader_inputs = {'extras': shader_inputs, 'fixes': install_shader_inputs(shader_fixes, src, SHADER_FIXES)}
    write_json(work / 'compile/generated-shader-inputs.json', shader_inputs)
    if left_out is not None:
        write_json(work / 'compile/without-measurements.json', left_out)
    pinned = work / 'compile/pinned-ppc'
    pinned.mkdir()
    original_pinned = set()
    for f in spec['files']:
        if 'ppc_chunk' not in f:
            continue
        p = work / 'recipe/chunks' / f['name']
        original = root / f['source'] if f.get('source') else reference / 'src' / f['name']
        require_hash(p, sha(original))
        clone(p, (pinned if f.get('source') else src) / f['name'])
        if f.get('source'):
            original_pinned.add(original.parent)
    if len(original_pinned) != 1:
        raise ValueError('pinned chunks have multiple include directories; explicit mapping required')
    # The generator's own headers are this run's, not the old corpus folder's. The plans were made for that folder's
    # copies, so the run's must be the same files; then every unit reads them from the run.
    corpus = next(iter(original_pinned))
    generated = work / 'compile/generated-headers'
    generated.mkdir()
    for name in GENERATED_HEADERS:
        clone(work / 'codegen/ppc' / name, generated / name)
        require_hash(generated / name, sha(corpus / name))
    for name in BESIDE_GENERATED:
        clone(corpus / name, generated / name)
    write_json(work / 'compile/generated-headers.json', {
        'generated': {name: sha(generated / name) for name in GENERATED_HEADERS},
        'supplied': {name: {'from': str(corpus / name), 'sha256': sha(generated / name)} for name in BESIDE_GENERATED}})
    # Beside the pinned chunks stand the headers they include by a quoted name, and nothing else of the old folder:
    # a pack header copied here would be read as a file of this run and no measurement would show where it came from.
    for name in GENERATED_HEADERS + BESIDE_GENERATED:
        clone(generated / name, pinned / name)
    clone(reference / spec['version_script'], attempt / spec['version_script'])
    commands = plan_commands(root, spec)
    first_includes = {}
    for folder in ([native_packs] if isinstance(native_packs, Path) else list(native_packs or [])):
        # several stages may write headers in the old folders' layout; the first named goes first on the path
        for name, words in native_pack_includes(root, commands, folder).items():
            first_includes[name] = first_includes.get(name, []) + [w for w in words if w not in first_includes.get(name, [])]
    for name, command in commands.items():
        if '-I' + str(corpus) in command:
            first_includes[name] = ['-I' + str(generated)] + first_includes.get(name, [])
        if xma_archives is not None and '-I' + str(root / DECODER_HEADERS) in command:
            # the decoder's public headers come from this run's own FFmpeg build folder, not from the private copy
            first_includes[name] = ['-I' + str(xma_archives)] + first_includes.get(name, [])
    write_json(work / 'compile/native-pack-includes.json', first_includes)
    # The attempt names, among its link words, an earlier build's copy of the linker script that binds the generated
    # wrappers to their bodies. That script follows from a build's own objects, so this run makes its own: the first
    # build links without it and gives the objects, the second links with the script written from them.
    scripts = [w for w in spec.get('link_extra', []) if not w.startswith('-') and w.endswith('.ld')]
    if len(scripts) > 1:
        raise ValueError('the attempt names more than one linker script; explicit mapping required')
    connect = lambda chosen: connected_spec(chosen, attempt, work / 'recipe/chunks', pinned, link_words, first_includes, generated)
    write_json(attempt / 'attempt.json', connect(without_script(spec, scripts[0]) if scripts else spec))
    it.ATTEMPTS = attempts
    # Config and command plans remain immutable inputs. Cache/output roots are exclusively this run's.
    it.CONFIG = root / 'local-analysis/attempts/config.json'
    cfg = it.load_config()
    base = root / cfg['base_build']
    plan = json.loads((base / 'plan.json').read_text())
    link = json.loads((base / 'link-plan.json').read_text())
    if xma_archives is not None:
        link, archives = with_archives(link, xma_archives)
        write_json(work / 'compile/xma-archives.json', archives)
    items = [i for i in plan['plans'] if Path(i['source']).name == 'ppc_func_mapping.cpp']
    if len(items) != 1:
        raise ValueError('expected exactly one generated mapping object')
    item = items[0]
    mapping_source = work / 'compile/mapping-src/ppc_func_mapping.cpp'
    clone(work / 'codegen/ppc/ppc_func_mapping.cpp', mapping_source)
    require_hash(mapping_source, sha(Path(item['source'])))
    mapping_obj = work / 'compile/ppc_func_mapping.cpp.o'
    command = list(json.loads((root / item['spec']).read_text())['command'])
    command[command.index('-c') + 1] = str(mapping_source)
    command[command.index('-o') + 1] = str(mapping_obj)
    # Runtime support comes from the validated header set as in the reference mapping command.
    command[1:1] = ['-iquote', str(generated), '-iquote', str(Path(item['source']).parent)]
    command += ['-MD', '-MF', str(work / 'compile/mapping.d')]
    with (work / 'compile/mapping.log').open('wb') as log:
        subprocess.run(command, cwd=root / cfg['build_cwd'], stdout=log, stderr=subprocess.STDOUT, check=True)
    old_object = str(root / item['output'])
    require_hash(mapping_obj, link['inputs'][old_object])
    newbase = work / 'compile/base-plan'
    if link['command'].count(old_object) != 1:
        raise ValueError('mapping object must be consumed exactly once')
    link['command'] = [str(mapping_obj) if x == old_object else x for x in link['command']]
    link['inputs'][str(mapping_obj)] = link['inputs'].pop(old_object)
    write_json(newbase / 'plan.json', plan)
    write_json(newbase / 'link-plan.json', link)
    write_json(work / 'compile/mapping-proof.json', {'command': command, 'sha256': sha(mapping_obj),
               'reference_sha256': sha(Path(old_object)), 'equal': True})
    cfg['base_build'] = str(newbase)
    it.load_config = lambda: cfg
    library = it.cmd_build(attempt_name)
    if scripts:
        made = work / 'compile/redirect.ld'
        text, hooked = redirect_script(Path(link['command'][0]).parent / 'llvm-nm', (attempt / 'build' / 'obj').glob('*.o'))
        made.write_text(text, encoding='utf-8', newline='\n')
        first = sha(library)
        write_json(attempt / 'attempt.json', connect(with_script(spec, scripts[0], made)))
        library = it.cmd_build(attempt_name)  # every object is in this run's cache: only the link is new
        write_json(work / 'compile/redirect.json', {'script': str(made), 'sha256': sha(made), 'assignments': text.count('\n') - 1,
                   'hooked_wrappers_left_alone': hooked, 'replaces_link_word': scripts[0],
                   'linked_first_without_it': first, 'linked_with_it': sha(library)})
    if kept:
        record = {}
        for name, source, old in kept:
            made = attempt / 'build' / 'obj' / (name + '.o')
            require_hash(made, link['inputs'][old])
            record[name] = {'source': source, 'object': str(made), 'sha256': link['inputs'][old], 'replaces_link_word': old}
        write_json(work / 'compile/kept-objects.json', record)
    if portable_build_id is not None:
        # The same objcopy release as the compiler that built the objects: it sits beside the link driver.
        objcopy = Path(link['command'][0]).parent / 'llvm-objcopy'
        out = attempt / 'build' / 'libgame-portable.so'
        digest = portable_library(objcopy, library, out)
        write_json(work / 'compile/portable.json', {'library': str(out), 'sha256': digest, 'build_id': portable_build_id,
                   'link_words': link_words, 'linked': str(library), 'objcopy': str(objcopy)})


def check_work(root, work):
    """Everything a stage writes is game-derived: it stays under the ignored local-analysis/builder."""
    # (2026-10-24) resolved on both sides: local-analysis/builder may itself be a link (.builder since 2026-10-10)
    if not Path(work).resolve().is_relative_to((Path(root).resolve() / 'local-analysis/builder').resolve()):
        raise SystemExit('the work folder must lie in local-analysis/builder of the repository')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('stage', choices=['codegen', 'recipe', 'compile'])
    ap.add_argument('--repo', type=Path, required=True)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--shader-extras', type=Path, help='required generated shader header stage output for compile')
    ap.add_argument('--portable-build-id', help='40 hex digits: link the host-independent library with this build id')
    ap.add_argument('--native-packs', type=Path, action='append',
                    help='output of a stage that writes headers in the old folders\' layout (native-packs, code-headers): its folders go first on the include path; may be given more than once')
    ap.add_argument('--without-measurements', action='store_true',
                    help='leave out the optimisation profile and the symbol order (both measured on an earlier build): another library than the pinned one')
    ap.add_argument('--build-kept-objects', action='store_true',
                    help='also compile the runtime files whose objects were kept from the base build; each must come out as the kept object')
    ap.add_argument('--attempt', default='v507', help='compile: the attempt to build (default v507, the first builder\'s)')
    ap.add_argument('--shader-fixes', type=Path, help='compile: output of build_shader_fixes.py (the later fix tables), installed like --shader-extras')
    ap.add_argument('--recipe', type=Path, help='recipe: the recipe record to build from (default: local-analysis/recipes/recipe-v455.json)')
    ap.add_argument('--generator', type=Path, help='codegen: this host\'s build of the generator (default: this Mac\'s private build)')
    ap.add_argument('--context', type=Path, help='codegen: the generator\'s ppc_context.h (default: beside this Mac\'s private build)')
    ap.add_argument('--xma-archives', type=Path,
                    help='build folder of build_xma_decoder.sh from this run: its two archives are linked in the place of the supplied ones')
    args = ap.parse_args()
    root, work = args.repo.resolve(), args.work.resolve()
    check_work(root, work)
    if args.stage == 'compile':
        if args.shader_extras is None:raise SystemExit('--shader-extras is required for compile')
        run_compile(root, work, args.shader_extras.resolve(), args.portable_build_id,
                    [p.resolve() for p in args.native_packs] if args.native_packs else None,
                    args.xma_archives.resolve() if args.xma_archives else None,
                    not args.without_measurements, args.build_kept_objects, args.attempt,
                    args.shader_fixes.resolve() if args.shader_fixes else None)
    elif args.stage == 'codegen':
        run_codegen(root, work, args.generator.resolve() if args.generator else None, args.context.resolve() if args.context else None)
    else:
        run_recipe(root, work, args.recipe.resolve() if args.recipe else None)


if __name__ == '__main__':
    main()
