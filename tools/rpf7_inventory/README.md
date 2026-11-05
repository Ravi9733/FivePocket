# RPF7 header inventory

`rpf7_inventory.py` performs a read-only inventory of big-endian console RPF7
archives. Its default mode reads only the public 16-byte header of each regular
file and emits JSON containing:

- archive-relative path and byte size;
- header entry count and name-table size;
- platform bit and name-offset alignment;
- encryption tag; and
- bounded table offsets derived from the header.

It follows neither file nor directory symlinks. It does not extract assets or
hash game data.

## Redacted TOC summary

`--toc-summary` opts into bounded table reading. It validates entry types,
directory ranges, name references, and payload extents, but returns aggregate
counts only. It never returns entry names, decrypted buffers, payload bytes, or
key material. Defaults cap the operation at 1,000,000 records, a 64 MiB name
table, 96 MiB total metadata, and 4 KiB per referenced name.

For the Xbox 360 AES table tag, the caller can provide one **raw, exactly
32-byte** content key through `--aes-key-file`. The path must be an ordinary
file and cannot be a symlink. Keep that file outside the repository and outside
shared logs:

```sh
python3 tools/rpf7_inventory/rpf7_inventory.py \
  /path/to/archive.rpf \
  --toc-summary \
  --aes-key-file /private/path/gtav-xbox360-rpf-aes.key \
  --pretty
```

The tool contains no Rockstar key, known-key digest, or key-discovery routine.
Supplying an Xbox disc/DVD key or an XEX container key is not sufficient: this
interface needs the 32-byte RPF table/content AES key recovered locally from the
lawfully owned title's decrypted executable image. The remaining derivation
work is to trace the archive initialization/key-schedule call in that image and
write the selected 32 bytes to a Git-ignored, user-controlled path. Until then,
encrypted TOCs fail closed with a `blocked` summary.

NG/TFIT encryption is also intentionally unsupported; it requires a different
per-archive derivation and tables rather than this AES key.

The console header and entry interpretation is independently implemented from
the public big-endian layout documented by
[GTAVTools Structs.cs](https://github.com/GTA-Network/GTAVTools/blob/master/GTAVBrowser/RPF7Browser/RPF7/Structs.cs)
and its
[entry parser](https://github.com/GTA-Network/GTAVTools/blob/master/GTAVBrowser/RPF7Browser/RPF7/Entries/Entry.cs).
The separate AES-256-ECB transforms and unchanged partial-block behavior follow
the project's public
[AES implementation](https://github.com/GTA-Network/GTAVTools/blob/master/GTAVBrowser/RPF7Browser/Utils/AES.cs).
The dependency-free AES primitive is checked against the official
[NIST FIPS 197](https://doi.org/10.6028/NIST.FIPS.197-upd1) known-answer vector.

```sh
python3 tools/rpf7_inventory/rpf7_inventory.py /path/to/extracted-disc --pretty
python3 -m unittest discover -s tools/rpf7_inventory/tests -v
```

Exit status is `0` for a clean inventory, `1` when one or more candidate files
could not be inspected, and `2` when the supplied path itself is invalid.

## Stopped renderer requirements

These read-only reports inventory related requirements before another device
run. A clean report is **not** complete game compatibility or GPU admission.
Unknown control flow, ownership, GPU completion and missing capture bytes remain
explicit obligations; do not bypass runtime validation based on a report.

```sh
python3 tools/rpf7_inventory/inline_capture_preflight.py local-analysis/RUN/renderer-first-unsupported
python3 tools/rpf7_inventory/resolve_capture_preflight.py local-analysis/RUN/renderer-first-unsupported
```

Choose the first command for `draw.json`, the second for `resolve.json`.
Resolve versions1/2 are supported. Version1 lacks pointed rectangles/clear
colors: they are reported unavailable, never assumed zero. Reports fingerprint
the captured payloads and current implementation. No game execution or writes.

## Local executable-key scan

`rpf7_key_scan.py` can test 32-byte windows from a privately decoded XEX image
against the encrypted root record, then validate the complete table before
writing a uniquely identified key. It never prints or hashes key bytes, refuses
to overwrite the output, and should only target an ignored local path:

```sh
PYTHONPATH=tools/rpf7_inventory python3 tools/rpf7_inventory/rpf7_key_scan.py \
  local-analysis/Disc2-Play/default.image.bin \
  local-analysis/Disc2-Play/common.rpf \
  local-analysis/rpf7-aes-key.bin \
  --start 0x600 --size 1582200 --stride 16
```

For a wide byte-by-byte scan, `rpf7_key_scan_native` performs only the same
encrypted-root prefilter using OpenSSL and emits candidate offsets—not key
bytes. Each candidate must still pass the Python tool's complete bounded TOC
validation before any private key file is written.

If the executable constructs its key in writable memory at startup,
`rpf7_static_key_recover.py` conservatively propagates only immediate `lis`,
`li`, `addi`, and `ori` values in generated PPC comments and reconstructs
overlapping big-endian `stb`, `sth`, or `stw` stores. Any unmodelled instruction
invalidates the current constant state. A complete 32-byte result must validate
the archive's full bounded TOC before the tool creates an ignored mode-0600 key
file:

```sh
PYTHONPATH=tools/rpf7_inventory python3 \
  tools/rpf7_inventory/rpf7_static_key_recover.py \
  local-analysis/codegen/ppc_base \
  local-analysis/Disc2-Play/common.rpf \
  local-analysis/rpf7-aes-key.bin \
  --target-address 0x00000000
```

Replace the placeholder target with the writable 32-byte guest address proven
by the title's AES-context call site. The report contains the address, coverage,
contributing function names, and redacted TOC aggregates, but never key bytes or
a key digest.

## Private asset catalog

`rpf7_catalog.py` turns one fully validated table into a private SQLite index.
It records safe archive-relative paths, parent relationships, payload extents,
compression/encryption flags, and resource virtual/physical page metadata. The
database must be under ignored `local-analysis/`, is created mode 0600, and is
never overwritten. Names go only to that local database; stdout remains an
aggregate report. The default single-archive mode does not read payloads.

```sh
PYTHONPATH=tools/rpf7_inventory python3 tools/rpf7_inventory/rpf7_catalog.py \
  local-analysis/Disc2-Play/common.rpf \
  local-analysis/rpf7-aes-key.bin \
  local-analysis/rpf7-catalogs/common.sqlite3
```

Pass `--recursive` to decode and catalog binary entries whose validated name
ends in `.rpf`. Recursive traversal is fail-closed and defaults to a depth of
4, 64 nested archives, 256 MiB per decoded archive, and 512 MiB across all
nested archives. An SHA-256 digest is retained in memory only for each ancestor
chain so a malformed archive cannot form a cycle; digests are neither stored
nor emitted. Compressed nested archives use the same bounded Xbox XMem/LZX
helper as binary extraction. The resulting mode-0600 database keeps the root
records in `entries` for extractor compatibility and stores the private nested
tree in `archives` and `nested_entries`. Recursive mode streams the root once
for its in-memory ancestor digest, reads candidate nested-archive payloads, and
never emits those bytes.

```sh
PYTHONPATH=tools/rpf7_inventory python3 tools/rpf7_inventory/rpf7_catalog.py \
  local-analysis/Disc2-Play/common.rpf \
  local-analysis/rpf7-aes-key.bin \
  local-analysis/rpf7-catalogs/common-recursive.sqlite3 \
  --recursive --max-depth 4
```

This recursion behavior follows CodeWalker's public nested-RPF scan and
[Swage's RPF7 virtual file implementation](https://github.com/0x1F9F1/Swage/blob/master/src/games/rage/rpf7.cpp),
which expose nested packfiles as ordinary binary entries before loading another
bounded RPF7 table.

The entry layout and 512-byte payload offsets follow the public console parser
in [GTAVTools](https://github.com/GTA-Network/GTAVTools/blob/master/GTAVBrowser/RPF7Browser/RPF7/Entries/Entry.cs).
Resource page-size decoding is independently checked against the public
[CodeWalker implementation](https://github.com/dexyfex/CodeWalker/blob/master/CodeWalker.Core/GameFiles/RpfFile.cs).
For resource entries, `logical_size` is always the system-plus-graphics size
decoded from those flags; the 24-bit entry size is the compressed on-disk size,
not the decoded size.

`rpf7_extract.py` performs one bounded, catalog-selected binary extraction. It
reads only the selected payload extent, applies AES before the Xbox XMem/LZX
decoder when the entry flags require it, verifies the decoded size, and writes
an ignored mode-0600 file without overwriting. Build `tools/xmem_lzx` first, or
pass its executable with `--xmem-decoder`. Resource entries remain excluded
until per-resource encryption selection and virtual/physical segment placement
have their own validation gates.

```sh
PYTHONPATH=tools/rpf7_inventory python3 tools/rpf7_inventory/rpf7_extract.py \
  local-analysis/rpf7-catalogs/common.sqlite3 \
  local-analysis/Disc2-Play/common.rpf \
  local-analysis/rpf7-aes-key.bin \
  local-analysis/rpf7-extract/selected.bin \
  --entry-index 1
```

## Standalone RSC7 header inspection

`rsc7_inspect.py` is the metadata-only validation gate for a standalone,
OpenIV-compatible RSC7 resource produced by a lawful local extraction. It reads
exactly the 16-byte little-endian header and stable file size, verifies `RSC7`
magic, checks that the eight-bit version agrees with the high nibbles of the
system/graphics flags, bounds page counts and decoded segment sizes, and emits
no path or payload data. The source file is never modified, so mode 0600 is
preserved.

```sh
python3 tools/rpf7_inventory/rsc7_inspect.py \
  local-analysis/rsc7-validation/header-sample.rsc7
```

The portable header layout is independently supported by
[CodeWalker's `CreateResourceFileEntry`](https://github.com/dexyfex/CodeWalker/blob/master/CodeWalker.Core/GameFiles/RpfFile.cs)
and [Swage's `datResourceFileHeader`](https://github.com/0x1F9F1/Swage/blob/master/src/games/rage/rsc.h).
Swage also shows that an RPF7 entry synthesizes this standalone header from its
TOC version and page flags. Its raw in-archive stream instead skips an opaque
16-byte prefix before decrypting/decompressing, using that prefix only for the
special `0xFFFFFF` large-resource size sentinel. Accordingly, the inspector
rejects raw in-archive resource extents rather than guessing that their prefix
is RSC7. Full Xbox resource extraction remains intentionally disabled until
per-resource encryption selection is proven; the inspector does not weaken
that boundary.

## Xbox RSC7 decompression and segment split

`rsc7_decode.py` advances a validated standalone RSC7 resource through the
Xbox 360 path only. After one secure bounded read, it passes the bytes following
the 16-byte header to the separately built XMem/LZX decoder, requires the exact
system-plus-graphics decoded byte count, and splits the contiguous image at the
declared system segment boundary. It creates a new mode-0700 private directory
containing `system.bin` and `graphics.bin`, both mode 0600, and never overwrites
an existing directory or file.

```sh
python3 tools/rpf7_inventory/rsc7_decode.py \
  local-analysis/rsc7-validation/header-sample-matched.rsc7 \
  local-analysis/rsc7-validation/decoded-sample
```

Defaults cap compressed input at 64 MiB, each declared segment at 1 GiB, total
decoded output at 2 GiB, and XMem framing at 8,192 blocks. The native helper
also validates every big-endian frame length and permits only its bounded zero
terminator/tail form before any output is created. This codec is intentionally
not auto-detected: standalone RSC7 does not identify the compression platform,
so non-Xbox or raw-DEFLATE resources fail closed.

The framing is independently supported by the public
[GTAVTools Xbox `XMemDecompressStream`](https://github.com/GTA-Network/GTAVTools/blob/master/GTAVBrowser/RPF7Browser/Utils/XMemDecompressStream.cs),
which parses `FF + output-size + input-size` or default-32-KiB frame headers,
and by [Swage's RPF7 reader](https://github.com/0x1F9F1/Swage/blob/master/src/games/rage/rpf7.cpp),
which selects streaming XMem/LZX for the RPF7 platform bit and decodes exactly
the flag-derived resource size. FiveM's public
[`UnwrapRSC7`](https://github.com/citizenfx/fivem/blob/master/code/components/tool-formats/src/ConvertFormats.cpp)
independently confirms that the decompressed image is system bytes followed by
graphics bytes at the flag-derived boundary. On the private Xbox 360 corpus,
51 bounded non-name-reporting samples matched this framing and exact size; the
committed tests contain synthetic data only.

## FXC to Xenos staging metadata

`fxc_xenos_inspect.py` performs a bounded handoff inspection on one privately
decoded Xbox 360 `.fxc`. It requires the little-endian `rgxe` outer magic, then
uses the pinned XenosRecomp directory scanner's word-order, flag mask, and
container stepping rules to locate 36-byte big-endian Xenos headers. A candidate
is staged only when its virtual/physical extent and the minimum constant-table,
optional definition-table, and shader records are all inside the candidate.

```sh
python3 tools/rpf7_inventory/fxc_xenos_inspect.py \
  local-analysis/fxc-validation/sample-with-xenos.fxc \
  local-analysis/fxc-validation/sample-with-xenos.manifest.json
```

The manifest is exclusively created mode 0600 below ignored `local-analysis/`.
It contains offsets, bounded sizes, stage classification, and aggregate counts,
but no FXC filename, shader names, shader bytecode, content hashes, or key
material. Defaults cap the FXC at 64 MiB, each candidate at 16 MiB, and the
candidate count at 100,000. The source is opened without following a symlink and
is not modified.

The outer layout is supported by CodeWalker's public
[`FxcFile`](https://github.com/dexyfex/CodeWalker/blob/master/CodeWalker.Core/GameFiles/FileTypes/FxcFile.cs),
which identifies `rgxe` and the six shader groups. The staged inner contract is
taken directly from the pinned
[XenosRecomp `ShaderContainer`](https://github.com/sonicnext-dev/XenosRecomp/blob/main/XenosRecomp/shader.h)
and its
[directory scanner](https://github.com/sonicnext-dev/XenosRecomp/blob/main/XenosRecomp/main.cpp).
On a private bounded sample, 60 of 60 decoded FXCs had `rgxe` and produced 918
fully bounded Xenos candidates with no rejected magic-like records. One
mode-0600 private manifest staged 53 candidates without reading FXC shader-name
tables or emitting bytecode.

This deliberately does not reuse `SanRecomp/install/shader_converter.cpp`'s
current `rgxa` parser: that magic and reduced fragment layout disagree with both
the public `rgxe` implementation and every checked private file. It also does
not invoke XenosRecomp directly. Its single-file path accepts only a raw container at
offset zero, while its directory path performs unbounded internal dereferences
after discovery. The staging manifest provides the exact validated extents
needed for the bounded extraction/recompiler wrapper below without guessing at
the rest of the FXC name and technique tables.

## Validator-only private Xenos batch

After applying the XenosRecomp patch queue and building it, the private batch
wrapper can pass every bounded container found by the `rgxe` staging contract
through the standalone structural preflight:

```sh
python3 tools/xenosrecomp_patches/manage.py apply
cmake -S tools/XenosRecomp -B out/host-tools/xenos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build out/host-tools/xenos --parallel
python3 tools/rpf7_inventory/xenos_preflight_batch.py \
  local-analysis/fxc-validation/xenos-preflight-report.json \
  local-analysis/fxc-validation/private-decoded.fxc
python3 tools/xenosrecomp_patches/manage.py reverse
```

Inputs must be explicitly supplied regular files below ignored
`local-analysis/`; tracked inputs, repeated inodes, symlinks in any input path,
and files outside that boundary are rejected. Defaults cap the batch at 1,024
files, 64 MiB per file, 16 MiB per container, 100,000 containers, and 1 GiB in
aggregate. An `rgxe` input is rescanned with `fxc_xenos_inspect.py` and only its
bounded candidate extents enter the validator protocol. A non-`rgxe` input is
treated as one raw container. The C++ worker never invokes the recompiler or
shader compilers.

The exclusively-created report must also remain below ignored
`local-analysis/` and is forced to mode 0600. It contains only aggregate
input/container and valid/invalid/rejected counts plus explicit redaction flags:
no filenames, hashes, names, offsets, shader text, bytecode, or payload. Any
invalid container, rejected magic-like candidate, or empty `rgxe` source makes
the aggregate status fail closed. Synthetic tests contain no game data.

## Private shader translation and Android-compatible constants

`xenos_translate_batch.py` now uses the patched length-aware single-container
translator, pinned DXC and an explicit SPIRV-Tools validator. Apply/build the
whole Xenos patch queue first and keep it applied while converting, so the
common header and binary use the same ABI.

```sh
python3 tools/rpf7_inventory/xenos_translate_batch.py \
  local-analysis/NEW-CONVERSION-DIRECTORY \
  local-analysis/fxc-validation/sample-with-xenos.fxc \
  --constant-binding descriptor32 \
  --spirv-validator /ABSOLUTE/NDK/shader-tools/darwin-x86_64/spirv-val
```

The output directory must not exist and must be ignored under local-analysis.
Whole-selection staging rejects malformed or empty FXC sources before outputs
are created. Defaults cap selection at 64 files / 64 MiB and 256 containers;
the explicit container ceiling is 1,024. Every child is isolated in a POSIX
process group with ten CPU seconds, a 30-second wall deadline, no core dumps,
16 MiB per output file and a 64 KiB pipe/log limit. Total retained output has a
512 MiB budget. These are resource limits, not a security sandbox. Parent/child
tool binaries and the explicitly selected common header are trusted tools.

Generated HLSL, SPIR-V and compiler diagnostics stay in mode-0600 files under
mode-0700 private directories. Reports contain numeric indices, stages,
statuses and standard capability identifiers, never source shader names or
payload. Per-container checkpoints are exclusive; partial results are preserved.
`validated-modules.bin` transports only the modules that passed full validation
and the selected capability contract. Nothing is installed or uploaded by this
conversion tool. Compiler success is not Xenos semantic equivalence or rendering.

The default `device-address` ABI uses three 64-bit push-constant addresses.
Thor reports bufferDeviceAddress=true but shaderInt64=false. The `descriptor32`
variant instead binds vertex/pixel/shared constant bytes through storage buffers
at set 4, bindings 0/1/2. It rejects output that still needs Int64 or physical
storage-buffer addresses. Both variants validated all 53 preserved sample
containers (30 vertex, 23 pixel). Android pipeline and GPU ABI tests are described
in `tools/vulkan_probe/README.md`; game-specific texture/state uploads remain work.
