# runtime/: the game library's runtime sources

The builder compiles these files, together with the code it translates from **your** discs, into the game library
(the `.bundle` your device loads). They are this project's own code (GPL-3.0-or-later), copied byte for byte from the
source folder release 1.1.0-alpha.1 (library v852) was compiled from. `ppc_context.h` is upstream XenonRecomp's header
(MIT, see `LICENSES/`) with this project's changes.

Some files name the game's functions by address, size or script-function name, to say where the runtime hooks in.
Those are facts needed to run your own copy; no game code or data is here (`tools/release/taint_scan.py` checks every
file against the game's discs, decoded executable, shader pack and generated code).

## Shaders

The `*_spirv.h`, `fg_gen_spirv.h`, `frame_interp_shaders.h` and `generic_*.inc` files hold SPIR-V compiled from the
shader sources next to them (`*.vert`, `*.frag`, `*.comp`, `*.glsl`, `*.hlsl`, `*.hlsli`). Each header's first lines
name its sources and command; recompiling them reproduces the SPIR-V word for word:

```
glslc --target-env=vulkan1.1 -O SOURCE -o OUT.spv                                    # .vert .frag .comp (NDK r27 glslc)
dxc -spirv -T cs_6_0 -E main -fspv-target-env=vulkan1.1 -O3 SOURCE.hlsl -Fo OUT.spv   # .hlsl (DXC 1.9)
```

The `spirv_*.h` and `cexec_overlay.h` files are C++ that edits the translated shaders' SPIR-V while the game runs.

## Not here

- Files the builder generates from your discs (the import bridge, guarded stops, call helpers and vertex-fetch index
  rules): they are made on your machine and never published.
- AMD FidelityFX `ffx_a.h` and `ffx_fsr1.h`: use `third_party/fsr1/` (the same files).
- `loading_label.h` and `map_tap_label.h` (text label bitmaps): held back until they are re-rendered with Nunito Sans;
  until then this folder alone does not build the library.
