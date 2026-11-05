from pathlib import Path
import struct
import sys
import unittest


TOOLS_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS_ROOT / "xex_info"))

from xex_info import XexFormatError, parse_xex2  # noqa: E402


IMAGE_BASE = 0x82000000
IMPORT_OFFSET = 0x240
DATA_IMPORT_SLOT = IMAGE_BASE + 0x100
FUNCTION_DESCRIPTOR_SLOT = IMAGE_BASE + 0x200
FUNCTION_THUNK_SLOT = IMAGE_BASE + 0x210
TLS_OFFSET = 0x2A0
TLS_RAW_DATA_ADDRESS = IMAGE_BASE + 0x300


def synthetic_xex() -> bytes:
    data = bytearray(0x340)
    security_offset = 0x50
    execution_offset = 0x200
    format_offset = 0x220
    optional_headers = (
        (0x00010100, 0x83639888),
        (0x00010201, IMAGE_BASE),
        (0x00040006, execution_offset),
        (0x000003FF, format_offset),
        (0x000103FF, IMPORT_OFFSET),
        (0x00020104, TLS_OFFSET),
    )
    struct.pack_into(">4sIIIII", data, 0, b"XEX2", 0, len(data), 0, security_offset, len(optional_headers))
    for index, header in enumerate(optional_headers):
        struct.pack_into(">II", data, 24 + index * 8, *header)

    struct.pack_into(">I", data, security_offset + 0x04, 0x400)
    struct.pack_into(">I", data, security_offset + 0x10C, 0x00000001)
    struct.pack_into(">I", data, security_offset + 0x110, IMAGE_BASE)
    struct.pack_into(">I", data, security_offset + 0x178, 0x00FF00FF)
    struct.pack_into(">I", data, security_offset + 0x17C, 0x00000001)
    struct.pack_into(">I", data, security_offset + 0x180, 123)

    struct.pack_into(">IIII4BI", data, execution_offset,
                     0x11223344, 0x01234567, 0x01230000, 0x545408A7,
                     2, 0, 2, 2, 0x545408A7)
    struct.pack_into(">IHH", data, format_offset, 8, 1, 1)

    string_table = b"xboxkrnl.exe\0\0\0\0"
    library_size = 40 + 3 * 4
    import_size = 12 + len(string_table) + library_size
    struct.pack_into(">III", data, IMPORT_OFFSET, import_size, len(string_table), 1)
    data[IMPORT_OFFSET + 12:IMPORT_OFFSET + 12 + len(string_table)] = string_table
    library_offset = IMPORT_OFFSET + 12 + len(string_table)
    struct.pack_into(
        ">I20sIIIHH",
        data,
        library_offset,
        library_size,
        bytes(20),
        0x11223344,
        0x02000000,
        0x01000000,
        0,
        3,
    )
    struct.pack_into(
        ">III",
        data,
        library_offset + 40,
        DATA_IMPORT_SLOT,
        FUNCTION_DESCRIPTOR_SLOT,
        FUNCTION_THUNK_SLOT,
    )
    struct.pack_into(
        ">IIII",
        data,
        TLS_OFFSET,
        64,
        TLS_RAW_DATA_ADDRESS,
        0x20,
        0x10,
    )
    return bytes(data)


def synthetic_decoded_image() -> bytes:
    image = bytearray(0x400)
    struct.pack_into(">I", image, DATA_IMPORT_SLOT - IMAGE_BASE, 0x00000193)
    struct.pack_into(">I", image, FUNCTION_DESCRIPTOR_SLOT - IMAGE_BASE, 0x00000194)
    struct.pack_into(">I", image, FUNCTION_THUNK_SLOT - IMAGE_BASE, 0x01000194)
    return bytes(image)


def synthetic_postprocessed_image() -> bytes:
    image = bytearray(0x400)
    struct.pack_into("<I", image, DATA_IMPORT_SLOT - IMAGE_BASE, 0x00000193)
    struct.pack_into("<I", image, FUNCTION_DESCRIPTOR_SLOT - IMAGE_BASE, 0x00000194)
    struct.pack_into(
        "<IIII",
        image,
        FUNCTION_THUNK_SLOT - IMAGE_BASE,
        0x00000060,
        0x00000060,
        0x00000060,
        0x2000804E,
    )
    return bytes(image)


class XexInfoTests(unittest.TestCase):
    def test_parses_bounded_metadata(self) -> None:
        result = parse_xex2(synthetic_xex())
        self.assertEqual(result["entry_point"], "0x83639888")
        self.assertEqual(result["image_base_address"], "0x82000000")
        self.assertEqual(result["execution_info"]["title_id"], "545408A7")
        self.assertEqual(result["execution_info"]["disc_number"], 2)
        self.assertEqual(result["execution_info"]["version"], "0.1.9029.103")
        self.assertEqual(result["file_format"]["encryption"], "normal")
        self.assertEqual(result["file_format"]["compression"], "basic")
        self.assertFalse(result["import_libraries"]["decoded_records"])
        self.assertEqual(result["import_libraries"]["library_count"], 1)
        self.assertEqual(result["import_libraries"]["record_count"], 3)
        self.assertEqual(
            result["tls_info"],
            {
                "slot_count": 64,
                "raw_data_address": "0x82000300",
                "data_size": 0x20,
                "raw_data_size": 0x10,
            },
        )

    def test_classifies_data_and_function_imports(self) -> None:
        result = parse_xex2(synthetic_xex(), synthetic_decoded_image())
        imports = result["import_libraries"]
        self.assertTrue(imports["decoded_records"])
        self.assertEqual(imports["import_count"], 2)
        self.assertEqual(imports["function_import_count"], 1)
        self.assertEqual(imports["data_import_count"], 1)
        self.assertEqual(
            imports["data_imports"],
            [{
                "library": "xboxkrnl.exe",
                "ordinal": 0x193,
                "slot_address": f"0x{DATA_IMPORT_SLOT:08X}",
            }],
        )
        library = imports["libraries"][0]
        self.assertEqual(library["name"], "xboxkrnl.exe")
        self.assertEqual(library["record_count"], 3)
        self.assertEqual(library["import_count"], 2)
        self.assertEqual(library["function_import_count"], 1)
        self.assertEqual(library["data_import_count"], 1)

    def test_classifies_xenonrecomp_postprocessed_imports(self) -> None:
        result = parse_xex2(synthetic_xex(), synthetic_postprocessed_image())
        imports = result["import_libraries"]
        self.assertEqual(imports["import_count"], 2)
        self.assertEqual(imports["function_import_count"], 1)
        self.assertEqual(imports["data_import_count"], 1)
        self.assertEqual(imports["data_imports"][0]["ordinal"], 0x193)

    def test_rejects_out_of_range_import_record(self) -> None:
        data = bytearray(synthetic_xex())
        library_offset = IMPORT_OFFSET + 12 + 16
        struct.pack_into(">I", data, library_offset + 40, IMAGE_BASE + 0x400)
        with self.assertRaisesRegex(XexFormatError, "import record"):
            parse_xex2(bytes(data), synthetic_decoded_image())

    def test_rejects_decoded_image_larger_than_security_image(self) -> None:
        with self.assertRaisesRegex(XexFormatError, "exceeds XEX security"):
            parse_xex2(synthetic_xex(), synthetic_decoded_image() + b"\0")

    def test_rejects_orphan_function_thunk(self) -> None:
        image = bytearray(synthetic_decoded_image())
        struct.pack_into(">I", image, DATA_IMPORT_SLOT - IMAGE_BASE, 0x01000193)
        with self.assertRaisesRegex(XexFormatError, "no preceding descriptor"):
            parse_xex2(synthetic_xex(), bytes(image))

    def test_rejects_import_table_beyond_declared_header(self) -> None:
        data = bytearray(synthetic_xex())
        struct.pack_into(">I", data, IMPORT_OFFSET, 0x200)
        with self.assertRaisesRegex(XexFormatError, "declared XEX header"):
            parse_xex2(bytes(data))

    def test_validates_tls_template_inside_decoded_image(self) -> None:
        result = parse_xex2(synthetic_xex(), synthetic_decoded_image())
        self.assertEqual(result["tls_info"]["raw_data_size"], 0x10)

    def test_rejects_tls_record_beyond_declared_header(self) -> None:
        data = bytearray(synthetic_xex())
        struct.pack_into(">I", data, 8, TLS_OFFSET + 12)
        with self.assertRaisesRegex(XexFormatError, "declared XEX header"):
            parse_xex2(bytes(data))

    def test_rejects_duplicate_optional_header_key(self) -> None:
        data = bytearray(synthetic_xex())
        header_count = struct.unpack_from(">I", data, 20)[0]
        struct.pack_into(">I", data, 20, header_count + 1)
        struct.pack_into(">II", data, 24 + header_count * 8, 0x00020104, TLS_OFFSET)
        with self.assertRaisesRegex(XexFormatError, "duplicate optional-header key"):
            parse_xex2(bytes(data))

    def test_rejects_tls_raw_data_larger_than_static_data(self) -> None:
        data = bytearray(synthetic_xex())
        struct.pack_into(">II", data, TLS_OFFSET + 8, 0x20, 0x24)
        with self.assertRaisesRegex(XexFormatError, "raw-data size exceeds"):
            parse_xex2(bytes(data))

    def test_rejects_unaligned_tls_static_data_size(self) -> None:
        data = bytearray(synthetic_xex())
        struct.pack_into(">II", data, TLS_OFFSET + 8, 0x22, 0x10)
        with self.assertRaisesRegex(XexFormatError, "not 4-byte aligned"):
            parse_xex2(bytes(data))

    def test_rejects_tls_slot_and_static_data_bounds(self) -> None:
        data = bytearray(synthetic_xex())
        struct.pack_into(">I", data, TLS_OFFSET, 2049)
        with self.assertRaisesRegex(XexFormatError, "slot count"):
            parse_xex2(bytes(data))

        data = bytearray(synthetic_xex())
        struct.pack_into(">I", data, TLS_OFFSET + 8, 1024 * 1024 + 4)
        with self.assertRaisesRegex(XexFormatError, "static data"):
            parse_xex2(bytes(data))

    def test_rejects_tls_template_outside_decoded_image(self) -> None:
        data = bytearray(synthetic_xex())
        struct.pack_into(">I", data, TLS_OFFSET + 4, IMAGE_BASE + 0x3F8)
        with self.assertRaisesRegex(XexFormatError, "exceeds the decoded image"):
            parse_xex2(bytes(data), synthetic_decoded_image())

    def test_rejects_non_xex_input(self) -> None:
        with self.assertRaisesRegex(XexFormatError, "not an XEX2"):
            parse_xex2(b"NOPE" + bytes(300))

    def test_rejects_out_of_bounds_optional_table(self) -> None:
        data = bytearray(32)
        struct.pack_into(">4sIIIII", data, 0, b"XEX2", 0, 24, 0, 24, 100)
        with self.assertRaisesRegex(XexFormatError, "optional-header table"):
            parse_xex2(bytes(data))


if __name__ == "__main__":
    unittest.main()
