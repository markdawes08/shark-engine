"""Verify the pinned private Server SQLite input; no system library resolution."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import stat
import tempfile
import urllib.request
import zipfile

from core_dependencies import DEPENDENCIES, verify_sources as verify_core_sources


ROOT = Path(__file__).resolve().parents[1]
MAX_ARCHIVE = 4 * 1024 * 1024
MAX_CONTENT = 16 * 1024 * 1024
ARCHIVE_NAMES = {"sqlite3.c", "sqlite3.h", "sqlite3ext.h", "shell.c"}
DEFINITIONS = ("SQLITE_THREADSAFE=1", "SQLITE_OMIT_LOAD_EXTENSION=1", "SQLITE_DQS=0",
               "SQLITE_DEFAULT_MEMSTATUS=0", "SQLITE_USE_URI=0", "SQLITE_MAX_ATTACHED=0",
               "SQLITE_DEFAULT_FOREIGN_KEYS=1", "SQLITE_ENABLE_API_ARMOR=1")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def entry(source: Path = ROOT) -> dict:
    locked = json.loads((source / "dependencies.lock.json").read_text(encoding="utf-8"))["dependencies"]["SQLite"]
    if (locked["license"] != "LicenseRef-SQLite-Public-Domain"
            or locked["license_files"] != ["LICENSE.txt"] or locked["patches"]
            or locked["transitive_dependencies"] or locked["compile_definitions"] != list(DEFINITIONS)):
        raise ValueError("SQLite dependency metadata requires a new integration review")
    return locked


def archive_files(archive: Path, locked: dict) -> dict[str, bytes]:
    if not archive.is_file() or archive.is_symlink() or archive.stat().st_size > MAX_ARCHIVE:
        raise ValueError("Missing, linked or oversized SQLite archive")
    data = archive.read_bytes()
    if digest(data) != locked["sha256"] or len(data) != locked["archive_bytes"]:
        raise ValueError("SQLite archive SHA-256 or size differs from the dependency lock")
    files = {}
    total = 0
    prefix = locked["archive_prefix"]
    with zipfile.ZipFile(io.BytesIO(data)) as stream:
        for member in stream.infolist():
            path = PurePosixPath(member.filename)
            mode = member.external_attr >> 16
            if (path.is_absolute() or ".." in path.parts or "\\" in member.filename
                    or not path.parts or path.parts[0] != prefix or member.flag_bits & 1
                    or stat.S_ISLNK(mode)):
                raise ValueError("Unsafe SQLite archive member")
            if member.is_dir():
                if path.as_posix().rstrip("/") != prefix:
                    raise ValueError("Unexpected SQLite archive directory")
                continue
            relative = path.relative_to(prefix).as_posix()
            total += member.file_size
            if relative not in ARCHIVE_NAMES or relative in files or total > MAX_CONTENT:
                raise ValueError("Duplicate, unexpected or oversized SQLite archive contents")
            files[relative] = stream.read(member)
    if set(files) != ARCHIVE_NAMES:
        raise ValueError("Incomplete SQLite source archive")
    for name, expected in locked["source_sha256"].items():
        if digest(files[name]) != expected:
            raise ValueError("SQLite source hash differs: " + name)
    header = files["sqlite3.h"]
    if (b'#define SQLITE_VERSION        "' + locked["version"].encode() + b'"' not in header
            or b'#define SQLITE_SOURCE_ID      "' + locked["source_id"].encode() + b'"' not in header):
        raise ValueError("SQLite source identity differs from the lock")
    end = header.find(b"*/")
    if not header.startswith(b"/*") or end < 0:
        raise ValueError("Missing SQLite public-domain notice")
    notice = header[:end + 2] + b"\n"
    if digest(notice) != locked["notice_sha256"] or len(notice) != locked["notice_bytes"]:
        raise ValueError("SQLite notice differs from reviewed header extraction")
    files["LICENSE.txt"] = notice
    return files


def verify_sources(source: Path, build: Path, archive: Path) -> dict:
    locked = entry(source)
    expected = archive_files(archive, locked)
    root = build / "_deps/sqlite-src"
    if root.is_symlink() or not root.is_dir():
        raise ValueError("Missing or linked SQLite source directory")
    actual = {}
    for path in root.rglob("*"):
        if path.is_symlink() or not path.is_file():
            raise ValueError("Linked or nonregular SQLite source input")
        relative = path.relative_to(root).as_posix()
        if relative not in expected or path.read_bytes() != expected[relative]:
            raise ValueError("Populated SQLite source differs from locked archive: " + relative)
        actual[relative] = digest(expected[relative])
    if set(actual) != set(expected):
        raise ValueError("Populated SQLite source inventory differs from locked archive")
    return {"version": locked["version"], "source_id": locked["source_id"],
            "archive": str(archive), "archive_sha256": locked["sha256"],
            "source_files": actual, "compiled_files": ["sqlite3.c"],
            "license_files": {"LICENSE.txt": locked["notice_sha256"]}}


def populate(build: Path, archive: Path, source: Path = ROOT) -> dict:
    files = archive_files(archive, entry(source))
    dependencies = build / "_deps"
    target = dependencies / "sqlite-src"
    if dependencies.is_symlink() or target.is_symlink():
        raise ValueError("Linked SQLite dependency directory")
    dependencies.mkdir(parents=True, exist_ok=True)
    if not target.exists():
        with tempfile.TemporaryDirectory(prefix=".sqlite-", dir=dependencies) as temporary:
            candidate = Path(temporary) / "source"
            candidate.mkdir()
            for name, content in files.items():
                (candidate / name).write_bytes(content)
            os.replace(candidate, target)
    return verify_sources(source, build, archive)


def prepare(source: Path, destination: Path) -> Path:
    locked = entry(source)
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / (locked["archive_prefix"] + ".zip")
    if not archive.exists():
        with urllib.request.urlopen(locked["url"], timeout=60) as response:
            data = response.read(MAX_ARCHIVE + 1)
        if len(data) > MAX_ARCHIVE or digest(data) != locked["sha256"]:
            raise ValueError("Downloaded SQLite archive differs from lock or exceeds limit")
        with archive.open("xb") as output:
            output.write(data)
    archive_files(archive, locked)
    return archive


def verify_server_build(source: Path, build: Path, archives: dict[str, Path], sqlite: Path) -> dict:
    if set(archives) != set(DEPENDENCIES):
        raise ValueError("Missing locked Core archive evidence")
    expected = {spec["prefix"] + suffix for spec in DEPENDENCIES.values()
                for suffix in ("-src", "-build", "-subbuild")}
    expected.remove("mbedtls-subbuild")
    expected.remove("jolt-subbuild")
    expected.add("sqlite-src")
    root = build / "_deps"
    if (root.is_symlink() or not root.is_dir() or {p.name for p in root.iterdir()} != expected
            or any(p.is_symlink() or not p.is_dir() for p in root.iterdir())):
        raise ValueError("Server-only dependency tree must contain exactly the locked Core and storage populations")
    result = {name: verify_core_sources(source, build, archive, name) for name, archive in archives.items()}
    result["SQLite"] = verify_sources(source, build, sqlite)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args()
    try:
        if args.build:
            if not args.archive:
                parser.error("--build requires --archive")
            record = populate(args.build.absolute(), args.archive.absolute())
        elif args.output:
            archive = prepare(ROOT, args.output.absolute())
            record = {"archive": str(archive), "sha256": digest(archive.read_bytes())}
        else:
            parser.error("--build or --output is required")
        print(json.dumps({"status": "PASS", **record}, indent=2))
        return 0
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
