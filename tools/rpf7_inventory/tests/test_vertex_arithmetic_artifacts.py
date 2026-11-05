from pathlib import Path
import struct
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'vulkan_probe'))
from generate_vertex_arithmetic_probe import make_inputs
from generate_vertex_dataflow_probe import compile_compute
from test_vertex_arithmetic_dataflow import simple


class VertexArithmeticArtifactsTests(unittest.TestCase):
    def test_exact_cpu_output_and_unwritten_parameter_canaries(self):
        vertices=struct.pack('>16f',*range(16))
        constants=struct.pack('>8f',2,2,2,2,1,1,1,1)
        source,proof,combined,expected=make_inputs(simple(),vertices,constants)
        self.assertEqual(combined,vertices+constants)
        self.assertEqual(len(expected),224)
        self.assertEqual(expected[:16],bytes([0x5A])*16)
        self.assertEqual(expected[-16:],bytes([0x5A])*16)
        for index in range(4):
            self.assertEqual(struct.unpack_from('<4f',expected,16+index*48),tuple(v*2+1 for v in range(index*4,index*4+4)))
            self.assertEqual(expected[32+index*48:64+index*48],bytes([0x5A])*32)

    def test_incomplete_constants_or_fourth_vertex_publish_nothing(self):
        vertices=struct.pack('>16f',*range(16));constants=bytes(32)
        for vertex_data,constant_data in ((vertices[:-4],constants),(vertices,constants[:-4]),
                                         (vertices,constants+bytes(16))):
            with self.assertRaises(ValueError):make_inputs(simple(),vertex_data,constant_data)

    def test_compile_failure_does_not_publish_validated_name(self):
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder)
            with patch('generate_vertex_dataflow_probe.run_worker',return_value=SimpleNamespace(status='exited',returncode=1)):
                with self.assertRaises(ValueError):compile_compute(output/'source.comp',output,Path('/compiler'),Path('/validator'))
            self.assertFalse((output/'dataflow.spv').exists())

    def test_validation_failure_retains_only_private_unvalidated_name(self):
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder);pending=output/'dataflow.spv.unvalidated';calls=[]
            def worker(command,**kwargs):
                calls.append(command)
                if len(calls)==1:
                    pending.write_bytes(b'invalid synthetic SPIR-V')
                    return SimpleNamespace(status='exited',returncode=0)
                return SimpleNamespace(status='exited',returncode=1)
            with patch('generate_vertex_dataflow_probe.run_worker',side_effect=worker):
                with self.assertRaises(ValueError):compile_compute(output/'source.comp',output,Path('/compiler'),Path('/validator'))
            self.assertEqual(len(calls),2)
            self.assertFalse((output/'dataflow.spv').exists())
            self.assertEqual(pending.stat().st_mode&0o777,0o600)


if __name__=='__main__':unittest.main()
