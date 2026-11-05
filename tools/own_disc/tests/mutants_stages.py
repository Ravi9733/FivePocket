#!/usr/bin/env python3
"""Mutant check for the stage runner (compile_rehearsal.py) and the shader-table connector (shader_compile_inputs.py):
each mutant is a copy with one rule removed or one value changed; test_stage_connections.py and test_compile_stage.py
(beside this file) must fail on it.

  mutants_stages.py [--only TEXT]

First a control on unchanged copies. Exits 1 if the control fails, a mutant survives or a mutant no longer applies."""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent
FILES = ('compile_rehearsal.py', 'shader_compile_inputs.py')
TESTS = ('test_stage_connections.py', 'test_compile_stage.py')
R, S = FILES

MUTANTS = [
    # ---- the recipe stage reads nothing of the earlier build
    (R, 'the recipe tool compares with the earlier build\'s chunk files', "stitch_cache=None, expect_only=True)", "stitch_cache=None, expect_only=False)"),
    (R, 'the recipe tool writes only the chunks that differ', "attempt='v507', chunks=None, write='all',", "attempt='v507', chunks=None, write='mismatch',"),
    # ---- another host's generator
    (R, 'the generator is always this Mac\'s', "                    '--generator', str(generator or root / GENERATOR),", "                    '--generator', str(root / GENERATOR),"),
    (R, 'the generator\'s header is always this Mac\'s', "                    '--context', str(context or root / GENERATOR_CONTEXT),", "                    '--context', str(root / GENERATOR_CONTEXT),"),
    (R, 'the command line drops the generator named', "        run_codegen(root, work, args.generator.resolve() if args.generator else None, args.context.resolve() if args.context else None)",
     "        run_codegen(root, work, None, args.context.resolve() if args.context else None)"),
    (R, 'the command line drops the header named', "        run_codegen(root, work, args.generator.resolve() if args.generator else None, args.context.resolve() if args.context else None)",
     "        run_codegen(root, work, args.generator.resolve() if args.generator else None, None)"),
    (R, 'the recipe stage runs the generator', "    else:\n        run_recipe(root, work, args.recipe.resolve() if args.recipe else None)\n", "    else:\n        run_codegen(root, work)\n"),
    (R, 'the command line drops the recipe named', "        run_recipe(root, work, args.recipe.resolve() if args.recipe else None)\n", "        run_recipe(root, work, None)\n"),
    (R, 'the recipe is always this Mac\'s', "    return Path(named) if named else root / RECIPE\n", "    return root / RECIPE\n"),
    (R, 'the stage reads the default recipe whatever is named', "    recipe = json.loads(recipe_file(root, recipe).read_text(encoding='utf-8'))", "    recipe = json.loads(recipe_file(root).read_text(encoding='utf-8'))"),
    # ---- a host without clones
    (R, 'every host runs the Mac copy command',
     "    if sys.platform == 'darwin':\n        subprocess.run(['/bin/cp', '-c', str(src), str(dst)], check=True)", "    if True:\n        subprocess.run(['/bin/cp', '-c', str(src), str(dst)], check=True)"),
    (R, 'the Mac copies bytes instead of cloning',
     "    if sys.platform == 'darwin':\n        subprocess.run(['/bin/cp', '-c', str(src), str(dst)], check=True)", "    if False:\n        subprocess.run(['/bin/cp', '-c', str(src), str(dst)], check=True)"),
    # ---- each stage reads this run's producer
    (R, 'the generator reads the old executable',
     "target = xex if name == 'file_path' else (original_dir / old).resolve()", "target = (original_dir / old).resolve()"),
    (R, 'the recipe reads the old chunks',
     "        base['path'] = str(parent / Path(base['path']).name)", "        pass"),
    (R, 'the compiler reads the old game code',
     "                f['source'] = str(pinned / f['name'])", "                pass"),
    (R, 'five compiles at once',
     "    out['build_jobs'] = 4", "    out['build_jobs'] = 5"),
    (R, 'a producer with another hash is accepted',
     "    if sha(path) != wanted:", "    if False:"),
    (R, 'the link takes the old mapping object',
     "    link['command'] = [str(mapping_obj) if x == old_object else x for x in link['command']]", "    pass"),
    (R, 'the build writes into the reference attempt',
     "    it.ATTEMPTS = attempts\n", "    pass\n"),
    (R, 'the captured shader tables are cloned with the other sources',
     " and p.name not in SHADER_INCLUDES", ""),
    (R, "this run's shader tables are not installed",
     "    shader_inputs = install_shader_inputs(shader_extras, src)", "    shader_inputs = {}"),
    (S, 'the installed table is not what the shader stage made',
     "stream.write(blob)", "stream.write(b'bad')"),
    (S, 'a table that differs from its receipt is accepted',
     "if len(blob)!=expected['bytes'] or digest!=expected['sha256']:", "if False:"),
    (S, 'an incomplete shader stage is accepted',
     "if report.get('status')!='complete' or set(report.get('outputs',{}))!=set(SHADER_INCLUDES):", "if False:"),
    # ---- the portable library
    (R, 'the portable link words do not reach the link',
     " + list(link_words)", ""),
    (R, 'debug sections stay in the portable library',
     "    return ['-Wl,--strip-debug', '-Wl,--build-id=0x' + build_id]", "    return ['-Wl,--build-id=0x' + build_id]"),
    (R, 'the build id is not pinned',
     "    return ['-Wl,--strip-debug', '-Wl,--build-id=0x' + build_id]", "    return ['-Wl,--strip-debug']"),
    (R, 'any build id is accepted',
     "    if not PORTABLE_ID.fullmatch(build_id or ''):", "    if False:"),
    (R, 'upper-case build ids are accepted',
     "PORTABLE_ID = re.compile(r'[0-9a-f]{40}')", "PORTABLE_ID = re.compile(r'[0-9a-fA-F]{40}')"),
    (R, 'build ids of another length are accepted',
     "PORTABLE_ID = re.compile(r'[0-9a-f]{40}')", "PORTABLE_ID = re.compile(r'[0-9a-f]{38,42}')"),
    (R, 'a bad build id is noticed only after the build',
     "    link_words = portable_link_words(portable_build_id) if portable_build_id is not None else []",
     "    link_words = ['-Wl,--strip-debug', '-Wl,--build-id=0x' + portable_build_id] if portable_build_id is not None else []"),
    (R, "the compiler's name strings stay in the library",
     "'--remove-section=.comment', ", ""),
    (R, 'a failed objcopy goes unnoticed',
     "    if done.returncode or not Path(out).is_file():", "    if False:"),
    (R, 'the portable step is skipped',
     "    if portable_build_id is not None:\n        # The same objcopy", "    if False:\n        # The same objcopy"),
    (R, "the record does not carry the library's hash",
     "'sha256': digest, 'build_id'", "'sha256': '', 'build_id'"),
    (R, 'objcopy runs on something other than what was linked',
     "        digest = portable_library(objcopy, library, out)", "        digest = portable_library(objcopy, out, out)"),
    # ---- where a stage may write
    # ---- the native-packs stage's folders on the include path
    (R, 'a folder whose name only starts like the private folder is taken',
     "    prefix = str(Path(root) / 'local-analysis') + '/'\n", "    prefix = str(Path(root) / 'local-analysis')\n"),
    (R, 'a pack folder the stage did not write is put on the path',
     "            if mirror.is_dir() and '-I' + str(mirror) not in words:", "            if '-I' + str(mirror) not in words:"),
    (R, 'a pack folder is put on the path once per mention',
     "            if mirror.is_dir() and '-I' + str(mirror) not in words:", "            if mirror.is_dir():"),
    (R, 'include folders outside the private folder are redirected too',
     "        if word.startswith('-I') and word[2:].startswith(prefix):", "        if word.startswith('-I'):"),
    (R, "a file's own words are replaced by the pack folders",
     "            f['defines'] = list(f.get('defines', [])) + list(first_includes[f['name']])", "            f['defines'] = list(first_includes[f['name']])"),
    (R, 'the pack folders go in front of a file\'s own words',
     "            f['defines'] = list(f.get('defines', [])) + list(first_includes[f['name']])", "            f['defines'] = list(first_includes[f['name']]) + list(f.get('defines', []))"),
    (R, 'game code chunks get pack folders too',
     "        elif first_includes and first_includes.get(f['name']):", "        if first_includes and first_includes.get(f['name']):"),
    (R, 'an unfinished pack stage is used',
     "    if report.get('status') != 'complete' or not report.get('headers'):", "    if not report.get('headers'):"),
    (R, 'a pack stage that wrote nothing is used',
     "    if report.get('status') != 'complete' or not report.get('headers'):", "    if report.get('status') != 'complete':"),
    (R, 'the pack headers are not checked against the stage\'s report',
     "        require_hash(Path(packs) / name, header['sha256'])\n", "        pass\n"),
    (R, 'the compile stage ignores the pack stage',
     "    for folder in ([native_packs] if isinstance(native_packs, Path) else list(native_packs or [])):", "    for folder in []:"),
    (R, 'only the first header stage is used',
     "    for folder in ([native_packs] if isinstance(native_packs, Path) else list(native_packs or [])):", "    for folder in ([native_packs] if isinstance(native_packs, Path) else list(native_packs or [])[:1]):"),
    (R, 'a later header stage replaces the folders of an earlier one',
     "            first_includes[name] = first_includes.get(name, []) + [w for w in words if w not in first_includes.get(name, [])]", "            first_includes[name] = words"),
    # ---- the generator's headers come from this run
    (R, "this run's generated header is not compared with the plans' folder",
     "        require_hash(generated / name, sha(corpus / name))\n", ""),
    (R, 'files that read the old corpus folder do not get this run\'s generated headers first',
     "        if '-I' + str(corpus) in command:", "        if False:"),
    (R, 'every runtime file gets the generated header folder',
     "        if '-I' + str(corpus) in command:", "        if True:"),
    (R, 'the generated header folder goes behind the pack folders',
     "            first_includes[name] = ['-I' + str(generated)] + first_includes.get(name, [])", "            first_includes[name] = first_includes.get(name, []) + ['-I' + str(generated)]"),
    (R, 'only one of the two generated headers is taken from this run',
     "GENERATED_HEADERS = ('ppc_recomp_shared.h', 'ppc_config.h')", "GENERATED_HEADERS = ('ppc_recomp_shared.h',)"),
    (R, 'game code chunks keep finding the old folder\'s generator headers',
     "        out['ppc_defines'] = list(out.get('ppc_defines', [])) + ['-iquote', str(generated)]", "        pass"),
    (R, "the chunks' own words are replaced by the generated header folder",
     "        out['ppc_defines'] = list(out.get('ppc_defines', [])) + ['-iquote', str(generated)]", "        out['ppc_defines'] = ['-iquote', str(generated)]"),
    (R, 'the compile stage does not hand the generated header folder to the chunks',
     "pinned, link_words, first_includes, generated)\n    write_json(attempt / 'attempt.json', connect(", "pinned, link_words, first_includes)\n    write_json(attempt / 'attempt.json', connect("),
    (R, 'the mapping unit searches the old folder first',
     "    command[1:1] = ['-iquote', str(generated), '-iquote', str(Path(item['source']).parent)]", "    command[1:1] = ['-iquote', str(Path(item['source']).parent), '-iquote', str(generated)]"),
    (R, 'the mapping unit does not search this run\'s headers',
     "    command[1:1] = ['-iquote', str(generated), '-iquote', str(Path(item['source']).parent)]", "    command[1:1] = ['-iquote', str(Path(item['source']).parent)]"),
    (R, "the project's support header stays in the old folder (and pulls that folder's generated headers in)",
     "BESIDE_GENERATED = ('ppc_context.h',)", 'BESIDE_GENERATED = ()'),
    (R, "the support header beside the generated ones is the generator's copy, not the project's",
     '        clone(corpus / name, generated / name)\n', "        clone(work / 'codegen/ppc' / name, generated / name)\n"),
    (R, "the record names the copy as the supplied header's origin",
     "'supplied': {name: {'from': str(corpus / name),", "'supplied': {name: {'from': str(generated / name),"),
    (R, 'the record lists the supplied header as generated',
     "        'generated': {name: sha(generated / name) for name in GENERATED_HEADERS},", "        'generated': {name: sha(generated / name) for name in GENERATED_HEADERS + BESIDE_GENERATED},"),
    (R, "the record gives the old folder's hash without reading this run's generated header",
     "        'generated': {name: sha(generated / name) for name in GENERATED_HEADERS},", "        'generated': {name: 'x' for name in GENERATED_HEADERS},"),
    # ---- the linker script from this build's own objects
    (R, "no script is made from this build's objects (the first link stays)",
     "    if scripts:\n        made = work / 'compile/redirect.ld'\n", "    if False:\n        made = work / 'compile/redirect.ld'\n"),
    (R, "the first build links with the earlier build's script",
     'connect(without_script(spec, scripts[0]) if scripts else spec))', 'connect(spec))'),
    (R, "the second build links with the earlier build's script",
     "    out['link_extra'] = [str(made) if w == script else w for w in out.get('link_extra', [])]", "    out['link_extra'] = list(out.get('link_extra', []))"),
    (R, "this run's script goes to the end of the link words",
     "    out['link_extra'] = [str(made) if w == script else w for w in out.get('link_extra', [])]", "    out['link_extra'] = [w for w in out.get('link_extra', []) if w != script] + [str(made)]"),
    (R, 'the first build loses another link word with the script',
     "    out['link_extra'] = [w for w in out.get('link_extra', []) if w != script]", "    out['link_extra'] = [w for w in out.get('link_extra', []) if w != script][:-1]"),
    (R, 'a hooked wrapper is bound to its body',
     '        if name in strong:\n            hooked += 1\n            continue\n', ''),
    (R, 'hooked wrappers are not counted',
     '            hooked += 1\n', '            pass\n'),
    (R, 'a wrapper first defined by a runtime object is bound',
     " or not owner.startswith('ppc_recomp.') or len(m.group(2)) != int(m.group(1)):", ' or len(m.group(2)) != int(m.group(1)):'),
    (R, 'a name whose length field is wrong is bound',
     ' or len(m.group(2)) != int(m.group(1)):', ':'),
    (R, 'the last definer of a weak name counts, not the first',
     '                weak.setdefault(fields[2], obj.name)', '                weak[fields[2]] = obj.name'),
    (R, 'the objects are read in the order they are handed in',
     '    for obj in sorted(objects, key=lambda p: p.name):', '    for obj in list(objects):'),
    (R, 'strong definitions are not seen',
     "            elif fields[1] == 'T':", "            elif fields[1] == 'S':"),
    (R, 'local and undefined names count as wrappers',
     "            if fields[1] == 'W':", "            if fields[1] in 'WtU':"),
    (R, 'the assignments stand in the order the names were met',
     '    for name, owner in sorted(weak.items()):', '    for name, owner in weak.items():'),
    (R, 'the script has no last line end',
     "    return REDIRECT_HEAD + '\\n'.join(lines) + '\\n', hooked", "    return REDIRECT_HEAD + '\\n'.join(lines), hooked"),
    (R, 'the script has another first line',
     "REDIRECT_HEAD = '/* code-reach: bind unhooked", "REDIRECT_HEAD = '/* bind unhooked"),
    (R, 'an object nm cannot read is passed over',
     "        if done.returncode != 0:\n            raise ValueError('nm failed on ' + obj.name + ': ' + done.stdout[-300:])\n", ''),
    (R, 'several linker scripts: the first is taken',
     "    if len(scripts) > 1:\n        raise ValueError('the attempt names more than one linker script; explicit mapping required')\n", ''),
    (R, 'any link input counts as the linker script',
     "if not w.startswith('-') and w.endswith('.ld')]", "if not w.startswith('-')]"),
    (R, 'nm is taken from the path',
     "redirect_script(Path(link['command'][0]).parent / 'llvm-nm',", "redirect_script('llvm-nm',"),
    (R, 'every file of the object folder is read',
     "(attempt / 'build' / 'obj').glob('*.o'))", "(attempt / 'build' / 'obj').glob('*'))"),
    (R, 'the script is not written before the second build',
     "        made.write_text(text, encoding='utf-8', newline='\\n')\n", ''),
    (R, 'the record counts one assignment more',
     "'assignments': text.count('\\n') - 1,", "'assignments': text.count('\\n'),"),
    (R, 'the record gives the second library for the first',
     "        first = sha(library)\n        write_json(attempt / 'attempt.json', connect(with_script(spec, scripts[0], made)))\n        library = it.cmd_build('v507')", "        write_json(attempt / 'attempt.json', connect(with_script(spec, scripts[0], made)))\n        library = it.cmd_build('v507')\n        first = sha(library)"),
    # ---- beside the pinned chunks
    (R, 'every header of the old folder is copied beside the pinned chunks',
     '    for name in GENERATED_HEADERS + BESIDE_GENERATED:\n        clone(generated / name, pinned / name)\n', "    for name in GENERATED_HEADERS + BESIDE_GENERATED:\n        clone(generated / name, pinned / name)\n    for p in corpus.iterdir():\n        if p.is_file() and p.suffix in ('.h', '.inc') and not (pinned / p.name).exists():\n            clone(p, pinned / p.name)\n"),
    (R, 'the support header is missing beside the pinned chunks',
     '    for name in GENERATED_HEADERS + BESIDE_GENERATED:\n        clone(generated / name, pinned / name)\n', '    for name in GENERATED_HEADERS:\n        clone(generated / name, pinned / name)\n'),
    # (taking these three from the old folder instead is no mutant: the stage has just required them equal)
    # ---- this run's own audio decoder archives
    (R, "the run's decoder archives are not linked",
     '    if xma_archives is not None:\n        link, archives = with_archives(link, xma_archives)\n', '    if xma_archives is not None:\n        archives = with_archives(link, xma_archives)[1]\n'),
    (R, 'only the first decoder archive is replaced',
     "XMA_ARCHIVES = ('libavcodec.a', 'libavutil.a')", "XMA_ARCHIVES = ('libavcodec.a',)"),
    (R, 'a decoder archive that is not the pinned one is linked',
     "        require_hash(made, out['inputs'][old])\n", ''),
    (R, 'the hash of a replaced archive stays under the old path',
     "        out['inputs'][str(made)] = out['inputs'].pop(old)\n        record[name] = {'made': str(made), 'sha256': out['inputs'][str(made)], 'replaces_link_word': old}", "        record[name] = {'made': str(made), 'sha256': out['inputs'][old], 'replaces_link_word': old}"),
    (R, 'the link word of a replaced archive stays the old path',
     "        out['command'] = [str(made) if w == old else w for w in out['command']]\n        out['inputs'][str(made)]", "        out['inputs'][str(made)]"),
    (R, 'a missing archive of the run is not noticed before the hash',
     "        if not made.is_file():\n            raise ValueError('this run has no ' + name + ': the decoder stage did not finish')\n", ''),
    (R, 'a plan that names an archive twice is accepted',
     "        if len(old) != 1 or old[0] not in out['inputs']:", "        if len(old) < 1 or old[0] not in out['inputs']:"),
    (R, 'a plan without the hash of an archive is accepted',
     "        if len(old) != 1 or old[0] not in out['inputs']:", '        if len(old) != 1:'),
    (R, 'an option that ends like an archive counts as one',
     "        old = [w for w in out['command'] if not w.startswith('-') and Path(w).name == name]", "        old = [w for w in out['command'] if w.endswith(name)]"),
    (R, 'the plan handed in is changed',
     '    out, record, words = copy.deepcopy(link), {}, {}', '    out, record, words = link, {}, {}'),
    (R, 'the record of the archives is not written',
     "        write_json(work / 'compile/xma-archives.json', archives)\n", ''),
    (R, 'the archives are looked for directly in the folder',
     '        made = Path(folder) / name[:-2] / name', '        made = Path(folder) / name'),
    (R, 'the command line drops the decoder archives',
     '                    args.xma_archives.resolve() if args.xma_archives else None,\n', '                    None,\n'),
    (R, 'the command line drops the pack folders',
     '                    [p.resolve() for p in args.native_packs] if args.native_packs else None,', '                    None,'),
    # ---- without the measurements of an earlier build
    (R, 'the profile stays although the measurements are to be left out',
     '            if isinstance(word, str) and word.startswith(names):\n                if word not in into:\n                    into.append(word)\n            elif', '            if isinstance(word, str) and word.startswith(names):\n                if word not in into:\n                    into.append(word)\n                kept.append(word)\n            elif'),
    (R, "a measurement's warning switches stay",
     '            elif word not in companions:\n                kept.append(word)', '            else:\n                kept.append(word)'),
    (R, 'the chunks keep the profile',
     "        extra['flags'] = strip(extra['flags'], PROFILE_WORDS, PROFILE_COMPANIONS, named['profile'])\n", ''),
    (R, 'the single files keep the profile',
     "            f['extra_flags'] = strip(f['extra_flags'], PROFILE_WORDS, PROFILE_COMPANIONS, named['profile'])\n", '            pass\n'),
    (R, 'the symbol order stays among the link words',
     "    out['link_extra'] = strip(out.get('link_extra', []), ORDER_WORDS, ORDER_COMPANIONS, named['symbol_order'])\n", ''),
    (R, 'a measurement in an unknown place is passed over',
     "    if left:\n        raise ValueError('the attempt still names a measurement of an earlier build in a place this stage does not know: ' + left[0][:60])\n", ''),
    (R, 'only lists are searched for a remaining measurement',
     '        elif isinstance(value, dict):\n            for item in value.values():\n                yield from texts(item)\n', ''),
    (R, 'a remaining symbol order is not looked for',
     '    left = [t for t in texts(out) if t.startswith(PROFILE_WORDS + ORDER_WORDS)]', '    left = [t for t in texts(out) if t.startswith(PROFILE_WORDS)]'),
    (R, 'only one kind of profile word is known',
     "PROFILE_WORDS = ('-fprofile-use=', '-fprofile-instr-use=', '-fprofile-sample-use=')", "PROFILE_WORDS = ('-fprofile-use=',)"),
    (R, 'a profile is named once per place that carried it',
     '                if word not in into:\n                    into.append(word)', '                into.append(word)'),
    (R, 'the attempt handed in is changed',
     "    out = copy.deepcopy(spec)\n    named = {'profile': [], 'symbol_order': []}", "    out = spec\n    named = {'profile': [], 'symbol_order': []}"),
    (R, 'the measurements are always left out',
     '    if not measurements:\n        spec, left_out = without_measurements(spec)', '    if True:\n        spec, left_out = without_measurements(spec)'),
    (R, 'the measurements are never left out',
     '    if not measurements:\n        spec, left_out = without_measurements(spec)', '    if False:\n        spec, left_out = without_measurements(spec)'),
    (R, 'what was left out is not recorded',
     "    if left_out is not None:\n        write_json(work / 'compile/without-measurements.json', left_out)\n", ''),
    (R, 'the command line ignores --without-measurements',
     '                    not args.without_measurements, args.build_kept_objects)', '                    True, args.build_kept_objects)'),
    # ---- the runtime objects kept from the base build
    (R, 'the kept objects are never compiled in the run',
     '    kept = kept_runtime_files(root, spec) if kept_objects else []', '    kept = []'),
    (R, 'the kept objects are compiled in every run',
     '    kept = kept_runtime_files(root, spec) if kept_objects else []', '    kept = kept_runtime_files(root, spec)'),
    (R, 'a kept object that comes out otherwise is linked',
     "            require_hash(made, link['inputs'][old])\n            record[name]", '            record[name]'),
    (R, 'the kept files are not added to the attempt',
     "    spec['files'] = list(spec['files']) + [{'name': name, 'origin': source, 'source': source} for name, source, _ in kept]\n", ''),
    (R, "a kept file is taken from the attempt's own folder",
     "[{'name': name, 'origin': source, 'source': source} for name, source, _ in kept]", "[{'name': name, 'origin': source} for name, source, _ in kept]"),
    (R, 'a kept file may be a private source',
     " or source.is_relative_to(root / 'local-analysis'):", ':'),
    (R, 'a file the attempt builds under another name counts as kept',
     "    built = {f.get('template', f['name']) for f in spec['files'] if 'ppc_chunk' not in f}", "    built = {f['name'] for f in spec['files'] if 'ppc_chunk' not in f}"),
    (R, 'the mapping unit counts as kept',
     "        if source.name in built or source.name == 'ppc_func_mapping.cpp':", '        if source.name in built:'),
    (R, 'the record of the kept objects is not written',
     "        write_json(work / 'compile/kept-objects.json', record)\n", ''),
    (R, "a kept file reads the private copy of the decoder's headers",
     "        if xma_archives is not None and '-I' + str(root / DECODER_HEADERS) in command:", '        if False:'),
    (R, "every file gets the run's decoder folder",
     "        if xma_archives is not None and '-I' + str(root / DECODER_HEADERS) in command:", '        if xma_archives is not None:'),
    (R, 'the command line ignores --build-kept-objects',
     '                    not args.without_measurements, args.build_kept_objects)', '                    not args.without_measurements, False)'),
    (R, 'any work folder is accepted',
     "    if not Path(work).resolve().is_relative_to(Path(root).resolve() / 'local-analysis/builder'):", "    if False:"),
    (R, 'a work folder anywhere in local-analysis is accepted',
     "Path(root).resolve() / 'local-analysis/builder'):", "Path(root).resolve() / 'local-analysis'):"),
]


def run(folder: Path) -> subprocess.CompletedProcess:
    env = dict(os.environ, STAGE_TOOL=str(folder / R), COMPILE_ADAPTER=str(folder / R), COMPILE_SHADER_HELPER=str(folder / S))
    out, code = '', 0
    for test in TESTS:
        r = subprocess.run([sys.executable, '-B', str(HERE / test), '-v'], env=env, capture_output=True, text=True, timeout=180)
        out += r.stderr
        code = code or r.returncode
    return subprocess.CompletedProcess([], code, '', out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--only', help='run only the mutants whose description contains this text')
    a = ap.parse_args()
    sources = {f: (TOOLS / f).read_text() for f in FILES}
    chosen = [(i, m) for i, m in enumerate(MUTANTS, 1) if not a.only or a.only in m[1]]
    killed = 0
    with tempfile.TemporaryDirectory() as tmp:
        control = Path(tmp) / 'control'
        control.mkdir()
        for f in FILES:
            shutil.copy(TOOLS / f, control / f)
        r = run(control)
        if r.returncode != 0:
            print('CONTROL FAILED: the tests do not pass on unchanged copies\n' + r.stderr[-2000:])
            return 1
        print('control: the tests pass on unchanged copies of the tools', flush=True)
        for index, (name, what, old, new) in chosen:
            if sources[name].count(old) != 1:
                print(f'mutant {index:3d} DOES NOT APPLY ({sources[name].count(old)} matches in {name}): {what}')
                continue
            folder = Path(tmp) / f'm{index}'
            folder.mkdir()
            for f in FILES:
                (folder / f).write_text(sources[f].replace(old, new) if f == name else sources[f])
            r = run(folder)
            failed = [line.split(' ... ')[0].strip() for line in r.stderr.splitlines() if line.rstrip().endswith(('... FAIL', '... ERROR'))]
            if r.returncode == 0:
                print(f'mutant {index:3d} SURVIVED ({name}): {what}')
            else:
                killed += 1
                print(f'mutant {index:3d} killed by {failed[0].split(" (")[0] if failed else "the test run (it could not start)"}: {what}')
    print(f'{killed} of {len(chosen)} mutants killed')
    return 0 if killed == len(chosen) else 1


if __name__ == '__main__':
    sys.exit(main())
