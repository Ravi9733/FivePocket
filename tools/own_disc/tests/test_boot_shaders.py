"""Synthetic disc source -> binding metadata -> boot translator connection checks."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
import unittest
HERE=Path(__file__).resolve().parent  # (defaults: the tree's own files; the mutant runner points these at changed copies)
sys.path[:0]=[os.environ.get('BOOT_SUPPORT',str(HERE.parent)),str(Path(os.environ.get('BOOT_REPO',str(HERE.parents[2])))/'tools/iter')]
p=Path(os.environ.get('BOOT_SOURCE',str(HERE.parent/'build_boot_shaders.py')));spec=importlib.util.spec_from_file_location('boot',p);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
from generic_shader_pack import canonical_microcode

def code():return struct.pack('>6I',1|(1<<12)|(1<<16),0x2000,0,1<<19,0,0)

class Checks(unittest.TestCase):
    def test_binding_roundtrip(self):
        raw=code();metadata=m.binding_metadata(raw);metadata[0].update(destination=3,format=57,stride=7,swizzle=0x688)
        patched=m.bind_vertex(raw,metadata)
        self.assertEqual(m.binding_metadata(patched),metadata)
        self.assertEqual(canonical_microcode(patched),canonical_microcode(raw))
        self.assertNotEqual(raw,patched)
    def test_binding_slots(self):
        with self.assertRaises(ValueError):m.bind_vertex(code(),[])
    def test_binding_ranges(self):
        metadata=m.binding_metadata(code());metadata[0]['format']=64
        with self.assertRaises(ValueError):m.bind_vertex(code(),metadata)
    def test_extent(self):
        with self.assertRaises(ValueError):m.vertex_slots(code()[:12])
    def test_image_producer_identity(self):
        raw=code();item=dict(source='decoded-image',offset=4,bytes=len(raw),guest_sha256=m.sha(raw))
        self.assertEqual(m.source_for(item,None,[],b'head'+raw)[0],raw)
        with self.assertRaises(ValueError):m.source_for(item,None,[],b'head'+raw[:-1]+b'x')
    def test_image_extent(self):
        item=dict(source='decoded-image',offset=-1,bytes=1,guest_sha256=m.sha(b''))
        with self.assertRaisesRegex(ValueError,'extent'):m.source_for(item,None,[],b'')
    def test_ambiguous_disc_source(self):
        with self.assertRaises(ValueError):m.source_for(dict(source='disc-fxc',canonical_sha256='none',stage='vertex'),None,[],b'')

if __name__=='__main__':unittest.main()
