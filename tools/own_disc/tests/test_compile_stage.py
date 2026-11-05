#!/usr/bin/env python3
"""The compile stage against a synthetic tree and a stand-in compiler: it must build from this run's recipe output,
this run's mapping and this run's shader tables, never write into the reference attempt, and, when asked for the
portable library, pass the link words and run objcopy on what was linked.

COMPILE_ADAPTER and COMPILE_SHADER_HELPER name the two files under test (the mutant runner points them at changed
copies). From the builder-rehearsal and builder-shaders lanes' tests of 2026-10-07, joined, with the portable cases."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
helper = Path(os.environ.get('COMPILE_SHADER_HELPER', HERE.parent / 'shader_compile_inputs.py'))
spec = importlib.util.spec_from_file_location('shader_compile_inputs', helper)
helpers = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = helpers          # the stage runner imports it by this name
spec.loader.exec_module(helpers)
SHADER_INCLUDES = helpers.SHADER_INCLUDES
target = Path(os.environ.get('COMPILE_ADAPTER', HERE.parent / 'compile_rehearsal.py'))
spec = importlib.util.spec_from_file_location('compile_adapter', target)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
ID = 'dc6659d7f4d87473d1ea10ed2fd7e99ac20ddcfe'

FAKE_ITERATE = '''import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
ATTEMPTS = ROOT / 'local-analysis/attempts'
CONFIG = ATTEMPTS / 'config.json'
def load_config(): return json.loads(CONFIG.read_text())
def cmd_build(name):
    a = ATTEMPTS / name
    spec = json.loads((a / 'attempt.json').read_text())
    cfg = load_config()
    link = json.loads((Path(cfg['base_build']) / 'link-plan.json').read_text())
    contents = Path(spec['files'][0]['source']).read_text()
    mapping = Path(link['command'][1]).read_text()
    tables = ''.join((a / 'src' / n).read_text() for n in %r)
    (a / 'build').mkdir(exist_ok=True)
    objects = a / 'src/fake-objects.json'
    if objects.exists():
        (a / 'build/obj').mkdir(exist_ok=True)
        for name, text in json.loads(objects.read_text()).items():
            (a / 'build/obj' / name).write_text(text)
    for f in spec['files']:
        if 'ppc_chunk' not in f and f.get('source'):
            (a / 'build/obj').mkdir(exist_ok=True)
            (a / 'build/obj' / (f['name'] + '.o')).write_bytes((ROOT / f['source']).read_bytes())
    linked = ''.join('|linked script:' + Path(w).read_text() for w in spec.get('link_extra', []) if w.endswith('.ld') and Path(w).exists())
    (a / 'build/library').write_text(contents + mapping + tables + linked)
    (a / 'build/observed.json').write_text(json.dumps({'source': spec['files'][0]['source'], 'mapping': link['command'][1],
        'jobs': spec['build_jobs'], 'link_extra': spec.get('link_extra')}))
    calls = a / 'build/calls.json'
    calls.write_text(json.dumps((json.loads(calls.read_text()) if calls.exists() else []) + [spec.get('link_extra')]))
    return a / 'build/library'
''' % (SHADER_INCLUDES,)


def put(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data)


def script(path, body):
    put(path, '#!' + sys.executable + '\n' + body)
    path.chmod(0o755)


class Tree:
    def __init__(self, root, objcopy_exit=0):
        self.root = root
        self.original = root / 'local-analysis/attempts/v507'
        self.work = root / 'local-analysis/builder/run/work'
        corpus = root / 'local-analysis/corpus'
        put(corpus / 'ppc_recomp.0.cpp', 'recipe fixture')
        put(corpus / 'ppc_func_mapping.cpp', 'mapping fixture')
        for name in ('ppc_recomp_shared.h', 'ppc_config.h'):  # the generator's headers: the old folder's and this run's are the same files
            put(corpus / name, 'generated ' + name)
            put(self.work / 'codegen/ppc' / name, 'generated ' + name)
        put(corpus / 'ppc_context.h', 'the project header of the old folder')
        put(corpus / 'native_old_shader_pack.h', 'a pack header of an earlier build')
        put(corpus / 'old_table.inc', 'a table of an earlier build')
        put(self.work / 'codegen/ppc/ppc_context.h', 'the support header as the generator copies it')
        self.corpus = corpus
        put(self.original / 'src/host.h', 'header fixture')
        put(self.original / 'exports', 'exports fixture')
        put(self.original / 'build/library', 'untouched reference')
        put(self.work / 'recipe/chunks/ppc_recomp.0.cpp', 'recipe fixture')
        put(self.work / 'codegen/ppc/ppc_func_mapping.cpp', 'mapping fixture')
        m.write_json(self.original / 'attempt.json', {'files': [{'name': 'ppc_recomp.0.cpp', 'ppc_chunk': 0,
            'source': str(corpus / 'ppc_recomp.0.cpp')}], 'version_script': 'exports', 'build_jobs': 8,
            'link_extra': ['-Wl,--own-word']})
        base = root / 'local-analysis/base'
        put(base / 'mapping.o', 'mapping fixture')
        tools = root / 'toolbin'
        script(tools / 'compiler', "import sys\nfrom pathlib import Path\na=sys.argv\n"
               "Path(a[a.index('-o')+1]).write_bytes(Path(a[a.index('-c')+1]).read_bytes())\n")
        script(tools / 'llvm-objcopy', "import sys, json\nfrom pathlib import Path\na=sys.argv[1:]\n"
               "Path(__file__).with_name('objcopy-argv.json').write_text(json.dumps(a))\n"
               "if %d: sys.exit(%d)\n"
               "Path(a[-1]).write_text(Path(a[-2]).read_text() + ' [no comment]')\n" % (objcopy_exit, objcopy_exit))
        script(tools / 'llvm-nm', "import sys\nfrom pathlib import Path\ntext = Path(sys.argv[-1]).read_text()\n"
               "assert sys.argv[1:-1] == ['--defined-only']\nprint(text, end='')\nsys.exit(3 if 'BROKEN' in text else 0)\n")
        self.tools = tools
        m.write_json(base / 'mapping-build.json', {'command': [str(tools / 'compiler'), '-c',
            str(corpus / 'ppc_func_mapping.cpp'), '-o', str(base / 'mapping.o')]})
        m.write_json(base / 'plan.json', {'plans': [{'source': str(corpus / 'ppc_func_mapping.cpp'),
            'spec': str(base / 'mapping-build.json'), 'output': str(base / 'mapping.o')}]})
        m.write_json(base / 'link-plan.json', {'command': [str(tools / 'driver'), str(base / 'mapping.o')],
            'inputs': {str(base / 'mapping.o'): m.sha(base / 'mapping.o')}})
        m.write_json(root / 'local-analysis/attempts/config.json', {'base_build': str(base), 'build_cwd': str(root)})
        put(root / 'tools/iter/iterate.py', FAKE_ITERATE)
        self.generated = root / 'generated'
        self.generated.mkdir()
        outputs = {}
        for name in SHADER_INCLUDES:
            blob = ('fresh ' + name).encode()
            (self.generated / name).write_bytes(blob)
            put(self.original / 'src' / name, 'old captured include')
            outputs[name] = dict(bytes=len(blob), sha256=hashlib.sha256(blob).hexdigest())
        put(self.generated / 'report.json', json.dumps(dict(status='complete', outputs=outputs)))
        self.built = self.work / 'compile/attempts/v507/build'
        self.fresh = ''.join('fresh ' + n for n in SHADER_INCLUDES)


class CompileStage(unittest.TestCase):
    def tree(self, **kw):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        return Tree(Path(tmp.name).resolve(), **kw)

    def test_isolates_writes_and_connects_all_three_producers(self):
        t = self.tree()
        m.run_compile(t.root, t.work, t.generated)
        self.assertEqual((t.original / 'build/library').read_text(), 'untouched reference')
        self.assertEqual((t.built / 'library').read_text(), 'recipe fixture' + 'mapping fixture' + t.fresh)
        observed = json.loads((t.built / 'observed.json').read_text())
        self.assertTrue(Path(observed['source']).is_relative_to(t.work))
        self.assertTrue(Path(observed['mapping']).is_relative_to(t.work))
        self.assertEqual(observed['jobs'], 4)
        self.assertEqual((t.original / 'src' / SHADER_INCLUDES[0]).read_text(), 'old captured include')

    def with_packs(self, t):
        """A runtime file whose plan command names a private pack folder, and a native-packs stage output for it."""
        attempt = json.loads((t.original / 'attempt.json').read_text())
        attempt['files'].append({'name': 'hook.cpp', 'origin': 'tools/x/hook.cpp', 'defines': ['-DOWN=1']})
        m.write_json(t.original / 'attempt.json', attempt)
        base = t.root / 'local-analysis/base'
        old = t.root / 'local-analysis/old-packs/exposure-v5'
        m.write_json(base / 'hook-build.json', {'command': [str(t.tools / 'compiler'), '-I' + str(old), '-I/usr/include', '-c', 'hook.cpp', '-o', 'hook.o']})
        plan = json.loads((base / 'plan.json').read_text())
        plan['plans'].append({'source': str(t.root / 'tools/x/hook.cpp'), 'spec': str(base / 'hook-build.json'), 'output': str(base / 'hook.o')})
        m.write_json(base / 'plan.json', plan)
        packs = t.root / 'local-analysis/builder/run/work/native-packs'
        put(packs / 'old-packs/exposure-v5/pack.h', 'made from the discs')
        blob = (packs / 'old-packs/exposure-v5/pack.h').read_bytes()
        m.write_json(packs / 'report.json', {'status': 'complete', 'headers': {'old-packs/exposure-v5/pack.h': {'status': 'written', 'bytes': len(blob), 'sha256': hashlib.sha256(blob).hexdigest()}}})
        return packs

    def test_the_pack_folders_of_this_run_reach_the_file_that_reads_them(self):
        t = self.tree()
        packs = self.with_packs(t)
        m.run_compile(t.root, t.work, t.generated, None, packs)
        word = '-I' + str(packs / 'old-packs/exposure-v5')
        self.assertEqual(json.loads((t.work / 'compile/native-pack-includes.json').read_text()), {'hook.cpp': [word]})
        written = {f['name']: f for f in json.loads((t.work / 'compile/attempts/v507/attempt.json').read_text())['files']}
        self.assertEqual(written['hook.cpp']['defines'], ['-DOWN=1', word])
        self.assertEqual((t.built / 'library').read_text(), 'recipe fixture' + 'mapping fixture' + t.fresh)

    def test_two_header_stages_both_reach_the_file_in_the_order_given(self):
        t = self.tree()
        packs = self.with_packs(t)
        code = t.root / 'local-analysis/builder/run/work/code-headers'
        put(code / 'old-packs/exposure-v5/hooked.h', 'made from this run\'s code')
        blob = (code / 'old-packs/exposure-v5/hooked.h').read_bytes()
        m.write_json(code / 'report.json', {'status': 'complete', 'headers': {'old-packs/exposure-v5/hooked.h': {'status': 'written', 'bytes': len(blob), 'sha256': hashlib.sha256(blob).hexdigest()}}})
        m.run_compile(t.root, t.work, t.generated, None, [code, packs])
        written = {f['name']: f for f in json.loads((t.work / 'compile/attempts/v507/attempt.json').read_text())['files']}
        self.assertEqual(written['hook.cpp']['defines'], ['-DOWN=1', '-I' + str(code / 'old-packs/exposure-v5'), '-I' + str(packs / 'old-packs/exposure-v5')])
        put(code / 'old-packs/exposure-v5/hooked.h', 'changed afterwards')
        t2 = self.tree()
        with self.assertRaisesRegex(ValueError, 'hash differs'):          # the second stage's report is held to as well
            packs2 = self.with_packs(t2)
            code2 = t2.root / 'local-analysis/builder/run/work/code-headers'
            put(code2 / 'old-packs/exposure-v5/hooked.h', 'changed afterwards')
            m.write_json(code2 / 'report.json', {'status': 'complete', 'headers': {'old-packs/exposure-v5/hooked.h': {'status': 'written', 'bytes': len(blob), 'sha256': hashlib.sha256(blob).hexdigest()}}})
            m.run_compile(t2.root, t2.work, t2.generated, None, [packs2, code2])

    def test_without_a_pack_stage_nothing_is_added(self):
        t = self.tree()
        self.with_packs(t)
        m.run_compile(t.root, t.work, t.generated)
        self.assertEqual(json.loads((t.work / 'compile/native-pack-includes.json').read_text()), {})
        written = {f['name']: f for f in json.loads((t.work / 'compile/attempts/v507/attempt.json').read_text())['files']}
        self.assertEqual(written['hook.cpp']['defines'], ['-DOWN=1'])

    def with_corpus_reader(self, t):
        """A runtime file whose plan command has the old corpus folder on its include path."""
        attempt = json.loads((t.original / 'attempt.json').read_text())
        attempt['files'].append({'name': 'reader.cpp', 'origin': 'tools/x/reader.cpp'})
        m.write_json(t.original / 'attempt.json', attempt)
        base = t.root / 'local-analysis/base'
        m.write_json(base / 'reader-build.json', {'command': [str(t.tools / 'compiler'), '-I' + str(t.corpus), '-c', 'reader.cpp', '-o', 'reader.o']})
        plan = json.loads((base / 'plan.json').read_text())
        plan['plans'].append({'source': str(t.root / 'tools/x/reader.cpp'), 'spec': str(base / 'reader-build.json'), 'output': str(base / 'reader.o')})
        m.write_json(base / 'plan.json', plan)

    def test_the_generators_headers_are_read_from_this_run(self):
        t = self.tree()
        self.with_corpus_reader(t)
        packs = self.with_packs(t)
        m.run_compile(t.root, t.work, t.generated, None, packs)
        generated = t.work / 'compile/generated-headers'
        self.assertEqual(sorted(p.name for p in generated.iterdir()), ['ppc_config.h', 'ppc_context.h', 'ppc_recomp_shared.h'])
        # the project's support header stands beside them (it includes them by a quoted name), and it is the supplied one
        self.assertEqual((generated / 'ppc_context.h').read_text(), 'the project header of the old folder')
        record = json.loads((t.work / 'compile/generated-headers.json').read_text())
        self.assertEqual(sorted(record['generated']), ['ppc_config.h', 'ppc_recomp_shared.h'])
        self.assertEqual(record['generated']['ppc_config.h'], hashlib.sha256(b'generated ppc_config.h').hexdigest())
        self.assertEqual(record['supplied'], {'ppc_context.h': {'from': str(t.corpus / 'ppc_context.h'),
                         'sha256': hashlib.sha256(b'the project header of the old folder').hexdigest()}})
        written = {f['name']: f for f in json.loads((t.work / 'compile/attempts/v507/attempt.json').read_text())['files']}
        self.assertEqual(written['reader.cpp']['defines'], ['-I' + str(generated)])          # in front of the old folder
        self.assertEqual(written['hook.cpp']['defines'], ['-DOWN=1', '-I' + str(packs / 'old-packs/exposure-v5')])   # its plan does not name the corpus folder
        pinned = t.work / 'compile/pinned-ppc'
        self.assertEqual((pinned / 'ppc_recomp_shared.h').read_text(), 'generated ppc_recomp_shared.h')
        self.assertEqual((pinned / 'ppc_context.h').read_text(), 'the project header of the old folder')
        self.assertEqual((pinned / 'ppc_config.h').read_text(), 'generated ppc_config.h')
        # nothing else of the old folder is copied beside the pinned chunks, where it would pass for a file of this run
        self.assertEqual(sorted(p.name for p in pinned.iterdir()), ['ppc_config.h', 'ppc_context.h', 'ppc_recomp.0.cpp', 'ppc_recomp_shared.h'])

    def test_the_mapping_unit_and_the_chunks_search_this_runs_headers_first(self):
        t = self.tree()
        m.run_compile(t.root, t.work, t.generated)
        generated = str(t.work / 'compile/generated-headers')
        command = json.loads((t.work / 'compile/mapping-proof.json').read_text())['command']
        quotes = [command[i + 1] for i, w in enumerate(command) if w == '-iquote']
        self.assertEqual(quotes, [generated, str(t.corpus)])
        written = json.loads((t.work / 'compile/attempts/v507/attempt.json').read_text())
        self.assertEqual(written['ppc_defines'][-2:], ['-iquote', generated])

    def test_a_generated_header_that_differs_from_the_plans_folder_is_refused(self):
        t = self.tree()
        put(t.work / 'codegen/ppc/ppc_config.h', 'another generator wrote this')
        with self.assertRaisesRegex(ValueError, 'hash differs'):
            m.run_compile(t.root, t.work, t.generated)
        self.assertFalse((t.built / 'library').exists())

    def test_the_generated_header_folder_goes_in_front_of_the_pack_folders(self):
        t = self.tree()
        packs = self.with_packs(t)
        spec = json.loads((t.root / 'local-analysis/base/hook-build.json').read_text())
        spec['command'][1:1] = ['-I' + str(t.corpus)]
        m.write_json(t.root / 'local-analysis/base/hook-build.json', spec)
        m.run_compile(t.root, t.work, t.generated, None, packs)
        written = {f['name']: f for f in json.loads((t.work / 'compile/attempts/v507/attempt.json').read_text())['files']}
        self.assertEqual(written['hook.cpp']['defines'], ['-DOWN=1', '-I' + str(t.work / 'compile/generated-headers'), '-I' + str(packs / 'old-packs/exposure-v5')])

    def test_an_unfinished_pack_stage_is_refused(self):
        t = self.tree()
        packs = self.with_packs(t)
        report = json.loads((packs / 'report.json').read_text())
        m.write_json(packs / 'report.json', dict(report, status='differs'))
        with self.assertRaisesRegex(ValueError, 'did not finish'):
            m.run_compile(t.root, t.work, t.generated, None, packs)
        self.assertFalse((t.built / 'library').exists())

    def test_a_pack_stage_that_wrote_nothing_is_refused(self):
        t = self.tree()
        packs = self.with_packs(t)
        m.write_json(packs / 'report.json', {'status': 'complete', 'headers': {}})
        with self.assertRaisesRegex(ValueError, 'did not finish'):
            m.run_compile(t.root, t.work, t.generated, None, packs)

    def test_a_pack_header_changed_after_its_stage_is_refused(self):
        t = self.tree()
        packs = self.with_packs(t)
        put(packs / 'old-packs/exposure-v5/pack.h', 'changed afterwards')
        with self.assertRaisesRegex(ValueError, 'hash differs'):
            m.run_compile(t.root, t.work, t.generated, None, packs)
        self.assertFalse((t.built / 'library').exists())

    def test_changed_mapping_is_refused_before_link(self):
        t = self.tree()
        put(t.work / 'codegen/ppc/ppc_func_mapping.cpp', 'changed mapping')
        with self.assertRaises(ValueError):
            m.run_compile(t.root, t.work, t.generated)
        self.assertFalse((t.built / 'library').exists())
        self.assertEqual((t.original / 'build/library').read_text(), 'untouched reference')

    def test_changed_generated_table_is_refused(self):
        t = self.tree()
        (t.generated / SHADER_INCLUDES[0]).write_text('bad')
        with self.assertRaises(ValueError):
            m.run_compile(t.root, t.work, t.generated)
        self.assertFalse((t.built / 'library').exists())

    def test_incomplete_shader_stage_is_refused(self):
        t = self.tree()
        report = json.loads((t.generated / 'report.json').read_text())
        report['status'] = 'incomplete'
        (t.generated / 'report.json').write_text(json.dumps(report))
        with self.assertRaises(ValueError):
            m.run_compile(t.root, t.work, t.generated)

    GAME = ("0000000000000000 W _Z5sub_8R10PPCContextPh\n"                 # written out of name order on purpose
            "0000000000000010 W _Z12sub_82000010R10PPCContextPh\n"
            "0000000000000020 T __imp__sub_82000010\n"
            "0000000000000030 W _Z12sub_82000020R10PPCContextPh\n"          # the runtime hooks this one
            "0000000000000040 W _Z14__savegprlr_14R10PPCContextPh\n"
            "0000000000000050 W _Z12sub_82000040R10PPCContextPh\n"          # also weak in a runtime object that comes first
            "0000000000000060 W _Z9sub_8R10PPCContextPh\n"                  # the length in the name is not the name's
            "0000000000000070 W _ZN6recomp6helperEv\n"                      # weak, and no wrapper
            "0000000000000080 t _Z12sub_82000050R10PPCContextPh\n"
            "                 U _Z12sub_82000060R10PPCContextPh\n")
    OBJECTS = {'ppc_recomp.0.cpp.o': GAME,
               'a_runtime.cpp.o': "0000000000000000 W _Z12sub_82000040R10PPCContextPh\n0000000000000010 W _Z12sub_82000030R10PPCContextPh\n",
               'hook.cpp.o': "0000000000000000 T _Z12sub_82000020R10PPCContextPh\n",
               'ppc_recomp.0.cpp.d': "0000000000000000 T _Z12sub_82000010R10PPCContextPh\n"}      # not an object: never read
    SCRIPT = ('/* code-reach: bind unhooked generated weak wrappers to their bodies (layout-only, see redirect_script.py) */\n'
              '_Z12sub_82000010R10PPCContextPh = __imp__sub_82000010;\n'
              '_Z14__savegprlr_14R10PPCContextPh = __imp____savegprlr_14;\n'
              '_Z5sub_8R10PPCContextPh = __imp__sub_8;\n')

    def with_script(self, t, objects=None, extra=()):
        """The attempt names an earlier build's linker script among its link words; the build writes these objects."""
        old = t.root / 'local-analysis/attempts/v462/redirect-v462.ld'
        put(old, 'THE SCRIPT OF AN EARLIER BUILD')
        attempt = json.loads((t.original / 'attempt.json').read_text())
        attempt['link_extra'] = ['-Wl,--own-word', str(old), '-Wl,--after', 'libextra.a', *extra]     # another link input that is no script
        m.write_json(t.original / 'attempt.json', attempt)
        put(t.original / 'src/fake-objects.json', json.dumps(self.OBJECTS if objects is None else objects))
        return old

    def test_the_linker_script_is_made_from_this_builds_objects_and_linked_in_a_second_build(self):
        t = self.tree()
        old = self.with_script(t)
        m.run_compile(t.root, t.work, t.generated)
        made = t.work / 'compile/redirect.ld'
        self.assertEqual(made.read_text(), self.SCRIPT)
        calls = json.loads((t.built / 'calls.json').read_text())
        self.assertEqual(calls, [['-Wl,--own-word', '-Wl,--after', 'libextra.a'],                       # first: no script at all
                                 ['-Wl,--own-word', str(made), '-Wl,--after', 'libextra.a']])          # then this run's, in the old one's place
        self.assertEqual((t.built / 'library').read_text(), 'recipe fixture' + 'mapping fixture' + t.fresh + '|linked script:' + self.SCRIPT)
        self.assertEqual(old.read_text(), 'THE SCRIPT OF AN EARLIER BUILD')
        record = json.loads((t.work / 'compile/redirect.json').read_text())
        text = lambda extra: hashlib.sha256(('recipe fixture' + 'mapping fixture' + t.fresh + extra).encode()).hexdigest()
        self.assertEqual(record, {'script': str(made), 'sha256': hashlib.sha256(self.SCRIPT.encode()).hexdigest(), 'assignments': 3,
                                  'hooked_wrappers_left_alone': 1, 'replaces_link_word': str(old),
                                  'linked_first_without_it': text(''), 'linked_with_it': text('|linked script:' + self.SCRIPT)})

    def test_the_script_does_not_depend_on_the_order_the_objects_are_listed_in(self):
        t = self.tree()
        folder = t.root / 'objects'
        for name, text in self.OBJECTS.items():
            put(folder / name, text)
        objects = [folder / n for n in ('ppc_recomp.0.cpp.o', 'hook.cpp.o', 'a_runtime.cpp.o')]
        self.assertEqual(m.redirect_script(t.tools / 'llvm-nm', objects), (self.SCRIPT, 1))
        self.assertEqual(m.redirect_script(t.tools / 'llvm-nm', reversed(objects)), (self.SCRIPT, 1))
        # without the runtime object that defines it first, the wrapper is the game code's and is bound
        self.assertIn('_Z12sub_82000040R10PPCContextPh = __imp__sub_82000040;\n', m.redirect_script(t.tools / 'llvm-nm', objects[:2])[0])
        self.assertEqual(m.redirect_script(t.tools / 'llvm-nm', objects[1:]), (m.REDIRECT_HEAD + '\n', 0))

    def test_the_portable_link_takes_the_library_of_the_second_build(self):
        t = self.tree()
        self.with_script(t)
        m.run_compile(t.root, t.work, t.generated, ID)
        made = str(t.work / 'compile/redirect.ld')
        calls = json.loads((t.built / 'calls.json').read_text())
        self.assertEqual([c[:3] for c in calls], [['-Wl,--own-word', '-Wl,--after', 'libextra.a'], ['-Wl,--own-word', made, '-Wl,--after']])
        self.assertEqual([c[-2:] for c in calls], [['-Wl,--strip-debug', '-Wl,--build-id=0x' + ID]] * 2)
        portable = json.loads((t.work / 'compile/portable.json').read_text())
        self.assertEqual(m.sha(Path(portable['linked'])), json.loads((t.work / 'compile/redirect.json').read_text())['linked_with_it'])
        self.assertIn('|linked script:', Path(portable['library']).read_text())

    def test_without_a_linker_script_there_is_one_build_and_no_script(self):
        t = self.tree()
        put(t.original / 'src/fake-objects.json', json.dumps(self.OBJECTS))
        m.run_compile(t.root, t.work, t.generated)
        self.assertEqual(json.loads((t.built / 'calls.json').read_text()), [['-Wl,--own-word']])
        self.assertFalse((t.work / 'compile/redirect.ld').exists())
        self.assertFalse((t.work / 'compile/redirect.json').exists())

    def test_two_linker_scripts_are_refused_before_anything_is_built(self):
        t = self.tree()
        self.with_script(t, extra=[str(t.root / 'another.ld')])
        with self.assertRaisesRegex(ValueError, 'more than one linker script'):
            m.run_compile(t.root, t.work, t.generated)
        self.assertFalse((t.built / 'library').exists())

    def test_an_object_nm_cannot_read_stops_the_stage(self):
        t = self.tree()
        self.with_script(t, objects=dict(self.OBJECTS, **{'broken.cpp.o': 'BROKEN'}))
        with self.assertRaisesRegex(ValueError, 'nm failed on broken.cpp.o'):
            m.run_compile(t.root, t.work, t.generated)
        self.assertFalse((t.work / 'compile/redirect.json').exists())
        self.assertEqual(len(json.loads((t.built / 'calls.json').read_text())), 1)       # no second link

    def with_archives(self, t, made=None):
        """The base link plan names two supplied decoder archives; this run built its own in work/xma."""
        base = t.root / 'local-analysis/base'
        supplied = t.root / 'local-analysis/native-xma/android-artifacts'
        link = json.loads((base / 'link-plan.json').read_text())
        for name in ('libavcodec.a', 'libavutil.a'):
            put(supplied / name, 'archive ' + name)
            link['command'].append(str(supplied / name))
            link['inputs'][str(supplied / name)] = m.sha(supplied / name)
            text = (made or {}).get(name, 'archive ' + name)
            if text is not None:
                put(t.work / 'xma' / name[:-2] / name, text)
        link['command'].append('-Wl,--exclude-libs,libavcodec.a')        # an option that ends in an archive's name
        m.write_json(base / 'link-plan.json', link)
        return supplied, t.work / 'xma'

    def test_the_decoder_archives_of_this_run_take_the_place_of_the_supplied_ones(self):
        t = self.tree()
        supplied, made = self.with_archives(t)
        m.run_compile(t.root, t.work, t.generated, None, None, made)
        link = json.loads((t.work / 'compile/base-plan/link-plan.json').read_text())
        codec, util = str(made / 'libavcodec/libavcodec.a'), str(made / 'libavutil/libavutil.a')
        self.assertEqual(link['command'][-3:], [codec, util, '-Wl,--exclude-libs,libavcodec.a'])
        self.assertEqual(link['command'][0], str(t.tools / 'driver'))
        self.assertFalse([w for w in link['command'] if 'android-artifacts' in w])
        self.assertFalse([k for k in link['inputs'] if 'android-artifacts' in k])
        self.assertEqual(link['inputs'][codec], hashlib.sha256(b'archive libavcodec.a').hexdigest())
        self.assertEqual(link['inputs'][util], hashlib.sha256(b'archive libavutil.a').hexdigest())
        self.assertEqual(json.loads((t.work / 'compile/xma-archives.json').read_text()), {
            'libavcodec.a': {'made': codec, 'sha256': hashlib.sha256(b'archive libavcodec.a').hexdigest(), 'replaces_link_word': str(supplied / 'libavcodec.a')},
            'libavutil.a': {'made': util, 'sha256': hashlib.sha256(b'archive libavutil.a').hexdigest(), 'replaces_link_word': str(supplied / 'libavutil.a')}})
        self.assertTrue((t.built / 'library').exists())

    def test_without_decoder_archives_of_the_run_the_plan_keeps_the_supplied_ones(self):
        t = self.tree()
        supplied, _ = self.with_archives(t)
        m.run_compile(t.root, t.work, t.generated)
        link = json.loads((t.work / 'compile/base-plan/link-plan.json').read_text())
        self.assertEqual(link['command'][-3:-1], [str(supplied / 'libavcodec.a'), str(supplied / 'libavutil.a')])
        self.assertFalse((t.work / 'compile/xma-archives.json').exists())

    def test_a_decoder_archive_that_is_not_the_pinned_one_is_refused(self):
        for name in ('libavcodec.a', 'libavutil.a'):
            t = self.tree()
            _, made = self.with_archives(t, {name: 'built from another revision'})
            with self.assertRaisesRegex(ValueError, 'hash differs: ' + name):
                m.run_compile(t.root, t.work, t.generated, None, None, made)
            self.assertFalse((t.built / 'library').exists())

    def test_a_missing_decoder_archive_is_refused(self):
        t = self.tree()
        _, made = self.with_archives(t, {'libavutil.a': None})
        with self.assertRaisesRegex(ValueError, 'no libavutil.a'):
            m.run_compile(t.root, t.work, t.generated, None, None, made)
        self.assertFalse((t.built / 'library').exists())

    def test_the_plan_must_name_each_archive_once_with_its_hash(self):
        link = {'command': ['driver', '/a/libavcodec.a', '/a/libavutil.a'], 'inputs': {'/a/libavcodec.a': 'x', '/a/libavutil.a': 'y'}}
        for change, _ in ((lambda l: l['command'].append('/b/libavcodec.a'), 'twice'), (lambda l: l['command'].remove('/a/libavutil.a'), 'missing'),
                          (lambda l: l['inputs'].pop('/a/libavutil.a'), 'no hash')):
            bad = json.loads(json.dumps(link))
            change(bad)
            with self.assertRaisesRegex(ValueError, 'exactly once, with its hash'):
                m.with_archives(bad, Path('/nowhere'))
        t = self.tree()
        for name in ('libavcodec.a', 'libavutil.a'):
            put(t.root / 'made' / name[:-2] / name, name)
            link['inputs']['/a/' + name] = hashlib.sha256(name.encode()).hexdigest()
        before = json.loads(json.dumps(link))
        changed, _ = m.with_archives(link, t.root / 'made')
        self.assertEqual(link, before)                                 # the plan handed in is not changed
        self.assertEqual(changed['command'], ['driver', str(t.root / 'made/libavcodec/libavcodec.a'), str(t.root / 'made/libavutil/libavutil.a')])

    def test_the_command_line_hands_the_stage_outputs_on(self):
        t = self.tree()
        seen, before, argv = [], m.run_compile, sys.argv
        m.run_compile = lambda *a: seen.append(a)
        sys.argv = ['compile_rehearsal.py', 'compile', '--repo', str(t.root), '--work', str(t.work), '--shader-extras', str(t.generated),
                    '--portable-build-id', ID, '--native-packs', str(t.root / 'a'), '--native-packs', str(t.root / 'b'), '--xma-archives', str(t.root / 'xma')]
        try:
            m.main()
            self.assertEqual(seen, [(t.root, t.work, t.generated, ID, [t.root / 'a', t.root / 'b'], t.root / 'xma', True, False, 'v507', None)])
            sys.argv = sys.argv[:8]
            m.main()
            self.assertEqual(seen[1], (t.root, t.work, t.generated, None, None, None, True, False, 'v507', None))
            sys.argv = sys.argv + ['--without-measurements']
            m.main()
            self.assertEqual(seen[2], (t.root, t.work, t.generated, None, None, None, False, False, 'v507', None))
            sys.argv = sys.argv[:8] + ['--build-kept-objects']
            m.main()
            self.assertEqual(seen[3], (t.root, t.work, t.generated, None, None, None, True, True, 'v507', None))
            sys.argv = sys.argv[:8] + ['--attempt', 'v806', '--shader-fixes', str(t.root / 'fixes')]   # the Thor line's build
            m.main()
            self.assertEqual(seen[4], (t.root, t.work, t.generated, None, None, None, True, False, 'v806', t.root / 'fixes'))
        finally:
            m.run_compile, sys.argv = before, argv

    PROFILE = '-fprofile-use=/old/pgo/round7.profdata'
    ORDER = '-Wl,--symbol-ordering-file=/old/layout/sym.txt'

    def with_measurements(self, t):
        """The attempt compiles some chunks with a profile of an earlier build and links by a symbol order made from it."""
        attempt = json.loads((t.original / 'attempt.json').read_text())
        attempt['ppc_chunk_extra'] = {'chunks': [0], 'flags': [self.PROFILE, '-Wno-profile-instr-unprofiled', '-Wno-profile-instr-out-of-date',
                                                             '-mllvm', '-pgso=false', '-O3']}
        attempt['link_extra'] = ['-Wl,--own-word', self.ORDER, '-Wl,--no-warn-symbol-ordering', '-Wl,--last-word']
        # single runtime files carry the profile too, in a key of their own
        attempt['files'].append({'name': 'runtime.cpp', 'origin': 'tools/x/runtime.cpp', 'defines': ['-DOWN=1'],
                                 'extra_flags': ['-fno-own', self.PROFILE, '-Wno-profile-instr-unprofiled', '-Wno-profile-instr-out-of-date']})
        m.write_json(t.original / 'attempt.json', attempt)

    def test_the_measurements_of_an_earlier_build_can_be_left_out(self):
        t = self.tree()
        self.with_measurements(t)
        m.run_compile(t.root, t.work, t.generated, None, None, None, False)
        written = json.loads((t.work / 'compile/attempts/v507/attempt.json').read_text())
        self.assertEqual(written['ppc_chunk_extra'], {'chunks': [0], 'flags': ['-mllvm', '-pgso=false', '-O3']})      # every other flag, in its place
        self.assertEqual(written['link_extra'], ['-Wl,--own-word', '-Wl,--last-word'])
        self.assertEqual(json.loads((t.built / 'observed.json').read_text())['link_extra'], ['-Wl,--own-word', '-Wl,--last-word'])
        self.assertEqual(json.loads((t.work / 'compile/without-measurements.json').read_text()),
                         {'profile': [self.PROFILE], 'symbol_order': [self.ORDER]})                       # named once, though twenty places may carry it
        runtime = [f for f in written['files'] if f['name'] == 'runtime.cpp'][0]
        self.assertEqual((runtime['extra_flags'], runtime['defines']), (['-fno-own'], ['-DOWN=1']))
        text = (t.work / 'compile/attempts/v507/attempt.json').read_text()
        self.assertNotIn('profile', text)
        self.assertNotIn('symbol-ordering', text)

    def test_by_default_the_attempts_measurements_stay(self):
        t = self.tree()
        self.with_measurements(t)
        m.run_compile(t.root, t.work, t.generated)
        written = json.loads((t.work / 'compile/attempts/v507/attempt.json').read_text())
        self.assertEqual(written['ppc_chunk_extra']['flags'][:3], [self.PROFILE, '-Wno-profile-instr-unprofiled', '-Wno-profile-instr-out-of-date'])
        self.assertEqual(written['link_extra'], ['-Wl,--own-word', self.ORDER, '-Wl,--no-warn-symbol-ordering', '-Wl,--last-word'])
        self.assertFalse((t.work / 'compile/without-measurements.json').exists())

    def test_a_measurement_named_in_a_place_the_stage_does_not_know_is_refused(self):
        spec = {'files': [{'name': 'a.cpp', 'defines': ['-DX']}], 'ppc_defines': ['-O2'], 'link_extra': []}
        self.assertEqual(m.without_measurements(spec), (spec, {'profile': [], 'symbol_order': []}))      # nothing to leave out: unchanged
        for change in (lambda s: s['files'][0]['defines'].append('-fprofile-use=/x'), lambda s: s['ppc_defines'].append('-fprofile-instr-use=/x'),
                       lambda s: s['ppc_defines'].append('-fprofile-sample-use=/x'), lambda s: s.update(some_new_key={'deep': ['-fprofile-use=/y']}),
                       lambda s: s['files'][0].update(link_words=['-Wl,--symbol-ordering-file=/z']), lambda s: s.update(launcher='-fprofile-use=/w')):
            bad = json.loads(json.dumps(spec))
            change(bad)
            with self.assertRaisesRegex(ValueError, 'still names a measurement'):
                m.without_measurements(bad)
        other = {'files': [], 'ppc_chunk_extra': {'chunks': [1], 'flags': ['-fprofile-instr-use=/p', '-O3', '-fprofile-sample-use=/q']},
                 'link_extra': ['-Wl,--symbol-ordering-file=/o', 'x.ld']}
        before = json.loads(json.dumps(other))
        out, named = m.without_measurements(other)
        self.assertEqual(other, before)                                       # the attempt handed in is not changed
        self.assertEqual((out['ppc_chunk_extra']['flags'], out['link_extra']), (['-O3'], ['x.ld']))
        self.assertEqual(named, {'profile': ['-fprofile-instr-use=/p', '-fprofile-sample-use=/q'], 'symbol_order': ['-Wl,--symbol-ordering-file=/o']})

    def with_kept(self, t, source_text='kept source', name='kept.cpp', where='android/app/src/main/cpp'):
        """The base plan has a runtime file the attempt does not build; its object was kept from the base build."""
        base = t.root / 'local-analysis/base'
        source = t.root / where / name
        put(source, source_text)
        put(base / (name + '.o'), 'kept source')                                  # what the base build made of it
        m.write_json(base / (name + '-build.json'), {'command': [str(t.tools / 'compiler'), '-I' + str(t.root / 'local-analysis/native-xma/ffmpeg'),
                                                             '-c', str(source), '-o', str(base / (name + '.o'))]})
        plan = json.loads((base / 'plan.json').read_text())
        plan['plans'].append({'source': str(source), 'spec': str(base / (name + '-build.json')), 'output': str(base / (name + '.o'))})
        m.write_json(base / 'plan.json', plan)
        link = json.loads((base / 'link-plan.json').read_text())
        link['command'].append(str(base / (name + '.o')))
        link['inputs'][str(base / (name + '.o'))] = m.sha(base / (name + '.o'))
        m.write_json(base / 'link-plan.json', link)
        return source, base / (name + '.o')

    def test_the_kept_runtime_objects_are_compiled_in_this_run_and_must_be_the_kept_ones(self):
        t = self.tree()
        source, old = self.with_kept(t)
        m.run_compile(t.root, t.work, t.generated, None, None, None, True, True)
        written = {f['name']: f for f in json.loads((t.work / 'compile/attempts/v507/attempt.json').read_text())['files']}
        rel = 'android/app/src/main/cpp/kept.cpp'
        self.assertEqual(written['kept.cpp'], {'name': 'kept.cpp', 'origin': rel, 'source': rel})
        made = t.built / 'obj/kept.cpp.o'
        self.assertEqual(json.loads((t.work / 'compile/kept-objects.json').read_text()), {'kept.cpp': {
            'source': rel, 'object': str(made), 'sha256': hashlib.sha256(b'kept source').hexdigest(), 'replaces_link_word': str(old)}})
        self.assertEqual(m.kept_runtime_files(t.root, json.loads((t.original / 'attempt.json').read_text())), [('kept.cpp', rel, str(old))])

    def test_a_kept_object_that_comes_out_otherwise_fails_the_stage(self):
        t = self.tree()
        self.with_kept(t, source_text='the source has moved on')
        with self.assertRaisesRegex(ValueError, 'hash differs: kept.cpp.o'):
            m.run_compile(t.root, t.work, t.generated, None, None, None, True, True)
        self.assertFalse((t.work / 'compile/kept-objects.json').exists())

    def test_by_default_the_kept_objects_are_linked_as_they_are(self):
        t = self.tree()
        self.with_kept(t, source_text='the source has moved on')
        m.run_compile(t.root, t.work, t.generated)
        names = [f['name'] for f in json.loads((t.work / 'compile/attempts/v507/attempt.json').read_text())['files']]
        self.assertNotIn('kept.cpp', names)
        self.assertFalse((t.work / 'compile/kept-objects.json').exists())

    def test_a_kept_file_gets_the_decoder_headers_of_this_run(self):
        t = self.tree()
        self.with_kept(t)
        self.with_corpus_reader(t)                                                 # a file whose plan does not name the decoder's headers
        _, made = self.with_archives(t)
        m.run_compile(t.root, t.work, t.generated, None, None, made, True, True)
        written = {f['name']: f for f in json.loads((t.work / 'compile/attempts/v507/attempt.json').read_text())['files']}
        self.assertEqual(written['kept.cpp']['defines'], ['-I' + str(made)])        # in front of the private copy's folder
        self.assertEqual(written['reader.cpp']['defines'], ['-I' + str(t.work / 'compile/generated-headers')])
        t2 = self.tree()
        self.with_kept(t2)
        m.run_compile(t2.root, t2.work, t2.generated, None, None, None, True, True)
        written = {f['name']: f for f in json.loads((t2.work / 'compile/attempts/v507/attempt.json').read_text())['files']}
        self.assertNotIn('defines', written['kept.cpp'])                           # no decoder stage in the run: the plan's own folder

    def test_a_kept_file_must_be_a_source_of_the_repository(self):
        for where in ('local-analysis/private-src',):
            t = self.tree()
            self.with_kept(t, where=where)
            with self.assertRaisesRegex(ValueError, 'not a source of the repository: kept.cpp'):
                m.run_compile(t.root, t.work, t.generated, None, None, None, True, True)
        t = self.tree()
        self.with_kept(t)
        attempt = json.loads((t.original / 'attempt.json').read_text())
        attempt['files'].append({'name': 'other.cpp', 'template': 'kept.cpp', 'origin': 'tools/x/other.cpp'})   # the attempt builds it itself, under another name
        self.assertEqual(m.kept_runtime_files(t.root, attempt), [])

    def test_without_a_build_id_the_link_is_the_attempts_own(self):
        t = self.tree()
        m.run_compile(t.root, t.work, t.generated)
        self.assertEqual(json.loads((t.built / 'observed.json').read_text())['link_extra'], ['-Wl,--own-word'])
        self.assertFalse((t.built / 'libgame-portable.so').exists())
        self.assertFalse((t.work / 'compile/portable.json').exists())
        self.assertFalse((t.tools / 'objcopy-argv.json').exists())

    def test_portable_library_is_linked_with_the_words_and_stripped_of_name_strings(self):
        t = self.tree()
        m.run_compile(t.root, t.work, t.generated, ID)
        self.assertEqual(json.loads((t.built / 'observed.json').read_text())['link_extra'],
                         ['-Wl,--own-word', '-Wl,--strip-debug', '-Wl,--build-id=0x' + ID])
        portable = t.built / 'libgame-portable.so'
        self.assertEqual(json.loads((t.tools / 'objcopy-argv.json').read_text()),
                         ['--remove-section=.comment', str(t.built / 'library'), str(portable)])
        self.assertEqual(portable.read_text(), (t.built / 'library').read_text() + ' [no comment]')
        record = json.loads((t.work / 'compile/portable.json').read_text())
        self.assertEqual(record['sha256'], hashlib.sha256(portable.read_bytes()).hexdigest())
        self.assertEqual((record['build_id'], record['library']), (ID, str(portable)))

    def test_a_bad_build_id_stops_before_anything_is_written(self):
        t = self.tree()
        with self.assertRaises(ValueError):
            m.run_compile(t.root, t.work, t.generated, ID[:-2])
        self.assertFalse((t.work / 'compile').exists())

    def test_a_failing_objcopy_is_an_error_and_leaves_no_record(self):
        t = self.tree(objcopy_exit=3)
        with self.assertRaises(ValueError):
            m.run_compile(t.root, t.work, t.generated, ID)
        self.assertFalse((t.work / 'compile/portable.json').exists())

    def test_a_host_without_clones_copies_the_same_bytes(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        src = Path(tmp.name) / 'source.bin'; src.write_bytes(b'\r\n\x00stage bytes\x1a')
        with mock.patch.object(m.sys, 'platform', 'win32'), \
             mock.patch.object(m.subprocess, 'run', side_effect=AssertionError('no Unix copy command on another host')):
            m.clone(src, Path(tmp.name) / 'deep/er/copy.bin')
        self.assertEqual((Path(tmp.name) / 'deep/er/copy.bin').read_bytes(), src.read_bytes())
        # on this Mac the clone command is still what runs (no bytes copied)
        seen = []
        with mock.patch.object(m.sys, 'platform', 'darwin'), mock.patch.object(m.subprocess, 'run', side_effect=lambda *a, **k: seen.append((a, k))), \
             mock.patch.object(m.shutil, 'copyfile', side_effect=AssertionError('a clone, not a copy, on the Mac')):
            m.clone(src, Path(tmp.name) / 'clone.bin')
        self.assertEqual(seen, [((['/bin/cp', '-c', str(src), str(Path(tmp.name) / 'clone.bin')],), {'check': True})])


if __name__ == '__main__':
    unittest.main()
