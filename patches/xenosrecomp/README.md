# XenosRecomp patch queue

These root-owned patches harden the pinned `tools/XenosRecomp` submodule while
keeping its checkout reproducible and suitable for an upstream submission.
They contain synthetic tests only—no game shaders, bytecode, filenames, hashes,
or extracted content.

Check and apply the ordered queue, then rebuild and run its tests:

```sh
python3 tools/xenosrecomp_patches/manage.py check
python3 tools/xenosrecomp_patches/manage.py apply
cmake -S tools/XenosRecomp -B out/host-tools/xenos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build out/host-tools/xenos --parallel
ctest --test-dir out/host-tools/xenos --output-on-failure
```

Return the submodule to its pinned state with:

```sh
python3 tools/xenosrecomp_patches/manage.py reverse
```

`0001-bounded-container-scan.patch` removes the directory scanner's first
unbounded `ShaderContainer` cast. A byte-wise big-endian helper requires the
complete 36-byte header, the existing flags and reserved-field contract, a
nonzero header-sized virtual-plus-physical extent, no 32-bit addition overflow,
and a complete extent inside the available input before hashing, stepping, or
queuing a recompile. The single-container CLI path applies the same check and
requires the declared extent to equal the input size. Focused synthetic tests
cover an exact-size valid container plus truncated header, wrong magic,
zero-size/infinite-loop candidate, size overflow, out-of-input extent, and
nonzero reserved-field rejection.

Patch `0001` is only the outer gate: by itself,
`ShaderRecompiler::recompile` still has no input length and trusts its internal
records. Patch `0002` supplies the structural preflight and length-aware entry
point for those ranges.

`0002-bounded-internal-preflight.patch` adds that structural preflight as a
standalone C++17 component with no DXC, AIR, zstd, or smol-v dependency. Before
the generator receives a pointer, it validates the complete declared container;
bounds and caps constant tables, records, and null-terminated names; checks
pixel/vertex stage and interpolator arrays; walks both supported definition
sections through bounded sentinels and validates physical value extents; and
checks the complete 12-byte microcode extent plus every executed instruction
range and vertex-fetch declaration reference. The byte length is carried from
both CLI paths into `ShaderRecompiler::recompile`, which now returns errors.
Input-derived missing-map, unsupported-relative-constant, attribute-mapping,
and control-flow assertions/exits on the reachable path also become returned
translation failures.

The validator-only synthetic test covers one complete valid container and
malformed constant offsets/counts/alignment/names, truncated and invalid stage
arrays, missing definition sentinels, out-of-range definition values, partial or
out-of-range microcode, invalid control-flow execution ranges, and a vertex
fetch without a matching declaration.

What remains unvalidated is semantic rather than an input byte-range gap:
constant-table creator/target/type/default offsets that the current generator
does not dereference; definition register-number conventions and the unused third
boolean-definition section; jump/loop graph validity beyond memory-referencing
Exec ranges; and correctness/support of individual Xenos opcodes. File-open,
short-read, output-write, DXC/AIR failure handling, and the upstream
game-specific shader limitations are also separate hardening milestones.

`0003-validator-only-batch.patch` exposes the same preflight through the
`XenosShaderPreflight` executable without linking or invoking DXC, AIR, zstd,
smol-v, or the shader-text generator. Its stdin protocol is a sequence of
big-endian 32-bit lengths and exact container payloads, followed by a zero
length and EOF. The worker enforces fixed 16-MiB per-container, 100,000-container,
and 1-GiB aggregate caps. It emits only a versioned total/valid/invalid count
line; malformed framing produces no output. Synthetic tests cover mixed
valid/invalid batches, truncated framing, and trailing bytes.

The protocol is intentionally not a public file interface. Use the root
`tools/rpf7_inventory/xenos_preflight_batch.py` wrapper so decoded inputs are
checked as private, ignored, regular non-symlink files and the result is written
as a mode-0600 redacted aggregate report.

`0004-safe-constant-identifiers.patch` rejects empty, unterminated and non-ASCII
HLSL identifiers before code generation. The first byte must be a letter or
underscore; later bytes may also be digits. All 256 byte values are tested in
both positions. This prevents constant names from injecting source directives.
Identifier collisions or reserved keywords may still cause normal compile errors.

`0005-descriptor-constant-abi.patch` adds the opt-in compiler define
`XENOS_RECOMP_DESCRIPTOR_CONSTANTS`. Three storage buffers replace 64-bit
push-constant device addresses: descriptor set 4, bindings 0/1/2 contain vertex,
pixel and shared constants. The normal device-address variant remains the
default. Both variants load the same byte offsets through common macros.
Game-specific UNLEASHED/MARATHON modes reject this new ABI until separately
integrated. It is not compatible with an unchanged desktop pipeline layout.
The uint mask in `swapFloats` now uses defined 32-bit shifts and rejects bit
indices above 31, removing an unnecessary shaderInt64 requirement.

Thor's current driver reports shaderInt64=false despite bufferDeviceAddress=true.
The descriptor variant validates all 53 staged shaders and passes module creation
and 30 vertex / 23 fragment pipeline compilation tests. The synthetic
`tools/vulkan_probe/constants.comp.hlsl` fixture exercises the actual shared
header on the GPU: all vertex/pixel/shared words, mixed-width fields, boolean
conversion and 192 swap-mask/index combinations match CPU expectations in two
dispatches. This is ABI evidence, not proof of Xenos shader semantic equivalence.

`0006-gta5-pixel-constant-range.patch` admits all 256 float4 constants in pixel
shaders. The 224-register cap was an Unleashed Recompiled runtime limit, not a
Xenos one; GTA V pixel shaders read up to c255 (for example the PostFX blur's
c254/c255 weights). The validator and the translator's dynamic-index clamps
change together. Two synthetic validator cases cover the boundary: c255 is
accepted and a c250..c256 range is refused. On the private GTA V corpus this
check had refused 474 pixel containers.

`0007-gta5-vertex-sampler-fetch-slots.patch` resolves vertex texture fetches.
Xenos vertex fetches use fetch constants 16..31 for vertex sampler registers
0..15, while the constant table names the register (observed:
CoronasDepthMapSampler s0 -> fetch 16, TintPaletteSampler s2 -> fetch 18).
Before this, 288 vertex shaders emitted an undeclared `sN_...DescriptorIndex`.
Open ABI item, not solved here: a vertex sampler register and a pixel sampler
register with the same number share one shared-constant descriptor slot. The
runtime needs per-stage shared constants before such pairs can bind different
textures. This patch has no unit test; the recompiler has no DXC-free harness
in this queue, and its guard is the private corpus translation count.

`0008-report-translation-error.patch` makes the single-container CLI print the
recompiler's fixed error string. For an unmapped vertex element that string
includes the numeric usage and usage index. It never includes payload bytes.

`0009-gta5-vertex-input-locations.patch` maps the three further vertex inputs in
GTA V's complete fxl_final_final corpus: COLOR1 -> 20, TEXCOORD4 -> 21,
TEXCOORD5 -> 22. These are the Vulkan `[[vk::location]]` values the runtime binds.
They stay under Adreno's 32 input locations. Before this, 181 vertex containers
refused translation.

`0010-gta5-texture1d-as-2d.patch` translates 1D texture fetches as 2D samples
at (u, 0.5). The runtime binds 1D textures as height-1 2D images. 45 GTA V pixel
shaders (BlendSampler/RefractionSampler) previously used an undeclared 1D
descriptor.

`0011-vertex-fetch-explicit-lod.patch` makes the translator define
`XENOS_VERTEX_SHADER` in vertex stages. In those stages the common header's
tfetch helpers use SampleLevel(..., 0), because vertex stages have no implicit
derivatives. This is an approximation: the fetch instruction's own LOD fields
are not translated. GTA V's vertex textures (depth maps, palettes) have no mips.

`0012-conditional-exec-boolean.patch` makes the translator honour the boolean
test of conditional-exec control flow. Xenos control-flow instructions
"conditional exec on a boolean constant" (`CondExec`, `CondExecEnd`,
`CondExecPredClean`, `CondExecPredCleanEnd`) run their block only while the
boolean constant `boolAddress` equals `condition`; the translator took only the
block's address, count and sequence, so the block always ran (only `CondJmp`
tested `g_Booleans`). The block is now wrapped in
`if ((g_Booleans & <bool>) != 0)` (`== 0` when the condition bit is 0), using
the named boolean's define or, for an unnamed boolean below 16, its raw bit
(vertex b0-b15 in bits 0-15, pixel b0-b15 in bits 16-31); a boolean outside
`g_Booleans` is a translation error. The end forms keep their return inside the
block, and the return test now names `CondExecPredCleanEnd` (it named
`CondExecEnd` twice). A shader whose blocks read constants the title uploads
only when it sets the boolean no longer reads stale values.

Verification so far: the same hunks, applied to the translator copy that
produced the shipped shader pack, change exactly the translations that contain
such blocks and nothing else, and with every boolean in its "run" state the new
modules give bit-identical outputs to the old ones; that functional test uses
captured title data and therefore lives outside this repository. In this
checkout the patched `shader_recompiler.cpp` passes a syntax-only compile; the
full tool was not built here (the pinned `thirdparty/dxc-bin` contents are not
present). There is no synthetic generator test for this patch yet: the queue
has none for generated HLSL (patches `0009`-`0011` likewise), and one that
builds a minimal container with a boolean-conditional block is the missing
piece.

`0013-alu-coissue-read-order.patch` makes the translator respect the operand
read order of co-issued ALU operations. A Xenos ALU instruction carries a
vector and a scalar operation that both read their sources before either
writes. The translator printed the vector assignment before the scalar
expression, so a scalar operation reading a temporary component that the
paired vector operation writes read the new value. `XenosAluCoIssueHazard`
(`alu_coissue.h`) decides when that happens. In that case only, the scalar
computation (predicate, `ps`, `a0`) is printed before the vector assignment,
and the scalar register write still comes after it. Every other instruction
prints exactly as before. `XenosTests/alu_coissue_test.cpp` checks the
predicate on synthetic instructions: overlap and no overlap, two-operand and
`*sc` forms, export, relative and constant sources, `retain_prev`, vector
kills, and no vector write.

Verification outside this repository (title data): the same rule applied to
the translator copy that produced the shipped pack finds 165 affected shaders.
For 159 of them the unpatched control reproduces the shipped module byte for
byte, and the patched translation holds the same lines with only the vector
line moved after the scalar line. In one of them, a water shader, the fix
turns an unfogged far-ocean band into the fog colour that the scene's fog pass
computes from the same constants. Not covered: relative scalar sources, and
`a0`/`p0` written by vector `MaxA`/`SetpPush` and read by the scalar operation
in the same instruction.
