# Bounded Xbox XMem/LZX decoder

`xmem-lzx` decodes the framed LZX stream used by Xbox 360 XMem compression.
It uses the libmspack `lzxd` source already pinned inside XenonRecomp; it does
not add or download another dependency.

Each input frame is checked before decompression:

- `FF O1 O0 I1 I0` declares 16-bit big-endian output and input sizes.
- `I1 I0` declares a 16-bit big-endian input size and an output size of
  `0x8000` bytes.
- After the declared output total is reached, the parser accepts only EOF or a
  big-endian zero-size terminator followed by a caller-bounded residual tail.
  The default tail limit is 15 bytes, enough for block-cipher alignment.

The expected decoded size is mandatory. Input size, output size, frame count,
window bits, and residual tail all have explicit limits. Malformed or truncated
frames fail before output is opened. Existing output files are never replaced.
Diagnostics contain status and sizes only, never payload contents.

Frame headers are removed into a bounded buffer and the compressed payloads are
decoded as one continuous LZX stream. This preserves the history and Huffman
state carried between the 32 KiB XMem frames.

Build and test:

```sh
cmake -S tools/xmem_lzx -B out/xmem-lzx -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build out/xmem-lzx --parallel
ctest --test-dir out/xmem-lzx --output-on-failure
```

Decode a file:

```sh
out/xmem-lzx/xmem-lzx \
  --expected-size 536631 \
  input.xmem output.bin
```

Use `-` for stdin or stdout:

```sh
producer | out/xmem-lzx/xmem-lzx --expected-size 536631 - - | consumer
```

The default regular-LZX window is 16 bits. Use `--window-bits N` only when the
container metadata specifies a different value. This tool performs
decompression only and does not inspect, identify, hash, or interpret the
decoded data.

The framing behavior is independently implemented from the public
[GTAVTools Xbox stream reader](https://github.com/GTA-Network/GTAVTools/blob/master/GTAVBrowser/RPF7Browser/Utils/XMemDecompressStream.cs).
LZX decoding uses the pinned
[libmspack](https://www.cabextract.org.uk/libmspack/) source and remains subject
to its LGPL license included in the XenonRecomp submodule.
