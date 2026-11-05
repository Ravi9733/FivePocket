# Private XEX2 image decoder

`xex_image` uses the pinned XenonRecomp loader to decrypt and expand a lawful
Xbox 360 XEX2 into its in-memory image layout. The decoded binary is game-derived
and must remain outside Git; stdout contains only bounded layout metadata.

```sh
cmake -S tools/xex_image -B out/host-tools/xex-image -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build out/host-tools/xex-image --parallel

out/host-tools/xex-image/xex_image \
  /path/to/default.xex \
  /path/to/private-analysis/default.image.bin
```

This is a local reverse-engineering handoff, not a redistributable game binary.
It supports only the XEX formats handled by the pinned XenonRecomp loader and
refuses to overwrite an existing output file.
