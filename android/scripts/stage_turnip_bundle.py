#!/usr/bin/env python3
"""Stage assets/ship from an existing APK for a separate FivePocket package ID.

The input APK is read-only. Only the copied config and NUL-delimited absolute
paths in the copied start request are rewritten; their hashes are then checked.
"""
import argparse
import hashlib
import json
import re
import zipfile
from pathlib import Path

PACKAGE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+$")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def stage(apk: Path, output: Path, package: str) -> None:
    if not PACKAGE.fullmatch(package):
        raise ValueError("invalid Android package ID")
    if not apk.is_file():
        raise ValueError("input APK does not exist")
    if output.exists() and any(output.iterdir()):
        raise ValueError("output directory must be empty")
    output.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(apk) as archive:
        names = set(archive.namelist())
        config_name = "assets/ship/config.json"
        if config_name not in names:
            raise ValueError("APK has no assets/ship/config.json")
        config = json.loads(archive.read(config_name))
        old_root = config.get("filesRoot")
        if not isinstance(old_root, str) or not old_root.startswith("/data/user/0/") or not old_root.endswith("/files"):
            raise ValueError("unexpected config filesRoot")
        old_package = old_root[len("/data/user/0/"):-len("/files")]
        if not PACKAGE.fullmatch(old_package):
            raise ValueError("config filesRoot has invalid package ID")
        new_root = f"/data/user/0/{package}/files"

        request_asset = config["startRequest"]["asset"]
        request_name = "assets/" + request_asset
        if request_name not in names:
            raise ValueError("start request asset is missing")
        request = archive.read(request_name)
        rewritten = request.replace(old_root.encode(), new_root.encode())
        if rewritten == request or rewritten.count(new_root.encode()) < 2:
            raise ValueError("start request did not contain the expected package-specific paths")

        required = [config["library"], config["shaderPack"], config["startRequest"]]
        for piece in required + config.get("pieces", []):
            name = "assets/" + piece["asset"]
            if name not in names:
                raise ValueError("missing bundled asset: " + piece["asset"])
            data = archive.read(name)
            if len(data) != piece["size"] or digest(data) != piece["sha256"]:
                raise ValueError("original asset checksum mismatch: " + piece["asset"])

        config["filesRoot"] = new_root
        config["startRequest"]["size"] = len(rewritten)
        config["startRequest"]["sha256"] = digest(rewritten)
        config_bytes = (json.dumps(config, indent=2, ensure_ascii=False) + "\n").encode()

        for name in sorted(names):
            if not name.startswith("assets/ship/") or name.endswith("/"):
                continue
            relative = name[len("assets/"):]
            destination = output / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            if name == config_name:
                data = config_bytes
            elif name == request_name:
                data = rewritten
            else:
                data = archive.read(name)
            destination.write_bytes(data)

    staged_config = json.loads((output / "ship/config.json").read_text())
    staged_request = (output / staged_config["startRequest"]["asset"]).read_bytes()
    if staged_config["filesRoot"] != new_root:
        raise ValueError("staged filesRoot mismatch")
    if len(staged_request) != staged_config["startRequest"]["size"] or digest(staged_request) != staged_config["startRequest"]["sha256"]:
        raise ValueError("staged start request checksum mismatch")
    print(f"Staged ship bundle for {package}: {len(list((output / 'ship').rglob('*')))} paths")
    print(f"filesRoot={new_root}")
    print(f"startRequest bytes={len(staged_request)} sha256={digest(staged_request)}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--package", required=True)
    args = parser.parse_args()
    stage(args.apk, args.output, args.package)


if __name__ == "__main__":
    main()
