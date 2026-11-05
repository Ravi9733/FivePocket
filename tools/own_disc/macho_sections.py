#!/usr/bin/env python3
"""The code identity of a 64-bit Mach-O file (an object or an executable): what it runs, without its debug information.

    macho_sections.py FILE [FILE ...]      prints each file's identity and section count

code_identity(path) is the SHA-256 over every section outside the __DWARF segment, in file order: segment and section
name, size, flags, alignment, the section's bytes (none for zero-fill sections) and its relocation entries. Left out:
the __DWARF sections (they hold the build's working directory and the spelling of every include path) and everything
outside sections (load commands, the symbol and string tables: in an executable these hold the debug map, the object
files' paths). So an object compiled in another folder, or an executable linked from objects at other paths, has the
same identity when its code and data are the same. elf_sections.py does the same for the Android library.

executable_identity(path) is the same for a linked executable, except that the contents of the linker's own
trampolines (__TEXT,__stubs, __auth_stubs, __objc_stubs) count by size only: Xcode's linker builds stubs and GOTs in
parallel (ld-1267, parallelBuildStubsAndGOTs) and numbers the stubs' slots in a different order from one link of the same
objects to the next (measured 2026-11-05: two links of identical objects differed in those two sections only).
"""
from __future__ import annotations

import hashlib
import struct
import sys
from pathlib import Path

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
ZEROFILL_TYPES = {0x1, 0xC, 0x12}           # S_ZEROFILL, S_GB_ZEROFILL, S_THREAD_LOCAL_ZEROFILL


def sections(data: bytes):
    """(segname, sectname, size, offset, align, reloff, nreloc, flags) of every section, in file order"""
    magic, _cpu, _sub, _type, ncmds, _size, _flags, _res = struct.unpack_from('<IiiIIIII', data, 0)
    if magic != MH_MAGIC_64:
        raise ValueError('not a thin 64-bit Mach-O file')
    at = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<II', data, at)
        if cmd == LC_SEGMENT_64:
            nsects = struct.unpack_from('<I', data, at + 64)[0]
            s = at + 72
            for _ in range(nsects):
                sect, seg = data[s:s + 16], data[s + 16:s + 32]
                _addr, size, offset, align, reloff, nreloc, flags = struct.unpack_from('<QQIIIII', data, s + 32)
                yield (seg.rstrip(b'\0').decode(), sect.rstrip(b'\0').decode(), size, offset, align, reloff, nreloc, flags)
                s += 80
        at += cmdsize


LINKER_STUBS = {('__TEXT', '__stubs'), ('__TEXT', '__auth_stubs'), ('__TEXT', '__objc_stubs')}


def executable_identity(path: Path) -> str:
    return code_identity(path, stubs_by_size=True)


def code_identity(path: Path, stubs_by_size: bool = False) -> str:
    data = Path(path).read_bytes()
    h = hashlib.sha256()
    for seg, sect, size, offset, align, reloff, nreloc, flags in sections(data):
        if seg == '__DWARF':
            continue
        h.update(f'{seg},{sect},{size},{flags},{align};'.encode())
        if stubs_by_size and (seg, sect) in LINKER_STUBS:
            continue
        if (flags & 0xFF) not in ZEROFILL_TYPES:
            h.update(data[offset:offset + size])
        h.update(data[reloff:reloff + 8 * nreloc])
    return h.hexdigest()


def main(argv: list[str]) -> int:
    for name in argv:
        data = Path(name).read_bytes()
        count = sum(1 for s in sections(data) if s[0] != '__DWARF')
        print(code_identity(Path(name)), count, name)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
