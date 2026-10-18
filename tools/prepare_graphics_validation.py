"""Build exact independent SPIR-V and Vulkan validation tools without installing an SDK.

This separate project is opt-in. --offline requires cached verified archives; it
never changes the engine/Server configure graph or machine Vulkan registration.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import sys
import tarfile
import tempfile
import urllib.request
import uuid

from prepare_graphics_tools import native_host, runtime_dependencies
from process_runner import run_process

ROOT = Path(__file__).resolve().parents[1]
MAX_ARCHIVE = 128 * 1024 * 1024
MAX_FILE = 64 * 1024 * 1024
MAX_EXPANDED = 512 * 1024 * 1024
MAX_MEMBERS = 30000


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def source_files(data: bytes, entry: dict) -> dict[str, bytes]:
    """Validate the whole immutable archive before writing any selected source."""
    if len(data) > MAX_ARCHIVE or digest(data) != entry["sha256"]:
        raise ValueError("Source archive SHA-256 or size mismatch")
    files = {}
    seen = set()
    roots = set()
    total = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        for index, member in enumerate(archive):
            if index >= MAX_MEMBERS:
                raise ValueError("Source archive has too many members")
            name = member.name.replace("\\", "/")
            path = PurePosixPath(name)
            if (path.is_absolute() or not path.parts or ".." in path.parts
                    or any(":" in part for part in path.parts)
                    or not (member.isdir() or member.isfile())):
                raise ValueError(f"Unsafe source archive member: {name}")
            roots.add(path.parts[0])
            relative = PurePosixPath(*path.parts[1:]).as_posix()
            if relative in seen:
                raise ValueError(f"Duplicate source archive member: {relative}")
            seen.add(relative)
            if not member.isfile():
                continue
            total += member.size
            if relative == "." or member.size > MAX_FILE or total > MAX_EXPANDED:
                raise ValueError("Source archive expanded size or root is invalid")
            files[relative] = archive.extractfile(member).read()
    if len(roots) != 1 or not files or "CMakeLists.txt" not in files:
        raise ValueError("Source archive must have one project root")
    if any(name not in files for name in entry["license_files"]):
        raise ValueError("Source archive is missing locked license notices")
    return files


def assert_no_links(path: Path) -> None:
    for parent in (path, *path.parents):
        if parent.is_symlink() or (hasattr(parent, "is_junction") and parent.is_junction()):
            raise ValueError(f"Validation cache path contains a link: {parent}")


def verify_sources(path: Path, files: dict[str, bytes], generated_outputs: tuple[str, ...] = ()) -> None:
    assert_no_links(path)
    entries = list(path.rglob("*"))
    for entry in entries:
        assert_no_links(entry)
    actual = {entry.relative_to(path).as_posix() for entry in entries if entry.is_file()}
    if actual != files.keys():
        raise ValueError("Cached validation source inventory changed; preserve it for investigation")
    for name, expected in files.items():
        candidate = path / name
        if candidate.stat().st_size > MAX_FILE:
            raise ValueError(f"Cached validation source size changed: {name}")
        actual_bytes = candidate.read_bytes()
        # Upstream regenerates these two shipped settings files in place. The
        # Windows Python text writer changes line endings, not source content.
        allowed_newlines = (os.name == "nt" and name in generated_outputs
                            and b"\r\n" not in expected
                            and actual_bytes == expected.replace(b"\n", b"\r\n"))
        if actual_bytes != expected and not allowed_newlines:
            raise ValueError(f"Cached validation source bytes changed: {name}")


def remove_staging(path: Path, cache: Path) -> None:
    assert_no_links(path)
    resolved = path.resolve(strict=True)
    parent = cache.resolve(strict=True)
    if resolved == parent or not resolved.is_relative_to(parent):
        raise ValueError("Refusing cleanup outside the owned validation cache")
    shutil.rmtree(resolved)


def prepare_source(name: str, entry: dict, cache: Path, *, offline: bool) -> tuple[Path, dict]:
    assert_no_links(cache)
    cache.mkdir(parents=True, exist_ok=True)
    archive_path = cache / (entry["sha256"] + ".tar.gz")
    assert_no_links(archive_path)
    if archive_path.exists():
        if archive_path.stat().st_size > MAX_ARCHIVE:
            raise ValueError("Cached validation archive exceeds the bound")
        data = archive_path.read_bytes()
    else:
        if offline:
            raise ValueError(f"Locked {name} archive is not cached; prepare once without --offline")
        if (not entry["url"].startswith("https://github.com/KhronosGroup/")
                or "/archive/" not in entry["url"]):
            raise ValueError("Validation sources must use official Khronos HTTPS archives")
        with urllib.request.urlopen(entry["url"], timeout=30) as response:
            data = response.read(MAX_ARCHIVE + 1)
    files = source_files(data, entry)
    if not archive_path.exists():
        with tempfile.NamedTemporaryFile(dir=cache, delete=False) as temporary:
            temporary.write(data)
            temporary_path = Path(temporary.name)
        try:
            temporary_path.replace(archive_path)
        finally:
            temporary_path.unlink(missing_ok=True)
    destination = cache / f"{name}-{entry['sha256'][:16]}"
    if destination.exists():
        verify_sources(destination, files, tuple(entry.get("generated_outputs", ())))
    else:
        # A published cache must inherit its workspace permissions. Python 3.13
        # mkdtemp(mode=0700) produces an administrator-only Windows ACL when
        # preparation is elevated, preventing later standard-user validation.
        temporary = cache / ("validation-" + uuid.uuid4().hex)
        temporary.mkdir()
        try:
            for relative, content in files.items():
                target = temporary / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(content)
            temporary.rename(destination)
        finally:
            if temporary.exists():
                remove_staging(temporary, cache)
    return destination, {
        "url": entry["url"], "archive_sha256": entry["sha256"],
        "source_directory": str(destination), "files_verified": len(files),
        "license_files": {name: digest(files[name]) for name in entry["license_files"]},
        "generated_output_sha256": {name: digest((destination / name).read_bytes())
                                    for name in entry.get("generated_outputs", ())},
    }


def file_identity(path: Path) -> dict:
    assert_no_links(path)
    if not path.is_file():
        raise ValueError(f"Missing validation product: {path}")
    return {"path": str(path.resolve()), "sha256": digest(path.read_bytes()), "bytes": path.stat().st_size}


def validation_identity(products: dict) -> dict:
    manifest_path = Path(products["validation_manifest"])
    library_path = Path(products["validation_library"])
    manifest = file_identity(manifest_path)
    library = file_identity(library_path)
    layer = json.loads(manifest_path.read_text(encoding="utf-8"))["layer"]
    selected = (manifest_path.parent / layer["library_path"]).resolve()
    if selected != library_path.resolve() or layer["name"] != "VK_LAYER_KHRONOS_validation":
        raise ValueError("Validation layer manifest must select the built local library")
    return {
        "manifest_path": manifest["path"], "manifest_sha256": manifest["sha256"],
        "library_path": library["path"], "library_sha256": library["sha256"],
        "name": layer["name"], "api_version": layer["api_version"],
    }


def verify_cache(cache: Path, lock: dict) -> None:
    inputs = {"VulkanHeaders": lock["dependencies"]["VulkanHeaders"],
              **lock["graphics_validation"]["dependencies"]}
    assert_no_links(cache)
    for name, entry in inputs.items():
        archive = cache / (entry["sha256"] + ".tar.gz")
        assert_no_links(archive)
        if not archive.is_file() or archive.stat().st_size > MAX_ARCHIVE:
            raise ValueError(f"Missing or oversized locked validation archive: {name}")
        files = source_files(archive.read_bytes(), entry)
        verify_sources(cache / f"{name}-{entry['sha256'][:16]}", files,
                       tuple(entry.get("generated_outputs", ())))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--offline", action="store_true", help="Require verified archive cache; no network")
    parser.add_argument("--output", type=Path, help="Fresh evidence directory")
    parser.add_argument("--verify-cache", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--jobs", type=int, default=2, help="Parallel compiler jobs (1..16)")
    args = parser.parse_args()
    if not 1 <= args.jobs <= 16:
        parser.error("--jobs must be in 1..16")
    if args.verify_cache is not None:
        try:
            verify_cache(args.verify_cache, json.loads((ROOT / "dependencies.lock.json").read_text()))
            return 0
        except (OSError, ValueError, KeyError, tarfile.TarError) as error:
            print(str(error), file=sys.stderr)
            return 1
    if args.output is None:
        parser.error("--output is required")
    output = args.output.resolve()
    host = native_host()
    report = {"schema_version": 1, "assignment": "02b", "status": "FAIL", "host": host,
              "timestamp_utc": datetime.now(timezone.utc).isoformat(),
              "os": platform.platform(), "python": platform.python_version(),
              "scope": "Independent validation tool build; GPU validation requires an actual probe run",
              "commands": [], "inputs": {}}
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        print(json.dumps({**report, "reason": f"Fresh evidence directory required: {error}"}, indent=2))
        return 2
    code = 1
    try:
        lock_bytes = (ROOT / "dependencies.lock.json").read_bytes()
        lock = json.loads(lock_bytes)
        report["dependency_lock_sha256"] = digest(lock_bytes)
        report["preparation_source_sha256"] = {
            name: digest((ROOT / name).read_bytes()) for name in (
                "cmake/GraphicsValidation.cmake", "tools/graphics_validation/CMakeLists.txt",
                "tools/prepare_graphics_validation.py", "tools/process_runner.py")}
        if host not in ("windows-x86_64", "linux-x86_64"):
            report.update(status="NOT_RUN", reason="Unsupported native host; validation tooling targets Windows x64 and Linux x64")
            code = 3
        else:
            cache = ROOT / ".tools/graphics/validation/sources"
            inputs = {"VulkanHeaders": lock["dependencies"]["VulkanHeaders"],
                      **lock["graphics_validation"]["dependencies"]}
            for name, entry in inputs.items():
                source, report["inputs"][name] = prepare_source(name, entry, cache, offline=args.offline)
                for relative in entry["license_files"]:
                    notice = output / "licenses" / name / relative
                    notice.parent.mkdir(parents=True, exist_ok=True)
                    notice.write_bytes((source / relative).read_bytes())
            build = ROOT / "build" / ("graphics-validation-" + host)
            assert_no_links(build)
            environment = {**os.environ, "FORCED_BUILD_VERSION_DESCRIPTION": inputs["SPIRVTools"]["commit"],
                           "PYTHONDONTWRITEBYTECODE": "1"}

            def invoke(command: list[str], name: str, timeout: int) -> dict:
                result = asdict(run_process(command, cwd=ROOT, timeout=timeout, env=environment))
                (output / (name + ".log")).write_text(result["stdout"] + result["stderr"], encoding="utf-8")
                report["commands"].append({"phase": name, **result})
                if result["returncode"] or result["timed_out"] or result["cleanup_errors"]:
                    raise ValueError(f"Validation {name} failed; see {output / (name + '.log')}")
                return result

            cmake = shutil.which("cmake")
            if not cmake:
                raise ValueError("Pinned CMake is unavailable; use the development tool environment")
            cmake_version = invoke([cmake, "--version"], "cmake-version", 30)
            ninja = shutil.which("ninja")
            if not ninja:
                raise ValueError("Pinned Ninja is unavailable; use the development tool environment")
            ninja_version = invoke([ninja, "--version"], "ninja-version", 30)
            report["toolchain"] = {
                "cmake": {**file_identity(Path(cmake)), "version": cmake_version["stdout"]},
                "ninja": {**file_identity(Path(ninja)), "version": ninja_version["stdout"]},
                "python": {**file_identity(Path(sys.executable)), "version": platform.python_version()},
                "windows_sdk_version": os.environ.get("WindowsSDKVersion"),
                "build_configuration": lock["graphics_validation"]["build_configuration"],
                "spirv_version_override": inputs["SPIRVTools"]["commit"],
                "upstream_source_patches": [],
            }
            invoke([cmake, "-S", str(ROOT / "tools/graphics_validation"), "-B", str(build), "-G", "Ninja",
                    "-DCMAKE_BUILD_TYPE=" + lock["graphics_validation"]["build_configuration"],
                    "-DSHARK_VALIDATION_SOURCE_ROOT=" + str(cache),
                    "-DPython3_EXECUTABLE=" + sys.executable], "configure", 180)
            # Archive sources live below Shark's Git checkout. Upstream's default
            # version deduction would therefore embed Shark's commit; use its
            # supported override and regenerate before any compile consumes it.
            spirv_source = Path(report["inputs"]["SPIRVTools"]["source_directory"])
            invoke([sys.executable, str(spirv_source / "utils/update_build_version.py"),
                    str(spirv_source / "CHANGES"), str(build / "deps/SPIRVTools/build-version.inc")],
                   "locked-spirv-version", 30)
            invoke([cmake, "--build", str(build), "--target", "shark_graphics_validation", "--parallel", str(args.jobs)],
                   "build", 1800)
            products = json.loads((build / "validation-products.json").read_text(encoding="utf-8"))
            executable = Path(products["spirv_val"])
            version = invoke([str(executable), "--version"], "spirv-val-version", 30)
            if inputs["SPIRVTools"]["commit"] not in version["stdout"]:
                raise ValueError("SPIR-V validator did not report its locked source revision")
            report["tools"] = {"spirv_val": {**file_identity(executable), "version": version["stdout"]}}
            report["validation_layer"] = validation_identity(products)
            if report["validation_layer"]["api_version"] != lock["dependencies"]["VulkanHeaders"]["version"]:
                raise ValueError("Validation layer/header API versions differ")
            report["compiler"] = products["compiler"]
            report["build_directory"] = str(build)
            report["runtime_dependencies"] = runtime_dependencies(executable, output)
            if report["runtime_dependencies"]["status"] == "FAIL":
                raise ValueError("Available native validation-tool dependency inspection failed")
            # Assert upstream configuration/build did not mutate verified inputs.
            for name, entry in inputs.items():
                _, report["inputs"][name] = prepare_source(name, entry, cache, offline=True)
            report["status"] = "PASS"
            code = 0
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        report["reason"] = str(error)
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
