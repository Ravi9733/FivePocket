# Headless Android Vulkan diagnostics

These are standalone native tests, not the APK or a guest GPU implementation.
They require Vulkan 1.2, select physical device zero, and never return success
for an Xbox import. No shader text, assets or generated game code is tracked.

Verified on AYN Thor (2026-09-05): its driver reports shaderInt64=false,
bufferDeviceAddress=true, shaderClipDistance=true and runtimeDescriptorArray=true.
The core shaderSampledImageArrayDynamicIndexing feature is also queried and
explicitly enabled for the conservative runtime-descriptor-heap profile. This
is required for dynamically uniform sampled-image array accesses even when a
module does not declare capability 29. Non-uniform indexing stays unsupported.
The address-based shader pack is rejected before device creation. The alternative
descriptor32 pack creates 53 shader modules and compiles 30 vertex / 23 fragment
pipelines. A separate synthetic compute fixture verifies 5,568 output words.
Those catalog tests establish no game draw, original stage pairing, texture
upload or gameplay. The separate raw initialization-pair test below now validates
one captured pairing under a synthetic native raster profile.

## Color8 full-clear and per-sample regression

`color8_clear_target_test.cpp` tests the native color8 target without game data.
Build it with `device.cpp`, the Android C++ headers, Vulkan probe headers and
`-lvulkan`; keep the executable and compiled shaders under `local-analysis/`.
Compile `color8_clear_samples.comp` twice with glslc targeting Vulkan1.2:
`-DMULTISAMPLED=0` and `-DMULTISAMPLED=1`. Run the executable with the resulting
single-sample and multisample SPIR-V paths as its two arguments. Use assertions
(`-UNDEBUG`), since the test's setup and comparisons deliberately fail closed.

The six cases cover1x/2x/4x and original color formats0/1, each with256 clear
values covering all bytes in every channel. Each clear is a real Vulkan command;
the independent compute reader checks every individual sample and outer canaries.
Re-clear tests exercise layouts and synchronization. No CPU-written target
pixels, asset input, game shader or guest completion is involved.

Thor validation2026-09-11:1536 clears,58720256 individually read samples PASS.
Gamma is original PWL byte encoding, not hardware sRGB. This does not validate
general gamma blending, arbitrary Xbox raster sample positions, depth/stencil
or game resolve semantics. Only the uniform clear's diagnostic resolve is tested.

The public CTest targets include bound-surface, complete-clear, color8
conversion/plan, exact float24, resolve-request and render-queue inventory
regressions with assertions enabled. These CPU tests use synthetic inputs and
require no game assets. `render_queue_preflight.py CAPTURE_DIRECTORY` groups
captured handler families for batch investigation. Its bounded inventory is
potential linear fallthrough at capture time: callbacks may alter the queue
head or records, so this does not predict which commands will execute.

Host request regressions are in `android/tests/gta5_color8_clear_plan_test.cpp`,
`gta5_color8_clear_test.cpp` and `gta5_clear_capture_test.cpp`; compile with
`-std=c++20 -UNDEBUG -Iandroid/app/src/main/cpp`. The read-only stopped-clear
inventory is `tools/rpf7_inventory/clear_capture_preflight.py CAPTURE_DIRECTORY`.

The optional coupled-depth test takes four additional shader paths: depth1x,
depthMS,stencil1x,stencilMS. Compile the same oracle with `-DDEPTH=1` or
`-DSTENCIL=1` and the corresponding `MULTISAMPLED` setting. It verifies768
combined color/depth/stencil clears at1x/2x/4x, all256 stencil values, exact20e4
depth endpoints and small values, plus failed-request invalidation/recovery.
All three aspects are individually read back. These tests passed on Thor.
The depth representation is exact20e4 values expanded into D32+S8, not general
Xbox depth testing. `xenos_float24_clear_test.cpp` exhausts all16777216 stored
patterns against independent IEEE-bit construction and encode/decode round trips.
Non-exact values, unproved rounding and single-aspect preservation are rejected.

## Captured float vertex dataflow (compute only)

`generate_vertex_dataflow_probe.py REPORT NEW_PRIVATE_DIRECTORY --shader-index N
--range-index N --glslc PATH --spirv-validator PATH` selects one vertex program
and one explicit slot0 buffer from the same private stopped report. Exact sizes,
hashes, ignored paths and no-symlink private reads are checked. It proves the
restricted float-fetch/copy profile, evaluates three explicit auto-index inputs
on the CPU, and emits a validated derived **compute** module. Failed compilation
or validation never publishes `dataflow.spv`. No container metadata is invented.

`vertex_dataflow_test dataflow.spv input.bin expected.bin` runs zero, one, three
and four invocations, each with a fresh fence. Only indices0..2 may write; each
vertex has32 output bytes and the128-byte buffer has16-byte outer canaries.
Both input bytes and every output byte are compared after each fence. Bounds or
non-finite fetches leave that invocation untouched. `--corrupt-expected-control`
must fail the one-invocation comparison after the zero-work control passes.

Verified on ARM64 Android emulator/Lavapipe (2026-09-08), **not Thor**: the first
and resolve captured programs match CPU references. Both corruption controls and
12 truncated/NaN/Inf input cases behave as expected (16 processes,60 fences).
This is neither a native vertex pipeline nor a guest draw: index-register setup,
predication, linkage, viewport, pixel math, textures, EDRAM and game frames are
not established. Runtime submission guards remain unchanged.

## Captured initialization pair

`texture32_sample_test` is a separate build-time profile of the fenced harness,
with a real32x32 RGBA8 image upload and native linear/repeat/LOD0 sampler. Its
independently authored `texture32_sample.comp` is **not a game pixel shader**.
The generator produces private center/periodic/integer-quarter references;
all16,416 output bytes andunchanged4KiB input are checked.15 emulator processes/
48 fences pass, including bad-reference andreplace-upload-with-zero controls.
This introduces no guest texture residency orruntime cache-coherence support.

The separate `vertex_arithmetic_dataflow_test` uses the same fenced comparison
harness with a224-byte output/four-vertex profile, selected at build time. Its
inputs come from `generate_vertex_arithmetic_probe.py`, with explicit private BE
constant vectors in referenced-register order. It supports only the experimental
float/packed-fetch, MAXcopy, finite-normal/zero MUL/unfusedMAD dataflow. This does
not change the older float-only profile or runtime shader guards.151 emulator
processes/600 fences pass on2026-09-08, including partial-export canaries and
all256 normalized byte values per channel; details in docs/fable/CONTEXT.md.
ALLOC semantics, pixel shaders, textures, rasterization andframes remain unproved.

On 2026-09-06, the stopped second command batch supplied two small inline shader
programs absent from the 53-container catalog. The restricted independent raw
analyzer derives constant vertex exports and a pixel interpolator copy, using
the captured program-control/context fields. No ShaderContainer metadata is
invented. `tools/host_guest_trace/analyze_stopped_pm4.py` emits private GLSL and
optionally compiles/validates Vulkan 1.1 modules; see docs/fable/CONTEXT.md for
the exact current private paths and reproduction commands.

`vulkan_point_pair_test VERTEX.spv PIXEL.spv` runs those explicitly selected,
spirv-val-validated outputs in a synthetic 16x16 RGBA8 target, with a 1-pixel
point, no textures/descriptors and no depth. The zero-draw control stays white;
one color-enabled point produces exactly one transparent-black pixel; 24
color-masked points leave all 256 pixels white. The fourth case adds five
synthetic scratch-like GPU updates before 24 masked points and four updates
after graphics, including a same-word overwrite and a read-index-like marker.
All four fences, 1,024 pixels and the 64-byte completion/canary tail pass on Thor.
An independent golden byte array checks BE encoding; command recording leaves
the mapped bytes unchanged and the fence unsignaled before queue submission.
Fences cap at five seconds, process alarm at 30 seconds.
This proves the restricted pair in native GPU test conditions, not guest PM4
execution, original viewport/scissor behavior, EDRAM, event writes or gameplay.
Private results: local-analysis/stopped-init-pair-v1/point-probe-result.json and
ordered-completion-probe-result.json. Appending `--corrupt-completion-control`
deliberately changes one GPU-updated byte: Thor completes the fences, reports
matched=0 and exits 1 without its success footer. The positive run exits 0.

`native_gpu_word_updates.h` preflights the entire native-buffer plan before any
GPU command is recorded. It caps 64 words / 4 MiB, validates exact phase order,
aligned ranges and within-phase duplicates, and preserves old output on failure.
Cross-phase overwrites require the documented barriers. Strict host UBSan/native
tests cover boundaries and 10,000 byte-encoding cases. This is native staging
infrastructure: it does not approve guest addresses, execute PM4, signal Xbox
events or consume the guest ring.

`native_gpu_word_readback.h` revalidates the entire mutable plan, computes each
destination's final value after ordered overwrites, and compares all addressed
bytes before publishing an audited result. The caller must already have waited
for a real fence and established host visibility. Untouched canaries remain a
separate full-buffer check; audited offsets carry no guest write authority.
Host UBSan/native fixtures cover every bit in all addressed bytes, stale
pre-overwrite values, malformed plans/extents and exact64-destination limits.

The point probe uses the audit after its fence. `--corrupt-recorded-update`
changes one GPU command's data without changing the validated plan: Thor reports
`passed=0 destinations=0`, exits1 and emits no success footer. Positive legacy,
actual-raw/generic and all16 generic-color paths pass; nine synthetic requests
yield eight final native destinations due to the ordered overwrite. These are
not the seven actual guest words. Evidence:
local-analysis/stopped-init-pair-v1/native-word-readback-audit-result.json.

## Build

The generic constant-copy pair also accepts proved native constants, removing
per-game shader compilation for this narrow subset. Independently authored
constant_point.vert/.frag use a 32-byte std430 push-constant block (position at
0, parameter at 16) and explicit location-0 linkage. NativeConstantPointInputs
validates all literal components before publication; host UBSan/native Thor
fixtures check all 256 vector combinations and failure canaries.

Compile once on the build host with NDK glslc --target-env=vulkan1.1 and validate
each output with spirv-val --target-env vulkan1.1. Example outputs are
local-analysis/vulkan-probe-build/api26/generic-constant-point.{vert,frag}.spv.
Alternatively configure the existing build with both explicit tools:

```sh
cmake -S tools/vulkan_probe -B local-analysis/vulkan-probe-build/api26 \
  -DRECOMP_GLSLC=/opt/homebrew/share/android-commandlinetools/ndk/27.2.12479018/shader-tools/darwin-x86_64/glslc \
  -DRECOMP_SPIRV_VAL=/opt/homebrew/share/android-commandlinetools/ndk/27.2.12479018/shader-tools/darwin-x86_64/spirv-val
cmake --build local-analysis/vulkan-probe-build/api26 --target generic_constant_shaders -j2
```

The target compiles a temporary candidate, validates it, then publishes the
final module. A forced-validator-failure experiment confirms neither final file
appears; restoring the real validator retries and produces the expected hashes.

Run point-pair-test with those two modules plus --generic-constants: Thor passes
all 16 color masks, each with no-draw, one enabled point and 24 masked points
with ordered staging. This checks 48 fences / 12,288 pixels. White output uses
a black control background so every color differs from its control.

With --generic-raw-pair RAW_VERTEX RAW_PIXEL, both bounded raw programs are
natively proved and linked before device creation. The actual captured pair
passes four cases / 1,024 pixels using the generic modules, with no game-specific
SPIR-V compilation. Raw files remain private. Reversing their stage order
rejects with exit 2 before any Vulkan output. --generic-corrupt-constants
deliberately changes one supplied component without changing expected readback;
it exits 1 on the point case with no success footer. Both legacy positive and
corrupted-completion controls still pass/fail as expected. Evidence lives at
local-analysis/stopped-init-pair-v1/generic-constant-probe-result.json.

These are still standalone synthetic-raster tests, not execution of the guest
batch, its invalidations or its seven guest destinations. Vulkan validation
layers and original hardware float-edge fidelity are not claimed. Push-constant
layout/lifetime basis: https://docs.vulkan.org/guide/latest/push_constants.html

The host-only parser tests need no Vulkan SDK:

```sh
cmake -S tools/vulkan_probe -B out/vulkan-probe -DCMAKE_BUILD_TYPE=Release
cmake --build out/vulkan-probe --parallel 3
ctest --test-dir out/vulkan-probe --output-on-failure
```

Android, using the configured local NDK:

```sh
cmake -S tools/vulkan_probe -B local-analysis/vulkan-probe-build/api26 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/opt/homebrew/share/android-commandlinetools/ndk/27.2.12479018/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=26 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release
cmake --build local-analysis/vulkan-probe-build/api26 --parallel 3
```

## Synthetic fixture shaders

Apply and build the complete Xenos patch queue first; keep it applied while
compiling against its shared header. On this ARM64 Mac:

```sh
DYLD_LIBRARY_PATH="$PWD/tools/XenosRecomp/thirdparty/dxc-bin/lib/arm64" \
tools/XenosRecomp/thirdparty/dxc-bin/bin/arm64/dxc-macos \
  -T cs_6_0 -E shaderMain -HV 2021 -spirv -fspv-target-env=vulkan1.1 \
  -fvk-use-dx-layout -D XENOS_RECOMP_DESCRIPTOR_CONSTANTS=1 \
  -I tools/XenosRecomp/XenosRecomp \
  -Fo local-analysis/leaf-switch-check/constants-descriptor32.spv \
  tools/vulkan_probe/constants.comp.hlsl

DYLD_LIBRARY_PATH="$PWD/tools/XenosRecomp/thirdparty/dxc-bin/lib/arm64" \
tools/XenosRecomp/thirdparty/dxc-bin/bin/arm64/dxc-macos \
  -T vs_6_0 -E shaderMain -HV 2021 -spirv -fspv-target-env=vulkan1.1 \
  -fvk-use-dx-layout \
  -Fo local-analysis/leaf-switch-check/pipeline-fixture-v2.spv \
  tools/vulkan_probe/fixture.vert.hlsl
```

Run the NDK `shader-tools/darwin-x86_64/spirv-val` on **each** output using
`--target-env vulkan1.1 --relax-block-layout` before device testing. The native
fixture runners trust these explicitly selected, validated build outputs; their
bounded file/header checks do not replace full SPIR-V validation.

## Device tests

Create a fresh owned shell staging directory with `adb shell mktemp -d
/data/local/tmp/recomp-vulkan.XXXXXX`. Copy only these exact binaries and inputs
there, use mode 0700 for executables and 0600 for inputs, then run (paths below
are relative to that directory):

```sh
timeout 30 ./shader_module_pack_test
timeout 30 ./vulkan_module_probe
timeout 30 ./vulkan_module_probe --modules ./modules-v2.bin
timeout 30 ./vulkan_constants_test ./constants-descriptor32.spv
timeout 30 ./vulkan_pipeline_test ./modules-v2.bin ./pipeline-fixture-v2.spv
```

`modules-v2.bin` is the private `validated-modules.bin` produced by
`xenos_translate_batch.py --constant-binding descriptor32`. The three Vulkan
executables set CPU/core/file/descriptor limits and a process wall alarm too.
The compute fence has a five-second deadline; timeout exits without destroying
potentially in-flight resources. These guards are not a security sandbox or a
guaranteed GPU-driver recovery mechanism. Remove only the exact files and owned
staging directory created for the test. Do not touch the persistent game-content
cache or other application data.

Expected aggregate results:

```text
VULKAN_MODULES_V1 created=53 total=53 binding=2 pipelines=0 draws=0
VULKAN_CONSTANTS_V1 dispatches=2 verified_words=5568 binding=2 shaderInt64_enabled=0
VULKAN_PIPELINES_V1 vertex=30 pixel=23 draws=0 game_pairings_verified=0
```

The module pack has bounded framing, unique numeric indices, supported SPIR-V
capabilities/extensions and stage checks. Vertex interface reflection supports
only scalar/vector 32-bit float/uint/int inputs, with unique locations below 32.
Arrays, matrices and unknown capability requirements fail closed. Its parser
publishes nothing on failure and is covered by host UBSan and native tests.

Pipeline tests use one storage-buffer descriptor per constant binding and one
descriptor per texture/sampler binding. They compile, but never bind data or draw.
Vertex-only pipelines discard rasterization; fragment tests use a synthetic
vertex partner with separate float4 outputs at locations 0..17. The first array-
based partner failed at the first fragment pipeline (VkResult -13); changing
only that partner to matching separate fields made all 23 pass. This is a test
fixture correction, not a recovered game shader pairing.

The compute test uses host-coherent memory, explicit host/compute barriers and
fenced readback. It checks all constant words, field offsets, boolean conversion
and the 32-bit swap-mask helper. Its vertex/pixel inputs now go through the
bounded guest-BE float4 decoder (3,840 float words across two dispatches), including
negative zero, infinities, NaN payloads and subnormals. Expected values come from
the independent source pattern, not the already uploaded buffers. Shared fields
remain explicitly host-managed. It does not test actual guest register-bank
mapping, original
Xenos instruction equivalence, textures, blending, frame presentation, or timing.
Vulkan validation-layer coverage has not been established.

## Explicit-sample pixel arithmetic experiment

`generate_pixel_sample_probe.py REPORT FRESH_PRIVATE_DIRECTORY --shader-index N
--records PRIVATE_RECORDS --constants PRIVATE_CONSTANTS --glslc COMPILER
--spirv-validator VALIDATOR` creates validated compute only. Four BE records each
contain r0, r1 and an explicit sample vec4; sorted referenced constants follow.
`pixel_sample_arithmetic_test MODULE INPUT EXPECTED` runs0/1/4/5 invocations with
real fences, unchanged input and full96-byte output/canary comparison. The
`--corrupt-expected-control` must fail after the zero-work case.

Sample values are supplied by the caller, not fetched from the image fixture.
This does not verify texture-coordinate/sample correspondence, interpolation,
computed LOD, fragment execution, title PM4 retirement, EDRAM or a game frame.
The new analysis is separate from runtime shader acceptance and does not relax
predicate, allocation or cache guards.

`generate_texture32_pixel_probe.py` adds real native32x32 center samples to the
same pixel dataflow (`--rgba`, `--tint`, `--constants`, shader report/index and
compiler/validator arguments). `texture32_pixel_test MODULE INPUT EXPECTED`
uploads4096 owned image bytes and reads BE tint/constants from the input tail.
Only this profile permits up to4176 input bytes. Outputs16,416 bytes with four
fenced work counts0/1/1024/1025; wrong-reference and zero-image controls must fail.
Bit-exact CPU references require binary0/255 image channels. Interpolation and
computed LOD are replaced by texel centers/explicit LOD0, not verified. It remains
compute, with no fragment stage, title resource residency or game frame.

Add `--fragment` to emit a real fragment module and separately compiled synthetic
fullscreen vertex partner. `texture32_fragment_test FRAGMENT_SPV VERTEX_SPV INPUT
EXPECTED` renders0/1/2/3 test triangles to a32x32 RGBA32-float attachment and copies
all pixels between readback canaries after each fence. Invalid math discards
fragments, preserving the clear value. The same two negative-control options
apply. The synthetic vertex partner, raster state and explicit LOD0 are test
choices, not recovered game state. No title command or frame is claimed.

`generate_texture32_vertex_pair.py` builds both captured stages for a restricted
four-vertex strip reference: exact full rectangle, flipped-V UV corners, unit W,
uniform vertex tint and binary-channel32x32 texture. Its two reports may differ;
the proof explicitly records that and never claims GPU-time state. The paired
`texture32_vertex_pair_test FRAGMENT_SPV VERTEX_SPV INPUT EXPECTED` consumes owned
image/pixel constants/vertex bytes/vertex constants (4352 bytes in the captured
case; separate4608-byte cap). UV/tint come from the vertex shader. All raster/
LOD/interface choices remain explicit diagnostic settings, not guest PM4.
Invalid vertex math is not a whole-primitive rejection guarantee.

The explicit-sample pixel generator also accepts up to three fetch results per
record. Each BE record contains r0/r1 followed by one vec4 per executed fetch;
there are four records, then sorted constant vectors. Three-fetch shader2 uses
320 record bytes plus80 constant bytes. Dot products are ordered unfused float32
steps; both conditional branches must validate under the restricted test policy.
This is still an explicit-sample compute test, not real three-plane sampling.

`linear_r8_plane_test` and `raw_linear_r8_plane_probe` exercise two exact linear
R8 layouts: 1280x720 with 1280-byte pitch and 640x360 with 768-byte pitch. The raw
probe reads an exact padded plane from stdin, strips row padding and returns
tight R8 bytes; no GPU work. `decode_linear_r8_capture.py` selects a hash-checked
owned range and literal six-word descriptor from one stopped report. Its metadata
does not claim draw binding, GPU-time contents or a frame.

`r8_planes_pixel_test` / `generate_r8_planes_pixel_probe.py` connect three actual
native R8 uploads to a derived pixel compute program. The tight image prefix is
1,382,400 bytes, followed by four explicit BE r0/r1 records and up to five constant
vectors. Slots 0/1/2 have separate descriptors; r0.xy and r0.zw are preserved.
Each view swizzles R,R,R,1. Linear/repeat/LOD0 and compute execution are diagnostic
choices, not title state. Separate zero-upload controls exist for all three planes.

The bit-exact CPU color reference accepts only uniform binary filter footprints.
Do not assume an ideal fractional weighted average equals native R8 filtering:
the emulator's repeat-boundary samples differed, and isolated measured samples
reproduced the pixel-result discrepancy. Nonuniform footprints now reject
reference generation rather than weakening comparisons. No game draw is retired.

`generate_r8_plane_sampling_fixture.py` creates an independent authored sweep:
four bounded invocations each check 64 values in all three R8 images, covering
all 256 byte values and full R,R,R,1 view output. `r8_planes_integer_filter.comp`
also reports byte-quantized samples at explicit r0.xy/zw coordinates. Its CPU
oracle requires exact integer intermediate and final weighted averages. These
tests run on `r8_planes_pixel_test` but contain no title pixel shader, and do not
establish native intermediate float precision or Xenos sampler behavior.

`r8_planes_fragment_test` / `generate_r8_planes_fragment_probe.py` execute the
three-plane pixel program as a fragment stage with an authored triangle/vec4 UV
partner. The explicit 32x32 RGBA32_SFLOAT target is read back in full. Image data
is followed by a 16-byte tint header and up to five BE constant vectors. Native
fragment tests retain early bounds/coordinate rejection and accumulate arithmetic
validity before one final discard; this equivalent form avoids an observed
many-discard backend-path failure without removing checks. No actual game vertex
partner, title linkage/raster state, EDRAM, predicate or guest draw is implied.

## Native raw-microcode lookup

Create a private binary lookup pack from the fully validated conversion:

```sh
python3 tools/rpf7_inventory/shader_microcode_catalog.py \
  local-analysis/xenos-translation-descriptor32-v3 \
  local-analysis/xenos-translation-descriptor32-v3/NEW-native-catalog.bin \
  --native-pack \
  --spirv-validator /opt/homebrew/share/android-commandlinetools/ndk/27.2.12479018/shader-tools/darwin-x86_64/spirv-val
out/vulkan-probe/shader_catalog_probe local-analysis/xenos-translation-descriptor32-v3/NEW-native-catalog.bin
```

The pack contains private instruction bytes and SPIR-V, never public metadata.
Its writer revalidates source identities before publication. The native parser
checks bounded framing, exact index/stage association and the existing SPIR-V
capability gate; it does not replace full validation or authenticate inputs.
It uses exact byte comparison, not another hash algorithm. Identical translations
deduplicate; different translations of the same stage/code remain ambiguous.
The C++ cache test joins actual PM4 inline inspection to lookup with synthetic
inputs. All 53 selected real artifacts resolve on Mac and Thor. No guest command
stream, external physical memory reader, original stage pairing or draw is bound.
Borrowed lookup results remain valid only while their owning cache is unchanged.

References: [Khronos buffer addresses](https://docs.vulkan.org/guide/latest/buffer_device_address.html),
[core feature requirements](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html),
[shader interfaces](https://docs.vulkan.org/spec/latest/chapters/interfaces.html),
[DXC Vulkan intrinsics](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/SPIR-V.rst).
