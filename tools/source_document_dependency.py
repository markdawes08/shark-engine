"""Populate only the reviewed source-JSON archive for EngineTools; reject drift."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def populate(build: Path, archive: Path) -> dict:
    locked = json.loads((ROOT / "dependencies.lock.json").read_text(encoding="utf-8"))["dependencies"]["JSON"]
    if (locked["license"] != "MIT" or locked["license_files"] != ["LICENSE.MIT"]
            or locked["patches"] or locked["transitive_dependencies"]):
        raise ValueError("JSON dependency metadata requires a new integration review")
    if not archive.is_file() or archive.is_symlink() or archive.stat().st_size > 1024 * 1024:
        raise ValueError("Missing, linked or oversized JSON archive")
    data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != locked["sha256"]:
        raise ValueError("JSON archive SHA-256 differs from the dependency lock")
    files = {}
    total = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as stream:
        for member in stream:
            path = PurePosixPath(member.name)
            if (path.is_absolute() or ".." in path.parts or "\\" in member.name
                    or not path.parts or path.parts[0] != "json"
                    or not (member.isfile() or member.isdir())):
                raise ValueError("Unsafe JSON archive member")
            if member.isdir():
                continue
            relative = path.relative_to("json").as_posix()
            total += member.size
            if relative in files or len(files) >= 128 or total > 4 * 1024 * 1024:
                raise ValueError("Duplicate or oversized JSON archive contents")
            files[relative] = stream.extractfile(member).read()
    if not {"LICENSE.MIT", "include/nlohmann/json.hpp"}.issubset(files):
        raise ValueError("Incomplete JSON source archive")
    dependencies = build / "_deps"
    populated = dependencies / "json-src"
    if dependencies.is_symlink() or populated.is_symlink():
        raise ValueError("Linked JSON source directory")
    dependencies.mkdir(parents=True, exist_ok=True)
    if not populated.exists():
        with tempfile.TemporaryDirectory(prefix=".json-", dir=dependencies) as temporary:
            candidate = Path(temporary) / "source"
            candidate.mkdir()
            for relative, content in files.items():
                path = candidate / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
            os.replace(candidate, populated)
    if not populated.is_dir():
        raise ValueError("JSON source must be a directory")
    actual = {}
    for path in populated.rglob("*"):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError("Linked or nonregular JSON source input")
        if path.is_file():
            relative = path.relative_to(populated).as_posix()
            if (relative not in files or path.stat().st_size != len(files[relative])
                    or path.read_bytes() != files[relative]):
                raise ValueError("Populated JSON source differs from the locked archive: " + relative)
            actual[relative] = hashlib.sha256(files[relative]).hexdigest()
    if set(actual) != set(files):
        raise ValueError("Populated JSON source inventory differs from the locked archive")
    return {"version": locked["version"], "commit": locked["commit"],
            "archive_sha256": locked["sha256"], "source_files": actual,
            "license_sha256": actual["LICENSE.MIT"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    arguments = parser.parse_args()
    print(json.dumps(populate(arguments.build, arguments.archive), indent=2))
