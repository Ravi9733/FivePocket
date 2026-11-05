# Building FivePocket from source

Nothing here needs the game. You need your own discs only to *run* the builder.

## Layout

| Folder | What |
|---|---|
| `android/` | the Android app (Java + C++ loader), its Gradle project and host tests (`android/tests`) |
| `tools/own_disc/` | the builder: `builder.py` (player front end: check, fetch, build, verify), `disc_builder.py` (the stages), packaging, notices, tests |
| `runtime/` | the game library's runtime and shader sources, compiled by the builder with the code translated from your discs (see `runtime/README.md`) |
| `tools/host_guest_trace/`, `tools/vulkan_probe/`, `tools/rpf7_inventory/` | runtime and archive sources the builder compiles or runs |
| `tools/xex_image/`, `tools/xmem_lzx/`, `tools/xex_info/` | small host programs: executable image decoder, LZX decompressor, header reader |
| `patches/xenonrecomp/`, `patches/xenosrecomp/` and `tools/*recomp_patches/` | our patch queues for the two upstream translators, with their managers and tests |
| `third_party/` | vendored FSR 1, GSR 1 and Nunito Sans with their licences |
| `tools/release/` | release checks and packaging |

## Toolchains

- Python 3.12 (the Windows builder download carries its own).
- CMake 3.24+ and Ninja; a C++20 compiler (Apple clang on macOS, llvm-mingw on Windows).
- JDK 17 and the Android SDK (API 35) for the app; the builder fetches Android NDK r27c itself.
- Windows only: Microsoft's DirectX Shader Compiler v1.9.2607 (fetched by the builder).

## Upstream sources

The builder compiles two upstream translators with our changes:

| Project | Upstream | Our changes |
|---|---|---|
| XenonRecomp (PowerPC to C++) | https://github.com/hedge-dev/XenonRecomp | `patches/xenonrecomp/` |
| XenosRecomp (Xbox 360 shaders) | https://github.com/hedge-dev/XenosRecomp | `patches/xenosrecomp/` |
| FFmpeg (XMA decoder) | https://github.com/xenia-project/FFmpeg at 15ece0882e8d | configuration in `tools/own_disc/xma_decoder/` |

The exact revisions a release was built from, and the complete changed source, are in that release's source archive
(`FivePocket-<version>-source.tar.xz`, file `SOURCES.txt`) and in each builder download's `builder/sources/` folder.

## The app

```
cd android
./gradlew assemblePublic        # unsigned public APK: loader and menus only, no game code
python3 ../android/scripts/validate_project.py
```

The public build refuses any asset other than its two text records (`prePublicBuild`). Sign it with your own key.

## The builder

Run the builder's own tests first:

```
python3 -m unittest discover -s tools/own_disc/tests
```

A builder download is a folder with the tools, the host programs, a profile and an inventory (`release.json`) that
`builder.py check` verifies before it runs. `tools/own_disc/make_release.py` writes that inventory;
`tools/own_disc/make_notices.py` and `make_sources.py` write its notices and corresponding source.

## Release checks

```
python3 tools/release/run_checks.py
```

Before a release the maintainer also runs `tools/release/taint_scan.py` against their own discs and builder outputs,
and `tools/release/package_release.py` to make the downloads; see [RELEASING.md](RELEASING.md).
