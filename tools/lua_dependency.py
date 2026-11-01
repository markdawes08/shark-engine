"""Prepare and verify the exact private Lua source archive used by Core."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SOURCES = (
    "lapi.c", "lcode.c", "lctype.c", "ldebug.c", "ldo.c", "ldump.c", "lfunc.c",
    "lgc.c", "llex.c", "lmem.c", "lobject.c", "lopcodes.c", "lparser.c", "lstate.c",
    "lstring.c", "ltable.c", "ltm.c", "lundump.c", "lvm.c", "lzio.c", "lauxlib.c",
    "lbaselib.c", "lmathlib.c", "lstrlib.c", "ltablib.c", "lutf8lib.c",
)
REVIEWED = {"doc/readme.html", "src/lua.h", "src/lauxlib.h", "src/lualib.h",
            "src/luaconf.h"} | {"src/" + name for name in SOURCES}


def linked(path: Path) -> bool:
    return path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction())


def entry(source: Path = ROOT) -> dict:
    value = json.loads((source / "dependencies.lock.json").read_text(encoding="utf-8"))["dependencies"]["Lua"]
    if (value.get("version") != "5.5.1" or value.get("archive_prefix") != "lua-5.5.1"
            or value.get("url") != "https://www.lua.org/ftp/lua-5.5.1.tar.gz"
            or value.get("sha256") != "1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce"
            or value.get("archive_bytes") != 398643 or value.get("source_file_count") != 75
            or value.get("source_bytes") != 1416954 or value.get("license") != "MIT"
            or value.get("license_files") != ["doc/readme.html"] or value.get("sources") != list(SOURCES)
            or value.get("patches") != [] or value.get("transitive_dependencies") != []
            or value.get("compile_definitions") != []
            or set(value.get("source_sha256", {})) != REVIEWED):
        raise ValueError("Lua dependency metadata requires a new integration review")
    return value


def archive_files(archive: Path, locked: dict | None = None) -> dict[str, bytes]:
    locked = entry() if locked is None else locked
    if not archive.is_file() or linked(archive) or archive.stat().st_size != locked["archive_bytes"]:
        raise ValueError("Missing, linked or size-mismatched Lua archive")
    data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != locked["sha256"]:
        raise ValueError("Lua archive SHA-256 differs from the dependency lock")
    files = {}
    total = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as stream:
        for index, member in enumerate(stream):
            path = PurePosixPath(member.name)
            if (index >= 100 or path.is_absolute() or ".." in path.parts or "\\" in member.name
                    or ":" in member.name or not path.parts or path.parts[0] != locked["archive_prefix"]
                    or not (member.isfile() or member.isdir())):
                raise ValueError("Unsafe Lua archive member")
            if member.isdir():
                continue
            relative = path.relative_to(locked["archive_prefix"]).as_posix()
            total += member.size
            if (relative in files or relative == "." or member.size < 0
                    or len(files) >= 75 or total > 1416954):
                raise ValueError("Duplicate or oversized Lua archive member")
            source = stream.extractfile(member)
            if source is None:
                raise ValueError("Missing Lua archive member data")
            data = source.read(member.size + 1)
            if len(data) != member.size:
                raise ValueError("Truncated Lua archive member")
            files[relative] = data
    if len(files) != locked["source_file_count"] or total != locked["source_bytes"]:
        raise ValueError("Lua archive inventory differs from the dependency lock")
    for name, expected in locked["source_sha256"].items():
        if name not in files or hashlib.sha256(files[name]).hexdigest() != expected:
            raise ValueError("Lua reviewed source hash differs: " + name)
    return files


def prepare(source: Path, destination: Path) -> Path:
    locked = entry(source)
    if linked(destination):
        raise ValueError("Linked Lua archive cache directory")
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / "lua-5.5.1.tar.gz"
    if not archive.exists():
        with urllib.request.urlopen(locked["url"], timeout=60) as response:
            data = response.read(locked["archive_bytes"] + 1)
        if len(data) != locked["archive_bytes"] or hashlib.sha256(data).hexdigest() != locked["sha256"]:
            raise ValueError("Downloaded Lua archive differs from the dependency lock")
        with archive.open("xb") as output:
            output.write(data)
    archive_files(archive, locked)
    return archive


def verify_sources(source: Path, build: Path, archive: Path) -> dict:
    locked = entry(source)
    expected = archive_files(archive, locked)
    root = build / "_deps/lua-src"
    if linked(root) or not root.is_dir():
        raise ValueError("Missing or linked Lua source directory")
    actual = {}
    for path in root.rglob("*"):
        if linked(path) or not (path.is_file() or path.is_dir()):
            raise ValueError("Linked or nonregular Lua source input")
        if path.is_file():
            name = path.relative_to(root).as_posix()
            if name not in expected or path.read_bytes() != expected[name]:
                raise ValueError("Populated Lua source differs from locked archive: " + name)
            actual[name] = hashlib.sha256(expected[name]).hexdigest()
    if set(actual) != set(expected):
        raise ValueError("Populated Lua source inventory differs from locked archive")
    return {"version": locked["version"], "archive": str(archive), "archive_sha256": locked["sha256"],
            "source_files": actual, "compiled_files": list(SOURCES), "license_file": "doc/readme.html",
            "license_sha256": actual["doc/readme.html"],
            "license_files": {"doc/readme.html": actual["doc/readme.html"]}}


def populate(build: Path, archive: Path, source: Path = ROOT) -> dict:
    files = archive_files(archive, entry(source))
    dependencies = build / "_deps"
    target = dependencies / "lua-src"
    if linked(dependencies) or linked(target):
        raise ValueError("Linked Lua dependency directory")
    dependencies.mkdir(parents=True, exist_ok=True)
    if not target.exists():
        with tempfile.TemporaryDirectory(prefix=".lua-", dir=dependencies) as temporary:
            candidate = Path(temporary) / "source"
            candidate.mkdir()
            for name, content in files.items():
                path = candidate / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
            os.replace(candidate, target)
    return verify_sources(source, build, archive)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args()
    try:
        if args.build and args.archive:
            record = populate(args.build.absolute(), args.archive.absolute())
        elif args.output and not args.build:
            archive = prepare(ROOT, args.output.absolute())
            record = {"archive": str(archive), "sha256": entry()["sha256"]}
        else:
            parser.error("--output or --build with --archive is required")
        print(json.dumps({"status": "PASS", **record}, indent=2))
        return 0
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
