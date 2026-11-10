#!/usr/bin/env python3
"""Verify native Android sources are implemented and tracked in this checkout.

These files are maintained in the repository now. Do not fetch an unrelated
release archive: the upstream v1.1.0-alpha.1 source archive does not contain
the files referenced by the current CMake project.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RELATIVE = Path("app/src/main/cpp")
NAMES = (
    "smoke_jni.cpp",
    "smoke_runtime.cpp",
    "smoke_runtime.h",
    "vulkan_smoke.cpp",
    "vulkan_smoke.h",
    "shader_constant_upload.cpp",
    "shader_constant_upload.h",
)

def main() -> None:
    missing = []
    for name in NAMES:
        path = ROOT / RELATIVE / name
        if not path.is_file() or path.stat().st_size == 0:
            missing.append(name)
    if missing:
        raise SystemExit("missing implemented native sources: " + ", ".join(missing))
    print("Native Android sources are present in the checked-out repository.")

if __name__ == "__main__":
    main()
