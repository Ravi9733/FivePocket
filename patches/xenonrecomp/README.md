# XenonRecomp patch queue

These patches are maintained outside the pinned `tools/XenonRecomp` submodule
so a checkout remains reproducible and the changes can be submitted upstream.
They contain no game code or game-derived data.

`0019-normshort4-vector-pack-unpack.patch` implements type4 four-component signed
16-bit packing/unpacking in biased float representations. Unpacking -32768
produces quiet NaN; packing saturates to -32767..32767, with unordered inputs
selecting the lower bound. The integer-only helper preserves FP state, supports
aliases and all twelve mask/shift combinations, including mask3/shift3's XY lane.
The incremental equivalent is `android/app/src/main/cpp/guest_vmx_short.h`.
Tests compile actual emitted helpers/calls against an independent numeric fixture
covering every signed16 encoding, saturation boundaries, aliases and lane masks.
Primary reference: [Xenia's pinned SHORT_4 backend](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/backend/x64/x64_seq_vector.cc).

`0017-normpacked32-vector-unpack.patch` and `0018-normpacked32-vector-pack.patch`
implement the type2 10:10:10:2 vector conversions. These operate on biased float
bit patterns; the surrounding guest code applies the normalization arithmetic.
Unpacking signed -512 produces quiet NaN. Packing saturates XYZ to -511..511
and W to0..3 in their biased representations; unordered inputs select the lower
bound. Integer operations preserve host FP state. Source/destination aliases,
all valid mask/shift combinations and untouched lanes are tested explicitly.
The incremental native helper lives in `android/app/src/main/cpp/guest_vmx_packed.h`;
the full queue embeds packing in ppc_context.h and emits unpacking bit expressions.
The queue's generated calls/expressions are compiled and tested against independent
numeric fixtures by the same unittest command below, without editing the submodule.
Primary reference: [Xenia's pinned vector backend](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/backend/x64/x64_seq_vector.cc),
[published packing vectors](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/ppc/testing/instr_vpkd3d128.s).

`0016-d3d-color-pack-nan-to-zero.patch` corrects D3DCOLOR packing of unordered
inputs. The lower clamp must also handle NaNs: matrix routines pack all-ones
NaN vectors to create zero mask lanes. Retaining the NaN payload instead can
leave a coordinate's W lane unmasked. Finite values and lane placement are
unchanged. Reference: [Xenia's D3DCOLOR pack implementation](https://github.com/xenia-project/xenia/blob/master/src/xenia/cpu/backend/x64/x64_seq_vector.cc#L2101).
The numeric regression applies the full queue to a temporary Git index, extracts
the actual emitted clamp and compiles/runs it on synthetic finite/NaN inputs;
the working submodule is not modified:

```sh
python3 -m unittest discover -s tools/xenonrecomp_patches/tests -v
```

`0015-half-vector-pack-unpack.patch` adds FLOAT16_2/FLOAT16_4 conversion for
`vpkd3d128` and `vupkd3d128`. Packing truncates toward zero, saturates finite
overflow, preserves signed zero and quiets NaNs. Reversed host lanes, partial
writes, mask3/shift3 and source/destination aliasing are handled explicitly.
The portable helper is mirrored in `android/app/src/main/cpp/guest_vmx_half.h`
for incremental private builds without changing the entire generated context.
The queue embeds the same definitions in ppc_context.h for future regeneration.
Sources: [Xenia pack backend](https://github.com/xenia-project/xenia/blob/master/src/xenia/cpu/backend/x64/x64_seq_vector.cc#L2122),
[pack fixtures](https://github.com/xenia-project/xenia/blob/master/src/xenia/cpu/ppc/testing/instr_vpkd3d128.s#L215),
[unpack fixtures](https://github.com/xenia-project/xenia/blob/master/src/xenia/cpu/ppc/testing/instr_vupkd3d128.s).
Focused regression (run from the repository root; output remains private):

```sh
clang++ -std=c++20 -O2 -UNDEBUG -Wall -Wextra -Werror \
  -Iandroid/app/src/main/cpp tools/host_guest_trace/tests/guest_vmx_half_test.cpp \
  -o local-analysis/repro-tag-leaf/guest-vmx-half-test
local-analysis/repro-tag-leaf/guest-vmx-half-test
```

Build the pinned dependencies, apply the queue, then rebuild XenonRecomp:

```sh
python3 tools/build_host_tools.py
python3 tools/xenonrecomp_patches/manage.py check
python3 tools/xenonrecomp_patches/manage.py apply
cmake --build out/host-tools/xenon --parallel
out/host-tools/xenon/XenonTests/XenonTranslationTests
```

`tools/build_host_tools.py` deliberately rejects a dirty submodule, so do not
run that wrapper between `apply` and `reverse`. The direct CMake rebuild above
is the expected patched-tool workflow.

Return the submodule to its pinned, clean state with:

```sh
python3 tools/xenonrecomp_patches/manage.py reverse
```

`0001-conditional-count-register-branches.patch` fixes direct `bdz`/`bdnz`
branches that cross an analyzed function boundary, implements `bdzf` and
`bdnzt`, and evaluates their full condition-register bit operand rather than
assuming the EQ bit. On the current private GTA V baseline this removes 2,625
unsupported-instruction sites (5,842 to 3,217) and makes all 373 generated C++
chunks compile on ARM64 macOS. Branches into the middle of a separately inferred
function still require corrected function boundaries; the patch emits the
existing unresolved-target diagnostic for those paths. This is a
translation-coverage result, not a claim of behavioral correctness for the
whole game.

`0002-scalar-update-form-load-store.patch` implements the 20 missing scalar
update-form load/store variants (`lbzux`, `ldux`, `lfdu[x]`, `lfsu[x]`,
`lhau[x]`, `lhzu[x]`, `lwaux`, `lwzux`, `stbux`, `stdux`, `stfdu[x]`,
`stfsu[x]`, and `sthu[x]`). Effective addresses are computed once, memory
access occurs before base-register writeback, stores preserve the old source
value when it aliases the updated base register, and writeback replaces the
complete 64-bit GPR.
The included `XenonTranslationTests` target checks those generated-code
invariants. On the current private GTA V baseline, applying this after `0001`
removes another 2,341 unsupported sites (3,217 to 876); all 373 generated C++
chunks compile for Android ARM64 with NDK 27.2.12479018.

`0003-bound-pe-section-names.patch` bounds Xbox PE section names to their
fixed eight-byte field. Without it, non-null-terminated names can absorb bytes
from adjacent header fields and produce unstable metadata or out-of-bounds
reads.

`0004-arm64-fpcr-operand-width.patch` widens the host operand passed to the
AArch64 `msr fpcr` instruction. This matches the required `Xt` register width
and removes the warning emitted once by every generated translation unit; using
Clang's suggested `%w0` modifier is not valid for this system-register form.

`0005-scalar-integer-carry.patch` implements `addc`, `addme`, and `subfze`,
including their overflow-enabled and record forms. Carry is captured before a
possibly aliased destination write, OE updates XER.OV and sticky XER.SO, and RC
records the completed result only after XER state is final. Focused translation
tests cover carry, signed-overflow, alias, and record ordering. On the current
private GTA V baseline this removes 548 unsupported sites (876 to 328); all 373
generated chunks plus the function mapping compile for Android ARM64 with zero
warnings.

`0006-low-risk-vmx-cache.patch` implements the bounded low-risk remainder:
`dcbst` as a documented coherent-host cache hint/no-op, `vnor128`, unsigned
`vcmpgtuw.` with CR6 all/none summary semantics, `vsubuwm`, `vsrb`, `lvehx`,
and `stvebx`. The element load/store mappings follow Xenon's reversed vector
lane convention; `lvehx` deliberately matches the full aligned-vector behavior
used by the existing `lvewx` implementation and current Xenia. Focused tests
cover emitted SIMD operations, destination alias safety, CR6 update ordering,
and effective-address element selection. On the current private GTA V baseline
this removes 176 unsupported sites (328 to 152); all 373 generated chunks plus
the function mapping compile for Android ARM64 with zero warnings. The remaining
`frsqrte`, `vpkuwus128`, `vrfip128`, `vsl`, `vslo128`, and `vsro128` sites require
additional semantic validation.

`0007-exact-vector-operations.patch` implements `vrfip`, `vpkuwus`, `vsl`,
`vslo`, and `vsro`, including the decoded VMX128 variants. Whole-vector shifts
use XenonRecomp's reversed byte layout and the architected shift-count fields;
unsigned word packing clamps before signed host packing so inputs with their
high bit set saturate to `0xFFFF` instead of zero. The focused tests execute
golden shift vectors, unsigned saturation boundaries, and base/VMX128
translation paths. `VSCR[SAT]` remains unmodeled, but the current generated
corpus contains no `mfvscr` or `mtvscr` sites. On the current private GTA V
baseline this removes 32 unsupported sites (152 to 120); the only remaining
unsupported mnemonic is the implementation-dependent `frsqrte`. All 373
generated chunks plus the function mapping compile for Android ARM64 with zero
warnings.

`0008-correct-vector-bound-and-d3d-pack.patch` replaces the `vcmpbfp` runtime
trap with lane-wise bound results, unordered handling, and record-form CR6
state. It also implements all architected D3D-color mask/shift placements while
preserving untouched destination lanes in XenonRecomp's reversed vector layout.
Focused translation tests cover the six mask/shift variants present in the
private GTA V image plus the special mask-3/shift-3 behavior. On that image the
patch removes all 439 unexpected D3D-color diagnostics and the sole missing-CR
diagnostic; it does not alter the remaining 120 `frsqrte` sites.

`0009-frsqrte-compatibility.patch` provides a documented boot-compatibility
fallback for scalar double-precision reciprocal-square-root estimates. It uses
the host-exact `1.0 / sqrt(x)`, which preserves the required result classes and
the architecture's estimate error bound, and tests positive, signed-zero,
infinity, negative, and NaN inputs. It intentionally does not claim Xenon's
implementation-dependent estimate bits or currently unmodeled FPSCR exception
and result-class side effects. This removes the final 120 unsupported sites from
the private GTA V image so runtime bring-up can proceed with that fidelity gap
recorded explicitly.

`0010-instrumentable-function-prologue.patch` keeps the default generated-code
prologue behavior unchanged while adding an opt-in `PPC_RUNTIME_CHECKPOINT()`
hook. Private execution builds can use it to enforce a cooperative generated
function-entry budget; normal builds receive a no-op. The executable Xenon test
confirms that a caller-supplied checkpoint runs exactly once per prologue. This
is not an instruction or wall-clock timeout and cannot stop a tight loop within
one generated function.

`0011-word-switch-selector.patch` selects switch cases using the low 32 bits of
the guest register, matching the `cmplwi` guard used by switch discovery. An
unrelated high word must not cause a valid 32-bit selector to enter an
unreachable default. The synthetic translator test checks the emitted word
selector and local destination. Bounded recovery of a reordered/NOP-padded
u16 table is provided separately by `tools/fp_codegen/switch_tables.py`.

`0012-guarded-fixed-point-traps.patch` implements encoded TW/TWI/TD/TDI trap
conditions, including aliases previously emitted as no-ops. Word operations
compare only 32 bits; doubleword operations compare 64 bits; immediates are
sign-extended, and RA=0 reads r0. The unsigned representation helper avoids
host signed overflow, covers every TO mask, and preserves registers/CR/XER.
True conditions call `PPC_RUNTIME_TRAP()`, defaulting to abort. The private
trace overrides that hook with a structured `guest-trap` stop, not SEH handling.
`XenonTrapTranslationTests` checks real decoded encodings and comparison/alias
boundaries. This corrects silent assertion skipping; earlier zero-unsupported
diagnostic counts never established that trap semantics were implemented.

`0013-runtime-timebase-hook.patch` routes exact encoded TB/TBU reads (selectors
268/269, both mftb and mfspr aliases) through `PPC_READ_TIMEBASE()`. Full TB reads
retain all 64 bits; TBU reads zero-extend the upper word. The default hook keeps
the upstream raw-host-counter behavior, while the private native trace overrides
it with a shared monotonic 50 MHz clock and matching kernel frequency query.
`XenonTrapTranslationTests` additionally checks all 128 register/selector/encoding
combinations and a counted caller-supplied hook. Unknown selectors do not acquire
guessed timebase semantics. This is not a cycle-accurate CPU or GPU clock model.

`0014-validate-live-switch-target.patch` checks the computed low-word CTR against
each recovered case before entering it. A mismatched target or unexpected selector
uses the existing indirect-transfer path and returns, instead of selecting stale
metadata or invoking undefined behavior. The guarded native harness accepts only
exact global function mappings on that fallback. Original table loads, address
calculations and interleaved stores remain emitted; no guest table is rewritten.
The translator test checks emitted ordering. `tools/fp_codegen/switch_target_fixture.py`
extracts an exact actual generated switch into a private executable fragment test,
covering matching/mismatched CTR, out-of-range selectors and unrelated high words
without executing any game case body.
