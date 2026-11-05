#!/usr/bin/env python3
"""Prepare a private exhaustive fixture for a verified constant-return leaf switch."""
from pathlib import Path
import argparse
import os
import subprocess

from switch_tables import MAX_IMAGE_BYTES, verify_constant_leaf_switch


def extract_function(text: str, address: int, *, allow_indirect: bool = False) -> str:
    marker = f"PPC_FUNC_IMPL(__imp__sub_{address:X}) {{"
    if text.count(marker) != 1:
        raise ValueError("generated leaf definition is missing or ambiguous")
    start = text.index(marker)
    opening = start + len(marker) - 1
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{": depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                function = text[start:index + 1]
                if ("PPC_CALL_INDIRECT_FUNC" in function and not allow_indirect) or "\n#" in function:
                    raise ValueError("leaf still contains unresolved dispatch or preprocessor text")
                return function
    raise ValueError("unterminated generated leaf")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("generated_source", type=Path)
    parser.add_argument("--image-base", type=lambda v: int(v, 0), required=True)
    parser.add_argument("--function-base", type=lambda v: int(v, 0), required=True)
    parser.add_argument("--function-size", type=lambda v: int(v, 0), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    for path in (args.image, args.generated_source, args.output):
        lexical = path.absolute()
        if lexical != lexical.resolve() or lexical.is_symlink() or not lexical.is_relative_to(root / "local-analysis") \
                or subprocess.run(["git", "check-ignore", "--quiet", "--", str(lexical)], cwd=root).returncode:
            raise ValueError("leaf fixture inputs/output must be ignored private paths without links")
    if not 0 < args.image.stat().st_size <= MAX_IMAGE_BYTES or not 0 < args.generated_source.stat().st_size <= 8 * 1024 * 1024:
        raise ValueError("leaf fixture source exceeds the size bound")
    verified = verify_constant_leaf_switch(args.image.read_bytes(), args.image_base, args.function_base, args.function_size)
    function = extract_function(args.generated_source.read_text(), args.function_base)
    text = '#include "ppc_context.h"\n#include "guest_address_space.h"\n#include <array>\n#include <cassert>\n'
    text += function + "\nint main() {\n"
    text += "recomp::bootstrap::GuestAddressSpace space; assert(space.reserve()); auto* base = space.base();\n"
    text += "const std::array<int64_t, 256> expected{{" + ",".join(map(str, verified["returns"])) + "}};\n"
    pointer = 0x1000 + verified["pointer_offset"]
    text += f"base[{pointer}] = 0; base[{pointer + 1}] = 0; base[{pointer + 2}] = 0x20; base[{pointer + 3}] = 0;\n"
    text += "for (unsigned selector = 0; selector < 256; ++selector) {\nPPCContext ctx{}; ctx.r3.u64 = 0x1000; ctx.r1.u64 = 0x8800; ctx.lr = 0x12345678ABCDEF00ULL; ctx.r24.u64 = 0xFEDCBA9876543210ULL;\n"
    text += f"base[{0x2000 + verified['selector_offset']}] = selector; __imp__sub_{args.function_base:X}(ctx, base);\n"
    text += "assert(ctx.r3.s64 == expected[selector]); assert(ctx.r1.u64 == 0x8800); assert(ctx.lr == 0x12345678ABCDEF00ULL); assert(ctx.r24.u64 == 0xFEDCBA9876543210ULL);\n}\n}\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "w", encoding='utf-8', newline='\n') as stream:
        stream.write(text)
    print("Prepared private constant-leaf fixture: 256 selector cases and stack/link/nonvolatile checks")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError):
        raise SystemExit("leaf fixture validation failed; private diagnostics suppressed")
