"""Prepare and verify Shark's exact private CPU Jolt archive, without resolution."""

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
MAX_ARCHIVE = 32 * 1024 * 1024
MAX_CONTENT = 64 * 1024 * 1024
MAX_FILES = 4096
MAX_MEMBERS = 8192
OPTIONS = {
    "USE_ASSERTS": False, "DOUBLE_PRECISION": False, "GENERATE_DEBUG_SYMBOLS": True,
    "OVERRIDE_CXX_FLAGS": False, "CROSS_PLATFORM_DETERMINISTIC": False,
    "CROSS_COMPILE_ARM": False, "JPH_BUILD_SHARED_LIBS": False,
    "INTERPROCEDURAL_OPTIMIZATION": False, "FLOATING_POINT_EXCEPTIONS_ENABLED": False,
    "CPP_EXCEPTIONS_ENABLED": True, "CPP_RTTI_ENABLED": True, "OBJECT_LAYER_BITS": 16,
    "USE_SSE4_1": False, "USE_SSE4_2": False, "USE_AVX": False, "USE_AVX2": False,
    "USE_AVX512": False, "USE_LZCNT": False, "USE_TZCNT": False, "USE_F16C": False,
    "USE_FMADD": False, "USE_WASM_SIMD": False, "JPH_USE_WASM64": False,
    "ENABLE_ALL_WARNINGS": False, "TRACK_BROADPHASE_STATS": False,
    "TRACK_NARROWPHASE_STATS": False, "JPH_TRACK_SIMULATION_STATS": False,
    "DEBUG_RENDERER_IN_DEBUG_AND_RELEASE": False, "DEBUG_RENDERER_IN_DISTRIBUTION": False,
    "PROFILER_IN_DEBUG_AND_RELEASE": False, "PROFILER_IN_DISTRIBUTION": False,
    "JPH_USE_EXTERNAL_PROFILE": False, "DISABLE_CUSTOM_ALLOCATOR": False,
    "USE_STD_VECTOR": False, "ENABLE_OBJECT_STREAM": False, "ENABLE_INSTALL": False,
    "USE_STATIC_MSVC_RUNTIME_LIBRARY": False, "JPH_USE_DX12": False, "JPH_USE_VK": False,
    "JPH_USE_MTL": False, "JPH_USE_CPU_COMPUTE": False, "JPH_SHADER_DEBUG_SYMBOLS": False,
    "JPH_SHADER_OPTIMIZATION": False, "TARGET_UNIT_TESTS": False, "TARGET_HELLO_WORLD": False,
    "TARGET_PERFORMANCE_TEST": False, "TARGET_SAMPLES": False, "TARGET_VIEWER": False,
}
INTEGRATION = {
    "cpp_standard": 20, "cpu_baseline": "x86-64 SSE2", "precompiled_headers": False,
    "unity_build": False, "thread_link": "Threads::Threads",
    "sanitizers": "Shark target-scoped profile",
}
REVIEWED_SOURCES = {
    "Build/CMakeLists.txt", "Jolt/Jolt.cmake", "LICENSE", "Jolt/Jolt.h", "Jolt/Core/Memory.h",
    "Jolt/Core/Factory.h", "Jolt/Core/TempAllocator.h", "Jolt/Core/JobSystemSingleThreaded.h",
    "Jolt/Physics/PhysicsSystem.h", "Jolt/RegisterTypes.h",
}


def linked(path: Path) -> bool:
    return path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction())


def safe_relative(name: str) -> bool:
    if not isinstance(name, str):
        return False
    path = PurePosixPath(name)
    return (isinstance(name, str) and bool(name) and "\\" not in name and ":" not in name
            and not path.is_absolute() and ".." not in path.parts
            and path.as_posix() == name and name != ".")


def entry(source: Path | None = None) -> dict:
    root = ROOT if source is None else source
    locked = json.loads((root / "dependencies.lock.json").read_text(encoding="utf-8"))["dependencies"]["Jolt"]
    if (locked.get("version") != "5.6.0" or locked.get("license") != "MIT"
            or locked.get("license_files") != ["LICENSE"] or locked.get("options") != OPTIONS
            or locked.get("integration") != INTEGRATION or locked.get("patches") != []
            or locked.get("transitive_dependencies") != []
            or not re.fullmatch(r"[0-9a-f]{40}", locked.get("commit", ""))
            or not re.fullmatch(r"[0-9a-f]{64}", locked.get("sha256", ""))
            or locked.get("archive_prefix") != "JoltPhysics-" + locked["commit"]
            or locked.get("url") != "https://codeload.github.com/jrouwe/JoltPhysics/tar.gz/" + locked["commit"]
            or type(locked.get("archive_bytes")) is not int
            or not 0 < locked["archive_bytes"] <= MAX_ARCHIVE
            or type(locked.get("source_file_count")) is not int
            or not 0 < locked["source_file_count"] <= MAX_FILES
            or type(locked.get("source_bytes")) is not int
            or not 0 < locked["source_bytes"] <= MAX_CONTENT
            or not isinstance(locked.get("source_sha256"), dict)
            or set(locked["source_sha256"]) != REVIEWED_SOURCES
            or any(not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value)
                   for value in locked["source_sha256"].values())):
        raise ValueError("Jolt dependency metadata requires a new integration review")
    return locked


def specification() -> dict:
    return entry()


def archive_files(archive: Path, locked: dict | None = None) -> dict[str, bytes]:
    locked = entry() if locked is None else locked
    if (not archive.is_file() or linked(archive)
            or archive.stat().st_size != locked["archive_bytes"]
            or archive.stat().st_size > MAX_ARCHIVE):
        raise ValueError("Missing, linked, oversized or size-mismatched Jolt archive")
    data = archive.read_bytes()
    if hashlib.sha256(data).hexdigest() != locked["sha256"]:
        raise ValueError("Jolt archive SHA-256 differs from the dependency lock")
    files = {}
    total = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as stream:
        for index, member in enumerate(stream):
            if index >= MAX_MEMBERS:
                raise ValueError("Oversized Jolt archive member inventory")
            path = PurePosixPath(member.name)
            if (path.is_absolute() or ".." in path.parts or "\\" in member.name or ":" in member.name
                    or not path.parts or path.parts[0] != locked["archive_prefix"]
                    or not (member.isfile() or member.isdir())):
                raise ValueError("Unsafe Jolt archive member")
            if member.isdir():
                continue
            relative = path.relative_to(locked["archive_prefix"]).as_posix()
            total += member.size
            if (not safe_relative(relative) or relative in files or len(files) >= MAX_FILES
                    or member.size < 0 or total > MAX_CONTENT):
                raise ValueError("Duplicate, unsafe or oversized Jolt archive contents")
            source = stream.extractfile(member)
            if source is None:
                raise ValueError("Missing Jolt archive member data")
            content = source.read(member.size + 1)
            if len(content) != member.size:
                raise ValueError("Truncated Jolt archive member")
            files[relative] = content
    if len(files) != locked["source_file_count"] or total != locked["source_bytes"]:
        raise ValueError("Jolt archive inventory differs from the dependency lock")
    for relative, expected in locked["source_sha256"].items():
        if relative not in files or hashlib.sha256(files[relative]).hexdigest() != expected:
            raise ValueError("Jolt reviewed source hash differs from the lock: " + relative)
    return files


def prepare(source: Path, destination: Path) -> Path:
    """Explicit bounded archive preparation, before a disconnected configure."""
    locked = entry(source)
    destination.mkdir(parents=True, exist_ok=True)
    if linked(destination):
        raise ValueError("Linked Jolt archive cache directory")
    archive = destination / ("jolt-" + locked["commit"] + ".tar.gz")
    download_archive(archive, locked)
    return archive


def download_archive(archive: Path, locked: dict | None = None) -> None:
    locked = entry() if locked is None else locked
    if archive.exists() or linked(archive):
        archive_files(archive, locked)
        return
    with urllib.request.urlopen(locked["url"], timeout=60) as response:
        data = response.read(locked["archive_bytes"] + 1)
    if len(data) != locked["archive_bytes"] or hashlib.sha256(data).hexdigest() != locked["sha256"]:
        raise ValueError("Downloaded Jolt archive differs from the locked size or SHA-256")
    if linked(archive.parent):
        raise ValueError("Linked Jolt archive cache directory")
    archive.parent.mkdir(parents=True, exist_ok=True)
    candidate = None
    try:
        with tempfile.NamedTemporaryFile(prefix=".jolt-", dir=archive.parent, delete=False) as stream:
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


def verify_sources(source: Path, build: Path, archive: Path) -> dict:
    locked = entry(source)
    files = archive_files(archive, locked)
    return _verify_source_files(build, archive, locked, files)


def _verify_source_files(build: Path, archive: Path, locked: dict, files: dict[str, bytes]) -> dict:
    """Check every populated byte against this invocation's verified archive."""
    populated = build / "_deps" / "jolt-src"
    if not populated.is_dir() or linked(populated):
        raise ValueError("Missing or linked Jolt source directory")
    actual = {}
    for path in populated.rglob("*"):
        if linked(path) or not (path.is_file() or path.is_dir()):
            raise ValueError("Linked or nonregular Jolt source input")
        if path.is_file():
            relative = path.relative_to(populated).as_posix()
            if (relative not in files or path.stat().st_size != len(files[relative])
                    or path.read_bytes() != files[relative]):
                raise ValueError("Populated Jolt source differs from the locked archive: " + relative)
            actual[relative] = hashlib.sha256(files[relative]).hexdigest()
    if set(actual) != set(files):
        raise ValueError("Populated Jolt source inventory differs from the locked archive")
    return {"version": locked["version"], "commit": locked["commit"], "archive": str(archive),
            "archive_sha256": locked["sha256"], "source_files": actual,
            "license_file": "LICENSE", "license_sha256": actual["LICENSE"],
            "license_files": {"LICENSE": actual["LICENSE"]}, "omitted_unbuilt_symlinks": 0}


def populate(build: Path, archive: Path) -> dict:
    locked = entry()
    files = archive_files(archive, locked)
    dependencies = build / "_deps"
    populated = dependencies / "jolt-src"
    if linked(dependencies) or linked(populated):
        raise ValueError("Linked Jolt source directory")
    dependencies.mkdir(parents=True, exist_ok=True)
    if not populated.exists():
        with tempfile.TemporaryDirectory(prefix=".jolt-", dir=dependencies) as temporary:
            candidate = Path(temporary) / "source"
            candidate.mkdir()
            directories = {parent for relative in files for parent in Path(relative).parents
                           if parent != Path(".")}
            for directory in sorted(directories, key=lambda path: (len(path.parts), path.as_posix())):
                (candidate / directory).mkdir()
            for relative, content in files.items():
                destination = candidate / relative
                destination.write_bytes(content)
            os.replace(candidate, populated)
    return _verify_source_files(build, archive, locked, files)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--download", action="store_true")
    arguments = parser.parse_args()
    try:
        if arguments.output:
            if arguments.build or arguments.archive or arguments.download:
                parser.error("--output cannot be combined with population options")
            archive = prepare(ROOT, arguments.output.absolute())
            print(json.dumps({"status": "PASS", "archive": str(archive), "sha256": entry()["sha256"]}))
            return 0
        if not arguments.build or not arguments.archive:
            parser.error("Population requires --build and --archive")
        if arguments.download:
            download_archive(arguments.archive)
        record = populate(arguments.build, arguments.archive)
        print(json.dumps({"status": "PASS", "version": record["version"],
                          "archive_sha256": record["archive_sha256"],
                          "regular_files": len(record["source_files"])}))
        return 0
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        print(json.dumps({"status": "FAIL", "error": str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
