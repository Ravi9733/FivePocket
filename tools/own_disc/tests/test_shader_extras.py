"""Synthetic microcode -> selector -> patch -> include connections; no game fixtures."""
import argparse
import contextlib
import importlib.util
import io
import json
import os
import subprocess
from pathlib import Path
import struct
import sys
import tempfile
import unittest

HERE=Path(__file__).resolve().parent  # (defaults: the tree's own files; the mutant runner points these at changed copies)
SOURCE=Path(os.environ.get('EXTRAS_SOURCE',HERE.parent/'build_shader_extras.py'))
sys.path.insert(0,str(Path(os.environ.get('EXTRAS_SUPPORT',HERE.parent))))
spec=importlib.util.spec_from_file_location('extras',SOURCE);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

def code(*fetches):
    count=len(fetches);seq=sum(1<<(i*2) for i in range(count))
    return struct.pack('>3I',1|(count<<12)|(seq<<16),0x2000,0)+b''.join(struct.pack('>3I',*w) for w in fetches)

class Checks(unittest.TestCase):
    def test_gradient_selector(self):
        self.assertEqual(m.classify(code((18,0,0))),('gradient',1,0))
        self.assertEqual(m.classify(code((19,0,0))),(None,0,0))
    def test_lod_connection(self):
        self.assertEqual(m.classify(code((24,0,0),(1,1<<29,0))),('reglod',1,1))
        with self.assertRaises(ValueError):m.classify(code((24,0,0)))
    def test_denorm_selector(self):
        self.assertEqual(m.classify(code((1|1<<25,0,0))),('denorm',1,0))
        with self.assertRaises(ValueError):m.classify(code((1|1<<25,0,0),(1,0,0)))
    def test_extent(self):
        with self.assertRaises(ValueError):m.fetches(code((18,0,0))[:-12])
    def test_gradient_delta(self):
        m.gradient_change('before\nafter','before\nr=ddx(x)+ddy(y);\nafter',1)
        with self.assertRaises(ValueError):m.gradient_change('before\nafter','altered\nr=ddx(x)+ddy(y);\nafter',1)
    def test_denorm_connection(self):
        old=m.rules.ANCHOR+';\nvoid shaderMain(){tfetch2D(a);}'
        new=m.denorm_source(old)
        self.assertIn('void shaderMain(){tfetch2DUnnormalized(a);}',new)
        self.assertIn('texCoord / float2(getTexture2DDimensions(texture))',new)
    def test_record_connection(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('EXTRAS_SCRATCH')) as t:
            root=Path(t);d=root/'record';d.mkdir();(d/'container.xenos').write_bytes(b'authored')
            rec={'record':'record','container_sha256':m.sha(b'authored')};item={'sha256':m.sha(b'authored')}
            self.assertEqual(m.checked_record(root,rec,item),d)
            (d/'container.xenos').write_bytes(b'changed')
            with self.assertRaises(ValueError):m.checked_record(root,rec,item)
    def test_include_connection(self):
        blob=struct.pack('<I',0x07230203)
        out=m.include_text('denorm',[(2,blob),(1,blob)])
        self.assertLess(out.index('{0x0000000000000001ull'),out.index('{0x0000000000000002ull'))
        self.assertIn('1u,DenormOverride0',out)
        with self.assertRaises(ValueError):m.include_text('denorm',[(1,b'bad')])


ROOT=HERE.parents[2]
for folder in ('tools/iter','tools/rpf7_inventory'):sys.path.insert(0,str(ROOT/folder))
SCRATCH=Path(os.environ.get('EXTRAS_SCRATCH') or ROOT/'local-analysis/shader-stage-tests');SCRATCH.mkdir(parents=True,exist_ok=True)


def stage_fixture(root):
    """What the override step reads before it starts a program: an inventory of one pixel container of the runtime's
    family, its full translation and a one-entry pack. Authored bytes, nothing of a game."""
    from generic_shader_pack import parse_container,canonical_microcode,fnv1a64
    blob=bytearray(80);struct.pack_into('>9I',blob,0,0x102a1100,68,12,0,36,0,36,0,0);struct.pack_into('>II',blob,36,0,12);blob=bytes(blob)
    canon=canonical_microcode(parse_container(blob,'pixel')[0]);key=f'{fnv1a64(canon):016x}'
    inventory=root/'inventory';inventory.mkdir();(inventory/'a.xenos').write_bytes(blob)
    row=dict(file='a.xenos',bytes=len(blob),sha256=m.sha(blob),stage='pixel',key=key,canonical_sha256=m.sha(canon),
             origins=[dict(origin='play:/common.rpf!/shaders/fxl_final_final/a.fxc',offset=9)])
    (inventory/'inventory.json').write_text(json.dumps(dict(schema=1,source='disc',key_files_used=False,containers=[row],unresolved=[],status='complete')))
    translations=root/'translations';record=translations/'records-000'/'shader-0001';record.mkdir(parents=True)
    spv=struct.pack('<5I',0x07230203,0x10300,0,1,0)
    (record/'container.xenos').write_bytes(blob);(record/'translated.spv').write_bytes(spv);(record/'translated.hlsl').write_text('// authored')
    entry=struct.pack('<QIIII',int(key,16),1,168,len(spv)//4,0)+bytes(96)+struct.pack('<II',0,0)+bytes(24)
    packed=struct.pack('<4sIII',b'GSP2',2,1,0)+entry+spv
    (translations/'generic-shaders.bin').write_bytes(packed)
    (translations/'translation.json').write_text(json.dumps(dict(failed=0,diagnostic_limit=None,pack_sha256=m.sha(packed),
        records=[dict(container_sha256=m.sha(blob),key=key,stage='pixel',status='validated',record='records-000/shader-0001')])))
    return inventory,translations


def toolchain_folders(root):
    """A host-tools folder and an NDK as shader_toolchain names them on this host: every file there, programs startable."""
    host_tools,ndk=root/'host-tools',root/'ndk'
    named=m.shader_toolchain.names(host_tools,ndk,extras=True)
    for field,path in named.items():
        path=Path(path)
        if field=='compiler_library_directory':path.mkdir(parents=True,exist_ok=True);continue
        path.parent.mkdir(parents=True,exist_ok=True);path.write_text('authored fixture');path.chmod(0o755)
    return host_tools,ndk,named


class Toolchain(unittest.TestCase):
    def arguments(self,root,**given):
        inventory,translations=stage_fixture(root)
        host_tools,ndk,self.named=toolchain_folders(root)
        return argparse.Namespace(**{**dict(repo=ROOT,inventory=inventory,translations=translations,toolchain=None,host_tools=host_tools,ndk=ndk,
                                            output=root/'out',check=True,jobs=1),**given})

    def checked(self,args):
        with contextlib.redirect_stdout(io.StringIO()) as said:
            code=m.overlays(args)
        return code,said.getvalue()

    def test_the_toolchain_named_from_the_two_folders_is_what_the_override_step_takes(self):
        with tempfile.TemporaryDirectory(dir=SCRATCH) as t:
            root=Path(t);args=self.arguments(root)
            code,said=self.checked(args)
            self.assertEqual((code,'EXTRAS_CHECK PASS' in said),(0,True))
            # the command line takes the two folders too
            done=subprocess.run([sys.executable,str(SOURCE),'--repo',str(ROOT),'--inventory',str(args.inventory),'--translations',str(args.translations),
                                 '--host-tools',str(args.host_tools),'--ndk',str(args.ndk),'--output',str(root/'out'),'--check','--jobs','1'],capture_output=True,
                                env=dict(os.environ,PYTHONPATH=str(HERE.parent)))      # (a copy of the tool elsewhere still finds the tool's own helpers)
            self.assertEqual((done.returncode,b'EXTRAS_CHECK PASS' in done.stdout),(0,True),done.stderr)
            # the file alone is taken as before; with the pack's six fields only it is refused
            file=root/'toolchain.json';file.write_text(json.dumps(self.named))
            alone=argparse.Namespace(**{**vars(args),'toolchain':file,'host_tools':None,'ndk':None})
            self.assertEqual(self.checked(alone)[0],0)
            file.write_text(json.dumps({k:self.named[k] for k in m.shader_toolchain.BASE}))
            with self.assertRaisesRegex(ValueError,'toolchain fields refused'):m.overlays(alone)
            # both ways at once, or neither: refused
            for given in (dict(toolchain=file),dict(host_tools=None),dict(ndk=None)):
                with self.assertRaisesRegex(ValueError,'toolchain refused'):m.overlays(argparse.Namespace(**{**vars(args),**given}))
            # a program the folders should hold is missing: refused
            Path(self.named['reglod_translator']).unlink()
            with self.assertRaises(FileNotFoundError):m.overlays(args)
            self.assertFalse(args.output.exists())

    def test_the_compiler_is_started_where_it_finds_its_library(self):
        # stand-ins for the compiler and the validator: the first writes down where the loader was told to look
        with tempfile.TemporaryDirectory(dir=SCRATCH) as t:
            root=Path(t);library=root/'lib';library.mkdir()
            variable={'darwin':'DYLD_LIBRARY_PATH','win32':'PATH'}.get(sys.platform,'LD_LIBRARY_PATH')
            compiler=root/'compiler.py';validator=root/'validator.py'
            compiler.write_text(f'#!{sys.executable}\nimport os,sys\nopen(sys.argv[sys.argv.index("-Fo")+1],"w").write(os.environ.get("{variable}",""))\n')
            validator.write_text(f'#!{sys.executable}\n')
            for p in (compiler,validator):p.chmod(0o755)
            config=dict(dxc=str(compiler),spirv_validator=str(validator),compiler_library_directory=str(library))
            (root/'in.hlsl').write_text('// authored')
            seen=m.compile_hlsl(config,root,root/'in.hlsl',root/'out.spv','case').decode()
            self.assertEqual(seen.split(os.pathsep)[0],str(library))

if __name__=='__main__':unittest.main()
