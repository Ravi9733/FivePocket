#!/usr/bin/env python3
"""Read non-secret build metadata from an Xbox 360 XEX2 header."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import sys
from typing import Sequence


SCHEMA_VERSION = 2
XEX2_MAGIC = b"XEX2"

XEX_HEADER_FILE_FORMAT_INFO = 0x000003FF
XEX_HEADER_ENTRY_POINT = 0x00010100
XEX_HEADER_IMAGE_BASE_ADDRESS = 0x00010201
XEX_HEADER_IMPORT_LIBRARIES = 0x000103FF
XEX_HEADER_TLS_INFO = 0x00020104
XEX_HEADER_EXECUTION_INFO = 0x00040006

XEX_IMPORT_DESCRIPTOR_SIZE = 40
XEX_IMPORT_TYPE_DATA = 0
XEX_IMPORT_TYPE_FUNCTION = 1

XEX_TLS_INFO_SIZE = 16
XEX_TLS_SLOT_SIZE = 4
MAX_TLS_SLOT_COUNT = 2048
MAX_TLS_STATIC_DATA_SIZE = 1024 * 1024
MAX_TLS_TOTAL_SIZE = MAX_TLS_STATIC_DATA_SIZE + (
    MAX_TLS_SLOT_COUNT * XEX_TLS_SLOT_SIZE
)

ENCRYPTION_TYPES = {0: "none", 1: "normal"}
COMPRESSION_TYPES = {0: "none", 1: "basic", 2: "normal", 3: "delta"}


class XexFormatError(ValueError):
    """Raised when the bounded XEX2 metadata reader finds malformed input."""


def _require(data: bytes, offset: int, size: int, description: str) -> None:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise XexFormatError(f"{description} extends beyond the file")


def _u16(data: bytes, offset: int, description: str) -> int:
    _require(data, offset, 2, description)
    return struct.unpack_from(">H", data, offset)[0]


def _u32(data: bytes, offset: int, description: str) -> int:
    _require(data, offset, 4, description)
    return struct.unpack_from(">I", data, offset)[0]


def _u32le(data: bytes, offset: int, description: str) -> int:
    _require(data, offset, 4, description)
    return struct.unpack_from("<I", data, offset)[0]


def _version(value: int) -> str:
    return f"{value >> 28}.{(value >> 24) & 0xF}.{(value >> 8) & 0xFFFF}.{value & 0xFF}"


def _parse_import_strings(data: bytes, offset: int, size: int, count: int) -> list[str]:
    _require(data, offset, size, "import-library string table")
    strings: list[str] = []
    cursor = 0
    for _ in range(count):
        if cursor >= size:
            raise XexFormatError("import-library string count exceeds its table")
        start = offset + cursor
        terminator = data.find(b"\0", start, offset + size)
        if terminator < 0:
            raise XexFormatError("import-library string is not terminated")
        try:
            strings.append(data[start:terminator].decode("ascii"))
        except UnicodeDecodeError as error:
            raise XexFormatError("import-library name is not ASCII") from error
        cursor = ((terminator - offset + 1) + 3) & ~3
        if cursor > size:
            raise XexFormatError("import-library string padding exceeds its table")
    return strings


def _is_xenonrecomp_function_stub(
    decoded_image: bytes,
    image_base: int,
    address: int,
) -> bool:
    if address < image_base:
        return False
    offset = address - image_base
    return (
        offset + 16 <= len(decoded_image)
        and struct.unpack_from("<IIII", decoded_image, offset)
        == (0x00000060, 0x00000060, 0x00000060, 0x2000804E)
    )


def _looks_like_xenonrecomp_descriptor(
    decoded_image: bytes,
    image_base: int,
    address: int,
) -> bool:
    if address < image_base or (address & 3) != 0:
        return False
    offset = address - image_base
    if offset + 4 > len(decoded_image):
        return False
    big_type = (_u32(decoded_image, offset, "import record") >> 24) & 0xFF
    little_type = (
        _u32le(decoded_image, offset, "postprocessed import record") >> 24
    ) & 0xFF
    return (
        big_type not in (XEX_IMPORT_TYPE_DATA, XEX_IMPORT_TYPE_FUNCTION)
        and little_type in (XEX_IMPORT_TYPE_DATA, XEX_IMPORT_TYPE_FUNCTION)
    )


def _read_import_record(
    decoded_image: bytes,
    image_base: int,
    address: int,
    postprocessed: bool,
) -> tuple[int, int | None]:
    if address < image_base or (address & 3) != 0:
        raise XexFormatError("import record is unaligned or below the decoded image")
    offset = address - image_base
    _require(decoded_image, offset, 4, "import record")
    # The pinned XenonRecomp image decoder byte-swaps descriptor words in place
    # and replaces function thunks with a fixed four-instruction return stub.
    # Accept that deterministic postprocessed representation in addition to a
    # raw decoded image; the preceding descriptor retains the function ordinal.
    if postprocessed:
        if _is_xenonrecomp_function_stub(decoded_image, image_base, address):
            return XEX_IMPORT_TYPE_FUNCTION, None
        value = _u32le(decoded_image, offset, "postprocessed import record")
    else:
        value = _u32(decoded_image, offset, "import record")
    record_type = (value >> 24) & 0xFF
    if record_type not in (XEX_IMPORT_TYPE_DATA, XEX_IMPORT_TYPE_FUNCTION):
        raise XexFormatError(f"import record has unsupported type {record_type}")
    return record_type, value & 0xFFFF


def _parse_import_libraries(
    data: bytes,
    offset: int,
    header_size: int,
    decoded_image: bytes | None,
    image_base: int,
) -> dict[str, object]:
    _require(data, offset, 12, "import-library header")
    total_size = _u32(data, offset, "import-library size")
    string_table_size = _u32(data, offset + 4, "import-library string-table size")
    string_count = _u32(data, offset + 8, "import-library string count")
    if total_size < 12 or offset + total_size > header_size:
        raise XexFormatError("import-library table exceeds the declared XEX header")
    _require(data, offset, total_size, "import-library table")
    if string_table_size > total_size - 12:
        raise XexFormatError("import-library string table exceeds its parent table")

    strings = _parse_import_strings(data, offset + 12, string_table_size, string_count)
    cursor = 12 + string_table_size
    libraries: list[dict[str, object]] = []
    data_imports: list[dict[str, object]] = []
    total_records = 0
    total_imports = 0
    total_functions = 0

    while cursor < total_size:
        remaining = total_size - cursor
        if remaining < XEX_IMPORT_DESCRIPTOR_SIZE:
            if any(data[offset + cursor:offset + total_size]):
                raise XexFormatError("trailing import-library bytes are not zero padding")
            break

        library_offset = offset + cursor
        library_size = _u32(data, library_offset, "import-library descriptor size")
        if library_size < XEX_IMPORT_DESCRIPTOR_SIZE or (library_size & 3) != 0:
            raise XexFormatError("import-library descriptor has an invalid size")
        if library_size > remaining:
            raise XexFormatError("import-library descriptor exceeds its parent table")

        library_id = _u32(data, library_offset + 24, "import-library ID")
        version = _u32(data, library_offset + 28, "import-library version")
        minimum_version = _u32(
            data, library_offset + 32, "import-library minimum version"
        )
        raw_name_index = _u16(data, library_offset + 36, "import-library name index")
        name_index = raw_name_index & 0xFF
        record_count = _u16(data, library_offset + 38, "import record count")
        required_size = XEX_IMPORT_DESCRIPTOR_SIZE + record_count * 4
        if required_size > library_size:
            raise XexFormatError("import record array exceeds its library descriptor")
        if name_index >= len(strings):
            raise XexFormatError("import-library name index exceeds the string table")

        record_addresses = [
            _u32(
                data,
                library_offset + XEX_IMPORT_DESCRIPTOR_SIZE + index * 4,
                "import record address",
            )
            for index in range(record_count)
        ]
        library_result: dict[str, object] = {
            "name": strings[name_index],
            "id": f"0x{library_id:08X}",
            "version_raw": f"0x{version:08X}",
            "version": _version(version),
            "minimum_version_raw": f"0x{minimum_version:08X}",
            "minimum_version": _version(minimum_version),
            "record_count": record_count,
        }

        if decoded_image is not None:
            postprocessed = any(
                _is_xenonrecomp_function_stub(decoded_image, image_base, address)
                or _looks_like_xenonrecomp_descriptor(
                    decoded_image,
                    image_base,
                    address,
                )
                for address in record_addresses
            )
            records = [
                (*_read_import_record(
                    decoded_image,
                    image_base,
                    address,
                    postprocessed,
                ), address)
                for address in record_addresses
            ]
            library_data_imports: list[dict[str, object]] = []
            function_count = 0
            import_count = 0
            record_index = 0
            while record_index < len(records):
                record_type, ordinal, address = records[record_index]
                if record_type != XEX_IMPORT_TYPE_DATA:
                    raise XexFormatError("function import thunk has no preceding descriptor")
                import_count += 1
                if record_index + 1 < len(records) and records[record_index + 1][0] == XEX_IMPORT_TYPE_FUNCTION:
                    function_type, function_ordinal, _ = records[record_index + 1]
                    assert function_type == XEX_IMPORT_TYPE_FUNCTION
                    if function_ordinal is not None and function_ordinal != ordinal:
                        raise XexFormatError("function thunk ordinal does not match its descriptor")
                    function_count += 1
                    record_index += 2
                    continue
                entry = {
                    "ordinal": ordinal,
                    "slot_address": f"0x{address:08X}",
                }
                library_data_imports.append(entry)
                data_imports.append({"library": strings[name_index], **entry})
                record_index += 1

            library_result.update(
                {
                    "import_count": import_count,
                    "function_import_count": function_count,
                    "data_import_count": len(library_data_imports),
                    "data_imports": library_data_imports,
                }
            )
            total_imports += import_count
            total_functions += function_count

        libraries.append(library_result)
        total_records += record_count
        cursor += library_size

    result: dict[str, object] = {
        "decoded_records": decoded_image is not None,
        "library_count": len(libraries),
        "record_count": total_records,
        "libraries": libraries,
    }
    if decoded_image is not None:
        result.update(
            {
                "import_count": total_imports,
                "function_import_count": total_functions,
                "data_import_count": len(data_imports),
                "data_imports": data_imports,
            }
        )
    return result


def _parse_tls_info(
    data: bytes,
    offset: int,
    header_size: int,
    decoded_image: bytes | None,
    image_base: int,
) -> dict[str, object]:
    if offset > header_size or XEX_TLS_INFO_SIZE > header_size - offset:
        raise XexFormatError("TLS information exceeds the declared XEX header")
    _require(data, offset, XEX_TLS_INFO_SIZE, "TLS information")

    slot_count = _u32(data, offset, "TLS slot count")
    raw_data_address = _u32(data, offset + 4, "TLS raw-data address")
    data_size = _u32(data, offset + 8, "TLS data size")
    raw_data_size = _u32(data, offset + 12, "TLS raw-data size")

    if slot_count > MAX_TLS_SLOT_COUNT:
        raise XexFormatError("TLS slot count exceeds the supported bound")
    if data_size > MAX_TLS_STATIC_DATA_SIZE:
        raise XexFormatError("TLS static data exceeds the supported bound")
    if data_size & (XEX_TLS_SLOT_SIZE - 1):
        raise XexFormatError("TLS static data size is not 4-byte aligned")
    if raw_data_size > data_size:
        raise XexFormatError("TLS raw-data size exceeds its static data size")

    slot_storage_size = slot_count * XEX_TLS_SLOT_SIZE
    if slot_storage_size > 0xFFFFFFFF - data_size:
        raise XexFormatError("TLS total storage size overflows 32 bits")
    if slot_storage_size + data_size > MAX_TLS_TOTAL_SIZE:
        raise XexFormatError("TLS total storage exceeds the supported bound")

    if raw_data_size > 0:
        if raw_data_address > 0xFFFFFFFF - raw_data_size + 1:
            raise XexFormatError("TLS raw-data range crosses the 32-bit guest boundary")
        if decoded_image is not None:
            if raw_data_address < image_base:
                raise XexFormatError("TLS raw-data template is below the decoded image")
            raw_data_offset = raw_data_address - image_base
            if (
                raw_data_offset > len(decoded_image)
                or raw_data_size > len(decoded_image) - raw_data_offset
            ):
                raise XexFormatError("TLS raw-data template exceeds the decoded image")

    return {
        "slot_count": slot_count,
        "raw_data_address": f"0x{raw_data_address:08X}",
        "data_size": data_size,
        "raw_data_size": raw_data_size,
    }


def parse_xex2(data: bytes, decoded_image: bytes | None = None) -> dict[str, object]:
    _require(data, 0, 24, "XEX2 header")
    if data[:4] != XEX2_MAGIC:
        raise XexFormatError("not an XEX2 file")

    module_flags = _u32(data, 4, "module flags")
    header_size = _u32(data, 8, "header size")
    security_offset = _u32(data, 16, "security offset")
    header_count = _u32(data, 20, "optional-header count")
    _require(data, 24, header_count * 8, "optional-header table")
    if header_size > len(data):
        raise XexFormatError("declared header size exceeds the file")

    optional_headers: dict[int, int] = {}
    for index in range(header_count):
        offset = 24 + index * 8
        key = _u32(data, offset, "optional-header key")
        value = _u32(data, offset + 4, "optional-header value")
        if key in optional_headers:
            raise XexFormatError(f"duplicate optional-header key 0x{key:08X}")
        optional_headers[key] = value

    # These offsets match Xex2SecurityInfo without exposing keys or signatures.
    _require(data, security_offset, 0x184, "security information")
    security_image_size = _u32(data, security_offset + 0x04, "image size")
    security = {
        "image_size": security_image_size,
        "image_flags": f"0x{_u32(data, security_offset + 0x10C, 'image flags'):08X}",
        "load_address": f"0x{_u32(data, security_offset + 0x110, 'load address'):08X}",
        "region": f"0x{_u32(data, security_offset + 0x178, 'region'):08X}",
        "allowed_media_types": f"0x{_u32(data, security_offset + 0x17C, 'allowed media types'):08X}",
        "page_descriptor_count": _u32(
            data, security_offset + 0x180, "page-descriptor count"
        ),
    }

    result: dict[str, object] = {
        "schema_version": SCHEMA_VERSION,
        "file_size": len(data),
        "module_flags": f"0x{module_flags:08X}",
        "header_size": header_size,
        "optional_header_count": header_count,
        "security": security,
    }

    if XEX_HEADER_ENTRY_POINT in optional_headers:
        result["entry_point"] = f"0x{optional_headers[XEX_HEADER_ENTRY_POINT]:08X}"
    if XEX_HEADER_IMAGE_BASE_ADDRESS in optional_headers:
        result["image_base_address"] = (
            f"0x{optional_headers[XEX_HEADER_IMAGE_BASE_ADDRESS]:08X}"
        )

    image_base = optional_headers.get(
        XEX_HEADER_IMAGE_BASE_ADDRESS,
        int(str(security["load_address"]), 16),
    )
    if decoded_image is not None:
        if len(decoded_image) == 0:
            raise XexFormatError("decoded image is empty")
        if len(decoded_image) > security_image_size:
            raise XexFormatError("decoded image exceeds XEX security image size")
        if image_base + len(decoded_image) > 0x1_0000_0000:
            raise XexFormatError("decoded image crosses the 32-bit guest boundary")

    tls_offset = optional_headers.get(XEX_HEADER_TLS_INFO)
    if tls_offset is not None:
        result["tls_info"] = _parse_tls_info(
            data,
            tls_offset,
            header_size,
            decoded_image,
            image_base,
        )

    imports_offset = optional_headers.get(XEX_HEADER_IMPORT_LIBRARIES)
    if imports_offset is not None:
        result["import_libraries"] = _parse_import_libraries(
            data,
            imports_offset,
            header_size,
            decoded_image,
            image_base,
        )

    execution_offset = optional_headers.get(XEX_HEADER_EXECUTION_INFO)
    if execution_offset is not None:
        _require(data, execution_offset, 24, "execution information")
        media_id = _u32(data, execution_offset, "media ID")
        version = _u32(data, execution_offset + 4, "version")
        base_version = _u32(data, execution_offset + 8, "base version")
        title_id = _u32(data, execution_offset + 12, "title ID")
        result["execution_info"] = {
            "media_id": f"{media_id:08X}",
            "version_raw": f"0x{version:08X}",
            "version": _version(version),
            "base_version_raw": f"0x{base_version:08X}",
            "base_version": _version(base_version),
            "title_id": f"{title_id:08X}",
            "platform": data[execution_offset + 16],
            "executable_type": data[execution_offset + 17],
            "disc_number": data[execution_offset + 18],
            "disc_count": data[execution_offset + 19],
            "savegame_id": f"{_u32(data, execution_offset + 20, 'savegame ID'):08X}",
        }

    format_offset = optional_headers.get(XEX_HEADER_FILE_FORMAT_INFO)
    if format_offset is not None:
        _require(data, format_offset, 8, "file-format information")
        info_size = _u32(data, format_offset, "file-format information size")
        if info_size < 8:
            raise XexFormatError("file-format information is shorter than its header")
        _require(data, format_offset, info_size, "file-format information")
        encryption = _u16(data, format_offset + 4, "encryption type")
        compression = _u16(data, format_offset + 6, "compression type")
        result["file_format"] = {
            "info_size": info_size,
            "encryption_type": encryption,
            "encryption": ENCRYPTION_TYPES.get(encryption, "unknown"),
            "compression_type": compression,
            "compression": COMPRESSION_TYPES.get(compression, "unknown"),
        }

    return result


def inspect_file(path: Path, decoded_image_path: Path | None = None) -> dict[str, object]:
    with path.open("rb") as stream:
        data = stream.read()
    metadata = parse_xex2(data)
    if decoded_image_path is None:
        return metadata

    decoded_size = decoded_image_path.stat().st_size
    security_image_size = int(metadata["security"]["image_size"])  # type: ignore[index]
    if decoded_size <= 0:
        raise XexFormatError("decoded image is empty")
    if decoded_size > security_image_size:
        raise XexFormatError("decoded image exceeds XEX security image size")
    with decoded_image_path.open("rb") as stream:
        decoded_image = stream.read(decoded_size + 1)
    if len(decoded_image) != decoded_size:
        raise XexFormatError("decoded image changed size while being read")
    return parse_xex2(data, decoded_image)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xex", type=Path, help="XEX2 file to inspect read-only")
    parser.add_argument(
        "--decoded-image",
        type=Path,
        help="decoded in-memory image used only to classify bounded import records",
    )
    parser.add_argument("--pretty", action="store_true", help="indent JSON output")
    args = parser.parse_args(argv)

    try:
        result = inspect_file(args.xex, args.decoded_image)
    except (OSError, XexFormatError) as error:
        print(f"xex_info: {error}", file=sys.stderr)
        return 2

    json.dump(result, sys.stdout, indent=2 if args.pretty else None)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
