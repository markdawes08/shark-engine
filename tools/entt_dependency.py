"""Verify and prepare the single locked EnTT source archive; no package resolution."""

from __future__ import annotations

import hashlib
import argparse
import io
import json
from pathlib import Path, PurePosixPath
import tarfile
import urllib.request


MAX_ARCHIVE = 8 * 1024 * 1024
MAX_CONTENT = 32 * 1024 * 1024


def entry(source: Path) -> dict:
    result = json.loads((source / "dependencies.lock.json").read_text(encoding="utf-8"))["dependencies"]["EnTT"]
    if (result.get("license") != "MIT" or result.get("license_files") != ["LICENSE"]
            or result.get("transitive_dependencies") != [] or result.get("patches") != []):
        raise ValueError("EnTT dependency metadata requires a new integration review")
    return result


def archive_files(archive: Path, locked: dict) -> dict[str, bytes]:
    if (not archive.is_file() or archive.is_symlink() or archive.stat().st_size > MAX_ARCHIVE):
        raise ValueError("Missing, linked or oversized EnTT archive")
    data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != locked["sha256"]:
        raise ValueError("EnTT archive SHA-256 differs from the dependency lock")
    files = {}
    total = 0
    prefix = "entt-" + locked["commit"]
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as stream:
        for member in stream:
            path = PurePosixPath(member.name)
            if (path.is_absolute() or ".." in path.parts or "\\" in member.name
                    or not path.parts or path.parts[0] != prefix
                    or not (member.isfile() or member.isdir())):
                raise ValueError("Unsafe EnTT archive member")
            if member.isdir():
                continue
            relative = path.relative_to(prefix).as_posix()
            total += member.size
            if relative in files or len(files) >= 4096 or total > MAX_CONTENT:
                raise ValueError("Duplicate or oversized EnTT archive contents")
            files[relative] = stream.extractfile(member).read()
    if "LICENSE" not in files or "src/entt/entity/registry.hpp" not in files:
        raise ValueError("Incomplete EnTT source archive")
    return files


def prepare(source: Path, destination: Path) -> Path:
    """Explicit preparation before a disconnected configure, outside its graph."""
    locked = entry(source)
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / (locked["commit"] + ".tar.gz")
    if archive.exists():
        archive_files(archive, locked)
        return archive
    with urllib.request.urlopen(locked["url"], timeout=60) as response:
        data = response.read(MAX_ARCHIVE + 1)
    if len(data) > MAX_ARCHIVE or hashlib.sha256(data).hexdigest() != locked["sha256"]:
        raise ValueError("Downloaded EnTT archive differs from the lock or exceeds its limit")
    # Exclusive creation preserves any concurrently provided cache file.
    with archive.open("xb") as stream:
        stream.write(data)
    archive_files(archive, locked)
    return archive


def verify_build(source: Path, build: Path, archive: Path) -> dict:
    """Prove the sole dependency tree is exactly the reviewed header input."""
    locked = entry(source)
    files = archive_files(archive, locked)
    dependencies = build / "_deps"
    if (not dependencies.is_dir() or dependencies.is_symlink()
            or {item.name for item in dependencies.iterdir()} != {"entt-src", "entt-build", "entt-subbuild"}
            or any(item.is_symlink() or not item.is_dir() for item in dependencies.iterdir())):
        raise ValueError("Server-only dependency tree must contain exactly the locked EnTT population")
    populated = dependencies / "entt-src"
    actual = {}
    for path in populated.rglob("*"):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError("Linked or nonregular EnTT source input")
        if path.is_file():
            relative = path.relative_to(populated).as_posix()
            if relative not in files or path.read_bytes() != files[relative]:
                raise ValueError("Populated EnTT source differs from the locked archive: " + relative)
            actual[relative] = hashlib.sha256(files[relative]).hexdigest()
    if set(actual) != set(files):
        raise ValueError("Populated EnTT source inventory differs from the locked archive")
    return {"version": locked["version"], "commit": locked["commit"],
            "archive": str(archive), "archive_sha256": locked["sha256"],
            "source_files": actual, "license_sha256": hashlib.sha256(files["LICENSE"]).hexdigest()}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    try:
        result = prepare(Path(__file__).resolve().parents[1], options.output.absolute())
        print(json.dumps({"status": "PASS", "archive": str(result),
                          "sha256": hashlib.sha256(result.read_bytes()).hexdigest()}))
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}))
        raise SystemExit(1)
