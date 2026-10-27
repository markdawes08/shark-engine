"""Populate the exact reviewed stb_image archive privately for EngineTools."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import tarfile
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
MAX_ARCHIVE = 2 * 1024 * 1024
MAX_CONTENT = 8 * 1024 * 1024
MAX_FILES = 512
MAX_MEMBERS = 1024
OPTIONS = {
    "upstream_cmake": "not executed", "SYSTEM": "ON",
    "implementation_definitions": {
        "STB_IMAGE_IMPLEMENTATION": "1", "STBI_ONLY_PNG": "1", "STBI_ONLY_JPEG": "1",
        "STBI_NO_STDIO": "1", "STBI_NO_HDR": "1", "STBI_NO_LINEAR": "1", "STBI_NO_SIMD": "1",
        "STBI_MAX_DIMENSIONS": "4096"},
    "allocator_hooks": ["STBI_MALLOC", "STBI_REALLOC_SIZED", "STBI_FREE"],
    "png_bits_per_channel": [1, 2, 4, 8, 16], "png_interlace": "none or Adam7",
    "jpeg_precision": 8, "preserve_png_16bit": "RGBA16",
}


def safe_relative(name: str) -> bool:
    if not isinstance(name, str) or not name or "\\" in name or ":" in name:
        return False
    path = PurePosixPath(name)
    return not path.is_absolute() and ".." not in path.parts and path.as_posix() == name and name != "."


def specification() -> dict:
    locked = json.loads((ROOT / "dependencies.lock.json").read_text(encoding="utf-8"))["dependencies"]["stb_image"]
    if (locked.get("license") != "MIT" or locked.get("license_files") != ["LICENSE", "stb_image.h"]
            or locked.get("options") != OPTIONS
            or locked.get("patches") != [] or locked.get("transitive_dependencies") != []
            or not re.fullmatch(r"[0-9a-f]{40}", locked.get("commit", ""))
            or not re.fullmatch(r"[0-9a-f]{64}", locked.get("sha256", ""))
            or locked.get("archive_prefix") != "stb-" + locked["commit"]
            or locked.get("url") != "https://codeload.github.com/nothings/stb/tar.gz/" + locked["commit"]
            or type(locked.get("archive_bytes")) is not int
            or not 0 < locked.get("archive_bytes", 0) <= MAX_ARCHIVE
            or not isinstance(locked.get("source_sha256"), dict)
            or not {"stb_image.h", "LICENSE"} <= set(locked.get("source_sha256", {}))
            or not 2 <= len(locked.get("source_sha256", {})) <= MAX_FILES
            or any(not safe_relative(name) for name in locked.get("source_sha256", {}))
            or any(not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value)
                   for value in locked.get("source_sha256", {}).values())):
        raise ValueError("stb_image dependency metadata requires a new integration review")
    return locked


def linked(path: Path) -> bool:
    return path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction())


def archive_files(archive: Path, locked: dict) -> dict[str, bytes]:
    if (not archive.is_file() or linked(archive)
            or archive.stat().st_size > MAX_ARCHIVE
            or archive.stat().st_size != locked["archive_bytes"]):
        raise ValueError("Missing, linked, oversized or size-mismatched stb_image archive")
    data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != locked["sha256"]:
        raise ValueError("stb_image archive SHA-256 differs from the dependency lock")
    files = {}
    total = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as stream:
        for index, member in enumerate(stream):
            if index >= MAX_MEMBERS:
                raise ValueError("Oversized stb_image archive member inventory")
            path = PurePosixPath(member.name)
            if (path.is_absolute() or ".." in path.parts or "\\" in member.name
                    or not path.parts or path.parts[0] != locked["archive_prefix"]
                    or not (member.isfile() or member.isdir())):
                raise ValueError("Unsafe stb_image archive member")
            if member.isdir():
                continue
            relative = path.relative_to(locked["archive_prefix"]).as_posix()
            total += member.size
            if (relative in files or len(files) >= MAX_FILES or member.size < 0
                    or total > MAX_CONTENT):
                raise ValueError("Duplicate or oversized stb_image archive contents")
            if ":" in relative or not relative or relative == ".":
                raise ValueError("Unsafe stb_image archive member")
            source = stream.extractfile(member)
            if source is None:
                raise ValueError("Missing stb_image archive member data")
            content = source.read(member.size + 1)
            if len(content) != member.size:
                raise ValueError("Truncated stb_image archive member")
            files[relative] = content
    if set(files) != set(locked["source_sha256"]):
        raise ValueError("stb_image archive inventory differs from the dependency lock")
    for relative, expected in locked["source_sha256"].items():
        if relative not in files or hashlib.sha256(files[relative]).hexdigest() != expected:
            raise ValueError("stb_image reviewed source hash differs from the dependency lock: " + relative)
    return files


def download_archive(archive: Path) -> None:
    """Fetch at most the pinned byte count, then verify before cache publication."""
    locked = specification()
    if archive.exists() or linked(archive):
        archive_files(archive, locked)
        return
    with urllib.request.urlopen(locked["url"], timeout=60) as response:
        data = response.read(locked["archive_bytes"] + 1)
    if len(data) != locked["archive_bytes"] or hashlib.sha256(data).hexdigest() != locked["sha256"]:
        raise ValueError("Downloaded stb_image archive differs from the locked size or SHA-256")
    if linked(archive.parent):
        raise ValueError("Linked stb_image archive cache directory")
    archive.parent.mkdir(parents=True, exist_ok=True)
    candidate = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".stb_image-", dir=archive.parent, delete=False) as stream:
            candidate = Path(stream.name)
            stream.write(data)
        archive_files(candidate, locked)
        if archive.exists() or linked(archive):
            archive_files(archive, locked)
        else:
            os.replace(candidate, archive)
    finally:
        if candidate is not None:
            candidate.unlink(missing_ok=True)


def populate(build: Path, archive: Path) -> dict:
    locked = specification()
    files = archive_files(archive, locked)
    dependencies = build / "_deps"
    populated = dependencies / "stb_image-src"
    if linked(dependencies) or linked(populated):
        raise ValueError("Linked stb_image source directory")
    dependencies.mkdir(parents=True, exist_ok=True)
    if not populated.exists():
        with tempfile.TemporaryDirectory(prefix=".stb_image-", dir=dependencies) as temporary:
            candidate = Path(temporary) / "source"
            candidate.mkdir()
            for relative, content in files.items():
                path = candidate / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
            os.replace(candidate, populated)
    if not populated.is_dir():
        raise ValueError("stb_image source must be a directory")
    actual = {}
    for path in populated.rglob("*"):
        if linked(path) or not (path.is_file() or path.is_dir()):
            raise ValueError("Linked or nonregular stb_image source input")
        if path.is_file():
            relative = path.relative_to(populated).as_posix()
            if (relative not in files or path.stat().st_size != len(files[relative])
                    or path.read_bytes() != files[relative]):
                raise ValueError("Populated stb_image source differs from the locked archive: " + relative)
            actual[relative] = hashlib.sha256(files[relative]).hexdigest()
    if set(actual) != set(files):
        raise ValueError("Populated stb_image source inventory differs from the locked archive")
    return {"version": locked["version"], "commit": locked["commit"],
            "archive_sha256": locked["sha256"], "source_files": actual,
            "license_sha256": actual["LICENSE"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--download", action="store_true", help="Prepare the missing locked archive with bounded HTTPS")
    arguments = parser.parse_args()
    try:
        if arguments.download:
            download_archive(arguments.archive)
        print(json.dumps(populate(arguments.build, arguments.archive), indent=2))
        return 0
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
