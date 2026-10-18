"""Prepare and verify the two reviewed Core header archives; no package resolution."""

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
DEPENDENCIES = {
    "EnTT": {"prefix": "entt", "license": "LICENSE", "header": "src/entt/entity/registry.hpp",
             "cache": "SHARK_EnTT_ARCHIVE"},
    "GLM": {"prefix": "glm", "license": "copying.txt", "header": "glm/glm.hpp",
            "cache": "SHARK_GLM_ARCHIVE_INPUT"},
}


def specification(name: str) -> dict:
    if name not in DEPENDENCIES:
        raise ValueError("Unreviewed Core header dependency: " + name)
    return DEPENDENCIES[name]


def entry(source: Path, name: str = "EnTT") -> dict:
    spec = specification(name)
    result = json.loads((source / "dependencies.lock.json").read_text(encoding="utf-8"))["dependencies"][name]
    if (result.get("license") != "MIT" or result.get("license_files") != [spec["license"]]
            or result.get("transitive_dependencies") != [] or result.get("patches") != []):
        raise ValueError(name + " dependency metadata requires a new integration review")
    return result


def archive_files(archive: Path, locked: dict, name: str = "EnTT") -> dict[str, bytes]:
    spec = specification(name)
    if (not archive.is_file() or archive.is_symlink() or archive.stat().st_size > MAX_ARCHIVE):
        raise ValueError("Missing, linked or oversized " + name + " archive")
    data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != locked["sha256"]:
        raise ValueError(name + " archive SHA-256 differs from the dependency lock")
    files = {}
    total = 0
    prefix = spec["prefix"] + "-" + locked["commit"]
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as stream:
        for member in stream:
            path = PurePosixPath(member.name)
            if (path.is_absolute() or ".." in path.parts or "\\" in member.name
                    or not path.parts or path.parts[0] != prefix
                    or not (member.isfile() or member.isdir())):
                raise ValueError("Unsafe " + name + " archive member")
            if member.isdir():
                continue
            relative = path.relative_to(prefix).as_posix()
            total += member.size
            if relative in files or len(files) >= 4096 or total > MAX_CONTENT:
                raise ValueError("Duplicate or oversized " + name + " archive contents")
            files[relative] = stream.extractfile(member).read()
    if spec["license"] not in files or spec["header"] not in files:
        raise ValueError("Incomplete " + name + " source archive")
    return files


def prepare(source: Path, destination: Path, name: str = "EnTT") -> Path:
    """Explicit preparation before a disconnected configure, outside its graph."""
    locked = entry(source, name)
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / (locked["commit"] + ".tar.gz")
    if archive.exists():
        archive_files(archive, locked, name)
        return archive
    with urllib.request.urlopen(locked["url"], timeout=60) as response:
        data = response.read(MAX_ARCHIVE + 1)
    if len(data) > MAX_ARCHIVE or hashlib.sha256(data).hexdigest() != locked["sha256"]:
        raise ValueError("Downloaded " + name + " archive differs from the lock or exceeds its limit")
    # Exclusive creation preserves any concurrently provided cache file.
    with archive.open("xb") as stream:
        stream.write(data)
    archive_files(archive, locked, name)
    return archive


def verify_sources(source: Path, build: Path, archive: Path, name: str = "EnTT") -> dict:
    """Compare the complete extracted source inventory with its verified archive."""
    spec = specification(name)
    locked = entry(source, name)
    files = archive_files(archive, locked, name)
    populated = build / "_deps" / (spec["prefix"] + "-src")
    if not populated.is_dir() or populated.is_symlink():
        raise ValueError("Missing or linked " + name + " source directory")
    actual = {}
    for path in populated.rglob("*"):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError("Linked or nonregular " + name + " source input")
        if path.is_file():
            relative = path.relative_to(populated).as_posix()
            if (relative not in files or path.stat().st_size != len(files[relative])
                    or path.read_bytes() != files[relative]):
                raise ValueError("Populated " + name + " source differs from the locked archive: " + relative)
            actual[relative] = hashlib.sha256(files[relative]).hexdigest()
    if set(actual) != set(files):
        raise ValueError("Populated " + name + " source inventory differs from the locked archive")
    return {"version": locked["version"], "commit": locked["commit"],
            "archive": str(archive), "archive_sha256": locked["sha256"],
            "source_files": actual, "license_file": spec["license"],
            "license_sha256": hashlib.sha256(files[spec["license"]]).hexdigest()}


def verify_build(source: Path, build: Path, archives: dict[str, Path]) -> dict:
    """The headless graph allows exactly EnTT and GLM, with no extra population."""
    if set(archives) != set(DEPENDENCIES):
        raise ValueError("Missing locked EnTT/GLM archive evidence")
    dependencies = build / "_deps"
    expected = {spec["prefix"] + suffix for spec in DEPENDENCIES.values()
                for suffix in ("-src", "-build", "-subbuild")}
    if (not dependencies.is_dir() or dependencies.is_symlink()
            or {item.name for item in dependencies.iterdir()} != expected
            or any(item.is_symlink() or not item.is_dir() for item in dependencies.iterdir())):
        raise ValueError("Server-only dependency tree must contain exactly the locked EnTT/GLM populations")
    return {name: verify_sources(source, build, archive, name) for name, archive in archives.items()}


def main(names=tuple(DEPENDENCIES)) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    try:
        source = Path(__file__).resolve().parents[1]
        archives = {name: prepare(source, options.output.absolute(), name) for name in names}
        print(json.dumps({"status": "PASS", "archives": {
            name: {"archive": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            for name, path in archives.items()}}))
        return 0
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
