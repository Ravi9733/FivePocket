"""Validate a private title-content snapshot (not a mount/license grant)."""
import re
import struct

RECORD_BYTES = 308
MAX_BYTES = 16 + 16 * RECORD_BYTES


def parse_catalog(data: bytes) -> tuple[int, int]:
    if not 16 <= len(data) <= MAX_BYTES or data[:8] != b'GXCONT1\0':
        raise ValueError('content catalog header rejected')
    title, count = struct.unpack_from('>II', data, 8)
    if not title or not 1 <= count <= 16 or len(data) != 16 + count * RECORD_BYTES:
        raise ValueError('content catalog size/count rejected')
    seen = set()
    for i in range(count):
        record = data[16 + i * RECORD_BYTES:16 + (i + 1) * RECORD_BYTES]
        if struct.unpack_from('>II', record) != (1, 2):
            raise ValueError('content catalog source/type rejected')
        display = record[8:264].decode('utf-16-be')
        if '\0' in display and any(c != '\0' for c in display[display.index('\0'):]):
            raise ValueError('content display padding rejected')
        name = record[264:].rstrip(b'\0')
        if not re.fullmatch(rb'[A-Za-z0-9]{1,40}', name) or name.lower() in seen:
            raise ValueError('content identity rejected')
        seen.add(name.lower())
    return title, count
