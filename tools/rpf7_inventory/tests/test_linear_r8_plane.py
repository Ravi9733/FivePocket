from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from linear_r8_plane import decode_linear_r8_descriptor, unpack_linear_r8_plane

DESCRIPTORS = (
    (0x0A024802, 0x10000002, 0x0059E4FF, 0x00281400, 3, 0x210),
    (0x06024802, 0x11000002, 0x002CE27F, 0x00281400, 3, 0x210),
)


class LinearR8Tests(unittest.TestCase):
    def test_exact_layouts_and_row_padding(self):
        for words, expected in zip(DESCRIPTORS, ((1280, 720, 1280), (640, 360, 768))):
            proof = decode_linear_r8_descriptor(words)
            width, height, pitch = expected
            self.assertEqual((proof['width'], proof['height'], proof['pitch_bytes']), expected)
            self.assertFalse(proof['gpu_executed'])
            source = b''.join(bytes(((x * 7) ^ (y * 13)) & 255 for x in range(width))
                              + b'\xe7' * (pitch - width) for y in range(height))
            expected_pixels = bytes(((x * 7) ^ (y * 13)) & 255
                                    for y in range(height) for x in range(width))
            self.assertEqual(unpack_linear_r8_plane(words, source), expected_pixels)

    def test_each_descriptor_bit(self):
        accepted = 0
        for words in DESCRIPTORS:
            for index in range(6):
                for bit in range(32):
                    changed = list(words)
                    changed[index] ^= 1 << bit
                    if index == 1 and 12 <= bit < 29:
                        decode_linear_r8_descriptor(changed)
                        accepted += 1
                    else:
                        with self.assertRaises(ValueError):
                            decode_linear_r8_descriptor(changed)
        self.assertEqual(accepted, 34)

    def test_extents_types_and_address_end(self):
        for words in DESCRIPTORS:
            size = decode_linear_r8_descriptor(words)['source_bytes']
            for source in (b'', bytes(size - 1), bytes(size + 1), bytearray(size)):
                with self.assertRaises(ValueError):
                    unpack_linear_r8_plane(words, source)
            end = list(words)
            end[1] = 0x1FFFF002
            with self.assertRaises(ValueError):
                decode_linear_r8_descriptor(end)
        for value in (True, -1, 1 << 32, 2.0):
            words = list(DESCRIPTORS[0])
            words[1] = value
            with self.assertRaises(ValueError):
                decode_linear_r8_descriptor(words)
        for words in ((), DESCRIPTORS[0][:-1], (*DESCRIPTORS[0], 0)):
            with self.assertRaises(ValueError):
                decode_linear_r8_descriptor(words)


if __name__ == '__main__':
    unittest.main()
