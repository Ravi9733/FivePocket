# Credits

## Where FivePocket comes from

FivePocket began as a fork of **SanRecomp** by Prince Ozordi (first commit 2026-01-02), an unfinished static
recompilation project for the same game on PC. SanRecomp in turn grew out of the work of the hedge-dev community:

- **Unleashed Recompiled** (hedge-dev and contributors): the static-recompilation PC port whose runtime design,
  kernel layer and credits SanRecomp inherited. Its credits list: Skyth, Sajid, Hyper, Darío, DeaThProj,
  RadiantDerg, PTKay, SuperSonic16, NextinHKRY, LadyLunanova, LJSTAR, saguinee, Goalringmod27, M&M, DaGuAr,
  brianuuuSonic, Kitzuku.
- **Marathon Recompiled** (sonicnext-dev and contributors): a later recompilation in the same family.
- **XenonRecomp** and **XenosRecomp** (hedge-dev and contributors): the PowerPC-to-C++ translator and the Xbox 360
  shader translator FivePocket's builder uses, with the patch sets in `patches/`.

The PC runtime, installer and desktop front end of SanRecomp are **not** part of FivePocket. This tree was rebuilt
for Android and iOS: the Android app, the renderer, the builder and the release tools here are new work.

## Modification notice (GPL-3.0 section 5(a))

- 2026-01 to 2026-11: modified by Marcus Fequiere and the FivePocket contributors. The upstream SanRecomp desktop
  code was removed; an Android app, a Vulkan renderer for the translated shaders, a disc-to-device builder for macOS
  and Windows, an iOS and Duo app built on the player's Mac, and release checks were written.
- 2026-11-05: first public source tree made from an allowlisted export; no upstream history is included.

Any file that still carries upstream code keeps its upstream copyright and licence notices.

## Open-source parts

FivePocket also stands on:

- **Xenia** (Ben Vanik and contributors) and **ReXGlue** (Tom Clay): texture, shader-microcode and GPU behaviour that
  parts of the renderer adapt, under BSD 3-Clause;
- **AMD FidelityFX Super Resolution 1** and **Qualcomm Snapdragon Game Super Resolution 1**: optional upscalers;
- **FFmpeg** (xenia-project fork): the XMA sound decoder, built from source on your machine (LGPL 2.1 or later);
- **SIMDe**, **{fmt}**, **toml++**, **xxHash**, **Zstandard**, **SMOL-V**, **tiny-AES-c**, **TinySHA1**, **libmspack**
  and the GNU binutils PowerPC disassembler, inside the translators;
- **MoltenVK** (iOS), the **Android NDK** runtime, **LLVM** and **MinGW-w64** runtimes, **CPython** (Windows builder);
- **Nunito Sans** (The Nunito Sans Project Authors), the user-interface font, under the SIL Open Font License 1.1.

The full list with terms is in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

The idea of static recompilation for consoles owes much to **N64: Recompiled** (Mr-Wiseguy and contributors).
