from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'vulkan_probe'))
from generate_vertex_dataflow_probe import expected_bytes, load_inputs
from test_raw_vertex_fetch_analysis import pair
from raw_shader_analysis import RawShaderError


class VertexDataflowArtifactsTests(unittest.TestCase):
    def test_cpu_reference_endian_and_canaries(self):
        values = [-0.5, 7.5, 0.75, 0., 0.25, 0.5, 1.] * 3
        output = expected_bytes(pair(), struct.pack('>21f', *values))
        self.assertEqual(len(output), 128)
        self.assertEqual(output[:16], b'\x5a' * 16)
        self.assertEqual(output[112:], b'\x5a' * 16)
        for i in range(3):
            self.assertEqual(struct.unpack_from('<4f', output, 16 + i*32), (-0.5, 7.5, 0.75, 1.))
            self.assertEqual(struct.unpack_from('<4f', output, 32 + i*32), (0., 0.25, 0.5, 1.))

    def test_full_reference_rejects_partial_or_nonfinite_inputs(self):
        source = struct.pack('>21f', *range(21))
        for bad in (source[:-4], struct.pack('>f', float('nan')) + source[4:]):
            with self.assertRaises(RawShaderError): expected_bytes(pair(), bad)

    def test_indices_are_bounded_before_any_read(self):
        for shader, buffer in ((-1, 0), (0, 8), (True, 0), (0, 0.5)):
            with self.assertRaises(ValueError): load_inputs(Path('/does-not-exist'), shader, buffer)


if __name__ == '__main__': unittest.main()
