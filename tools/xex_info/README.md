# XEX2 metadata inspector

`xex_info.py` reads bounded, non-secret metadata from an Xbox 360 XEX2 header
and emits JSON. It reports the title/media/build identifiers, disc numbering,
entry point, image layout, and compression mode without extracting executable
code, exposing cryptographic material, or modifying the input.

Optional TLS metadata (`0x00020104`) is reported as a schema-v2 additive
`tls_info` object containing `slot_count`, `raw_data_address`, `data_size`, and
`raw_data_size`. The inspector requires the fixed 16-byte record to remain
inside the declared XEX header, rejects duplicate optional-header keys, and
bounds TLS storage to 2,048 four-byte dynamic slots plus at most 1 MiB of
four-byte-aligned static data. The initialized template may not be larger than
the static area; when a decoded image is supplied, that template must be fully
contained within it. Only its address and sizes are emitted, never its bytes.

When the caller also supplies the private decoded in-memory image, the tool
parses the bounded XEX import-library table and classifies logical imports. A
type-zero descriptor followed by a matching type-one thunk is a function
import; an unpaired type-zero descriptor is a data-import slot. Both raw images
and the pinned XenonRecomp loader's byte-swapped/fixed-stub representation are
accepted. The JSON reports library names, ordinals, slot addresses, and counts,
but never emits image bytes, cryptographic data, hashes, or input paths.

```sh
python3 tools/xex_info/xex_info.py /path/to/default.xex --pretty
python3 tools/xex_info/xex_info.py /path/to/default.xex \
  --decoded-image /private/path/to/default.image.bin --pretty
python3 -m unittest discover -s tools/xex_info/tests -v
```
