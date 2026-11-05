#!/usr/bin/env python3
"""eof_blank.py IN.cpp OUT.cpp: drop the generator's trailing empty line (design gap G4, corpus rewrite signature).

XenonRecomp ends every chunk with `}` + newline + one empty line; every chunk the validated corpus repaired in place was
written back without that empty line (the files end with `}` + newline). The pass removes exactly one trailing empty line
and refuses a text that does not end with `}\\n\\n`. No parameters, no game data.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import textpass  # noqa: E402


def apply(text: str) -> tuple[str, dict]:
    if not text.endswith('}\n\n') or text.endswith('\n\n\n'):
        raise SystemExit('text does not end with exactly one empty line after a closing brace')
    return text[:-1], {'removed_empty_lines': 1}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    text, stats = apply(textpass.read_text(Path(sys.argv[1])))
    textpass.write_new(Path(sys.argv[2]), text)
    print(f'{Path(sys.argv[1]).name}: {stats}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
