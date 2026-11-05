# Translator revision r20260910

The shader translator of this project as it stood on 10 September 2026, kept so that the builder can make one
header again that was written with it: `native_additional_shader_pack.h` (four programs, eight SPIR-V modules).
Today's translator gives other text for those shaders; this revision gives the eight modules byte for byte.

`build_native_packs.py` runs it in a process of its own (`"revision": "r20260910"` in a recipe's call), so its
modules, which have the names of today's, cannot mix with them. Nothing else may import from this folder.

Where the files come from: this repository's own object store. The history between 10 and 28 September lost
trees but kept most file contents. `SOURCES.json` names, per file, the commit whose tree holds it and its blob.

- `inline_shader_translation.py`, `raw_shader_analysis.py`, `scalar_mul_const.py`, `unorm8_vertex_fetch.py`:
  commit 78faa2105 (2026-09-10 05:03 -0400), 22 minutes after the header was written.
- `pixel_sample_dataflow.py`, `vertex_arithmetic_dataflow.py`, `scalar_constant.py`: commit 8ac3931f2 (14:10 the
  same day). The revisions of the first two in commit 78faa2105 are no longer in the object store, and that commit
  had no `scalar_constant.py`. So this folder is a reconstruction, not a checkout of one commit. What makes it the
  right one is the result: all eight modules of the header, equal to the last byte.

The files are unchanged copies: do not edit them. `tools/own_disc/tests/test_translator_history.py` holds their
hashes.
