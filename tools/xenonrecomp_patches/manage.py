#!/usr/bin/env python3
"""Check, apply, or reverse the root-owned XenonRecomp patch queue."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Sequence


ROOT = Path(__file__).resolve().parents[2]
SUBMODULE = ROOT / "tools" / "XenonRecomp"
LOCK = ROOT / "tools" / "host_tools.lock.json"
PATCH_DIRECTORY = ROOT / "patches" / "xenonrecomp"


def pinned_revision() -> str:
    data = json.loads(LOCK.read_text(encoding="utf-8"))
    for tool in data["tools"]:
        if tool["name"] == "XenonRecomp":
            return str(tool["revision"])
    raise RuntimeError("XenonRecomp is absent from host_tools.lock.json")


def current_revision() -> str:
    return subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=SUBMODULE,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout.strip()


def patches() -> list[Path]:
    result = sorted(PATCH_DIRECTORY.glob("*.patch"))
    if not result:
        raise RuntimeError(f"no patches found in {PATCH_DIRECTORY}")
    return result


def git_apply(arguments: Sequence[str], patch: Path) -> None:
    command = ["git", "apply", *arguments]
    if patch.name == "0003-bound-pe-section-names.patch" and "--reverse" in arguments:
        # Reversing 0003 restores an upstream line whose trailing space is
        # already present at the pinned revision. Do not warn about that known
        # removed-line whitespace; retain normal diagnostics for every other
        # patch and direction.
        command.append("--whitespace=nowarn")
    command.append(str(patch))
    subprocess.run(
        command,
        cwd=SUBMODULE,
        check=True,
    )


def ensure_pinned() -> None:
    expected = pinned_revision()
    actual = current_revision()
    if actual != expected:
        raise RuntimeError(
            f"XenonRecomp revision mismatch: expected {expected}, found {actual}"
        )


def check_queue(queue: Sequence[Path]) -> None:
    """Apply the ordered queue to a temporary index without touching files."""

    with tempfile.TemporaryDirectory(prefix="xenonrecomp-patch-check-") as temporary:
        environment = os.environ.copy()
        environment["GIT_INDEX_FILE"] = str(Path(temporary) / "index")
        subprocess.run(
            ["git", "read-tree", "HEAD"],
            cwd=SUBMODULE,
            env=environment,
            check=True,
        )
        for patch in queue:
            subprocess.run(
                ["git", "apply", "--cached", str(patch)],
                cwd=SUBMODULE,
                env=environment,
                check=True,
            )
            print(f"check: {patch.relative_to(ROOT)}")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("check", "apply", "reverse"))
    args = parser.parse_args(argv)

    ensure_pinned()
    queue = patches()
    if args.action == "check":
        check_queue(queue)
        return 0
    if args.action == "reverse":
        queue.reverse()
        apply_arguments = ("--reverse",)
    else:
        apply_arguments = ()

    for patch in queue:
        git_apply(apply_arguments, patch)
        print(f"{args.action}: {patch.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
