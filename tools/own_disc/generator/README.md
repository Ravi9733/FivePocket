# The builder's code generator: XenonRecomp at 16ee5918 plus this project's patch

The builder's codegen stage runs XenonRecomp, built from:

- **Base:** upstream XenonRecomp (MIT, hedge-dev and contributors, https://github.com/hedge-dev/XenonRecomp) at commit
  `16ee5918821004471d1614b442c67039d41558f9`.
- **This project's changes:** `xenonrecomp-16ee5918.patch` (sha256 `c58670ddbe6763bc…`). It changes 5 files
  (`CMakeLists.txt`, `XenonRecomp/recompiler.cpp`, `XenonTests/CMakeLists.txt`, `XenonUtils/ppc_context.h`,
  `XenonUtils/xex.cpp`) and adds 5 tests (`XenonTests/{carry,exact_vector,low_risk,trap,update_form}_recompiler.cc`).

To rebuild the tree: check out the base commit, then `git apply xenonrecomp-16ee5918.patch`. The `git diff` of the result
(without the added files) has sha256 `a53bbd425e1231d460fefde203bf183561cd81f0f0381a274f9865602078624a`. That is the
`diff_sha256` the builder's sources record (`make_sources.py --generator-diff-sha256`) and the Windows package's sources
folder check. This was checked on 2026-11-05: the patch on a clean checkout of the base commit gave that diff, and all 5
added files were byte-identical to the checkout the builds used.

The patch holds no game code or data. Its hexadecimal constants are float bit patterns for the vector instructions.

## Open question for the owner and a lawyer (Q1)

Upstream XenonRecomp carries the XEX2 retail key as `Xex2RetailKey`, declared in `XenonUtils/xex.h`. That file is
unchanged by this patch. The key is used by `XenonUtils/xex.cpp` and `XenonUtils/xex_patcher.cpp` to decrypt the player's
own executable, and the builder's `xex_image` host program links that code. The design is unchanged until that question
is answered.
