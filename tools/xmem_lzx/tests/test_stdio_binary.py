#!/usr/bin/env python3
"""The decoder program handles binary data the same through the standard streams and through files, on every host.

The stage tools hand it a payload on its standard input and take the decoded bytes from its standard output. On
Windows those streams are text unless the program says otherwise: a 0x1A byte ends the input, every 0x0A written
grows a 0x0D. The stream here is made by hand (one stored LZX block in one frame; nothing of a game): its 16 bytes
hold 0x0A, 0x0D 0x0A and 0x1A, in the payload and in the result.

XMEM_LZX names the program; without it the Mac builder's copy is used, and the test is skipped where neither is
there. On a host whose streams are binary anyway the stream cases cannot fail; they are the check for Windows.
"""
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TOOL = Path(os.environ.get('XMEM_LZX', ROOT / 'local-analysis/builder/portable/host-tools-darwin/xmem-lzx'))
RAW = bytes([0x41, 0x0A, 0x0D, 0x0A, 0x1A, 0x00, 0xFF, 0x0A, 0x1A, 0x0D, 0x42, 0x43, 0x0A, 0x0A, 0x1A, 0x5A])


def stream(raw: bytes) -> bytes:
    """One frame holding one stored block: the header bit (no E8 translation), block type 3, the 24-bit length, padding
    to a 16-bit word, the three repeat offsets, the bytes."""
    bits = '0' + '011' + format(len(raw), '024b')
    bits += '0' * (-len(bits) % 16)
    words = b''.join(struct.pack('<H', int(bits[i:i + 16], 2)) for i in range(0, len(bits), 16))
    body = words + struct.pack('<III', 1, 1, 1) + raw
    return b'\xff' + struct.pack('>HH', len(raw), len(body)) + body


def arguments(payload: bytes) -> list[str]:
    return [str(TOOL), '--expected-size', str(len(RAW)), '--max-input', str(len(payload)), '--max-output', '65536', '--max-frames', '4']


@unittest.skipUnless(TOOL.is_file(), 'the decoder program is not built here')
class StandardStreamsAreBinary(unittest.TestCase):
    def test_through_files(self):
        with tempfile.TemporaryDirectory() as folder:
            source, target = Path(folder) / 'in.bin', Path(folder) / 'out.bin'
            source.write_bytes(stream(RAW))
            done = subprocess.run([*arguments(stream(RAW)), str(source), str(target)], capture_output=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(target.read_bytes(), RAW)

    def test_through_the_standard_streams(self):
        payload = stream(RAW)
        self.assertIn(b'\x1a', payload)
        done = subprocess.run([*arguments(payload), '-', '-'], input=payload, capture_output=True)
        self.assertEqual(done.returncode, 0, done.stderr)       # (text input would end at the first 0x1A)
        self.assertEqual(done.stdout, RAW)                      # (text output would hold 0x0D 0x0A for every 0x0A)

    def test_the_whole_input_is_counted(self):
        payload = stream(RAW)
        short = arguments(payload)
        short[short.index('--max-input') + 1] = str(len(payload) - 1)
        done = subprocess.run([*short, '-', '-'], input=payload, capture_output=True)
        self.assertEqual(done.returncode, 1)
        self.assertIn(b'input exceeds configured size', done.stderr)   # (a text stream would have stopped before the limit)


if __name__ == '__main__':
    unittest.main()
