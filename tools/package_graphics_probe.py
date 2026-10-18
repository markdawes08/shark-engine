"""Construct an inspected Windows graphics probe package from explicit local inputs.

This is an assignment 02 fixture, not a game exporter or a release qualification.
No downloads, SDK installation, source compilation, signing or GPU work occur.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import sys
import uuid

from graphics_probe import (ROOT, SHADERS, checked_file, inspect_result, probe_source_digest,
                            read_json, sha256, shader_inputs, source_state, validation_tools)
from prepare_graphics_validation import MAX_ARCHIVE, assert_no_links, source_files
from process_runner import run_process
from toolchain_report import collect

CRT_NAMES = {"msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"}
SYSTEM_DLLS = {
    "advapi32.dll", "bcrypt.dll", "cfgmgr32.dll", "combase.dll", "crypt32.dll",
    "gdi32.dll", "imm32.dll", "kernel32.dll", "ntdll.dll", "ole32.dll", "oleaut32.dll",
    "rpcrt4.dll", "setupapi.dll", "shell32.dll", "shlwapi.dll", "user32.dll",
    "userenv.dll", "version.dll", "winmm.dll", "wintrust.dll", "ws2_32.dll", "ucrtbase.dll",
}
# These UCRT API contracts are provided by the declared Windows 11 deployment
# baseline. This is an explicit list, not a wildcard allowing arbitrary DLLs.
SYSTEM_DLLS.update(f"api-ms-win-crt-{name}-l1-1-0.dll" for name in (
    "conio", "convert", "environment", "filesystem", "heap", "locale", "math",
    "multibyte", "private", "process", "runtime", "stdio", "string", "time", "utility"))
MAX_FILE = 128 * 1024 * 1024
MAX_PACKAGE = 512 * 1024 * 1024
MAX_FILES = 256


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def relative_name(name: str) -> str:
    path = PurePosixPath(name)
    if (not name or "\\" in name or path.is_absolute() or path.as_posix() != name
            or any(part in (".", "..") or ":" in part for part in path.parts)):
        raise ValueError(f"Unsafe package-relative path: {name}")
    return name


def regular_file(path: Path, expected: str | None = None, maximum: int = MAX_FILE) -> bytes:
    assert_no_links(path)
    if not path.is_file() or path.stat().st_size > maximum:
        raise ValueError(f"Missing or oversized package input: {path}")
    if expected is not None:
        checked_file(path, expected, maximum=maximum)
    data = path.read_bytes()
    if len(data) > maximum or (expected is not None and digest(data) != expected):
        raise ValueError(f"Package input changed while reading: {path}")
    return data


def cache_values(build_dir: Path, executable: Path) -> dict[str, str]:
    assert_no_links(build_dir)
    text = regular_file(build_dir / "CMakeCache.txt", maximum=4 * 1024 * 1024).decode("utf-8")
    values = {key: value.rstrip("\r") for key, value in re.findall(r"^([^/#:=\n]+):[^=\n]+=(.*)$", text, re.M)}
    if values.get("CMAKE_BUILD_TYPE") != "Release":
        raise ValueError("The packaged probe requires an explicit Release build; Debug/ASan builds are not redistributable")
    if values.get("SHARK_SANITIZER") != "none" or values.get("SHARK_BUILD_GRAPHICS_PROBE") != "ON":
        raise ValueError("Package requires the graphics probe with sanitizers disabled")
    if Path(values.get("CMAKE_HOME_DIRECTORY", "")).resolve() != ROOT.resolve():
        raise ValueError("Build cache belongs to a different source checkout")
    if executable.resolve() != (build_dir / "bin/GraphicsProbe.exe").resolve():
        raise ValueError("Executable must be the selected Release build's bin/GraphicsProbe.exe")
    return values


def pe_x64(data: bytes, name: str) -> None:
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError(f"Package native input is not a PE binary: {name}")
    offset = struct.unpack_from("<I", data, 0x3c)[0]
    if offset > len(data) - 6 or data[offset:offset + 4] != b"PE\0\0":
        raise ValueError(f"Invalid PE header: {name}")
    if struct.unpack_from("<H", data, offset + 4)[0] != 0x8664:
        raise ValueError(f"Package requires native Windows x64 binaries: {name}")


def parse_dependents(output: str) -> list[str]:
    if "Image has the following dependencies:" not in output:
        raise ValueError("dumpbin did not report a PE dependency table")
    dependencies = []
    for line in output.splitlines():
        name = line.strip()
        if line[:1].isspace() and name.lower().endswith(".dll"):
            if not re.fullmatch(r"[A-Za-z0-9_.-]+\.dll", name, re.I):
                raise ValueError(f"Unexpected path in PE import table: {name}")
            dependencies.append(name.lower())
    if not dependencies or len(dependencies) > 128:
        raise ValueError("Missing or excessive PE dependency records")
    return sorted(set(dependencies))


def inspect_closure(binaries: dict[str, Path], inspector: Path) -> dict:
    """Inspect every packaged binary, including the dynamically loaded layer.

    Only the package bin directory and named Windows 11 system contracts satisfy
    imports. A developer PATH, SDK or arbitrary System32 DLL cannot fill a hole.
    """
    assert_no_links(inspector)
    inspector_hash = sha256(inspector)
    bin_names = {PurePosixPath(name).name.lower() for name in binaries if name.startswith("bin/")}
    records = {}
    for name, binary in sorted(binaries.items()):
        data = regular_file(binary)
        pe_x64(data, name)
        result = run_process([str(inspector), "/NOLOGO", "/DEPENDENTS", str(binary)], cwd=ROOT, timeout=30)
        if result.returncode or result.timed_out or result.cleanup_errors:
            raise ValueError(f"Dependency inspection failed: {name}")
        imports = parse_dependents(result.stdout)
        for dependency in imports:
            if re.fullmatch(r"(?:msvcp\d+d|vcruntime\d+(?:_\d+)?d|ucrtbased)\.dll", dependency):
                raise ValueError(f"Debug CRT is not redistributable: {dependency} imported by {name}")
            if dependency not in bin_names and dependency not in SYSTEM_DLLS:
                raise ValueError(f"Unpackaged external dependency: {name} imports {dependency}")
        if sha256(binary) != digest(data):
            raise ValueError(f"Binary changed during dependency inspection: {name}")
        records[name] = {"sha256": digest(data), "imports": imports, "inspection": asdict(result)}
    if sha256(inspector) != inspector_hash:
        raise ValueError("Dependency inspector changed during packaging")
    return {"status": "PASS", "inspector": {"path": str(inspector), "sha256": inspector_hash},
            "binaries": records, "system_allowlist": sorted(SYSTEM_DLLS),
            "scope": "Static and delay-load imports; vendor ICDs are separately installed system driver prerequisites"}


def crt_inputs(directory: Path, manifest: dict) -> tuple[dict[str, Path], dict[str, bytes], dict]:
    assert_no_links(directory)
    if (manifest.get("architecture") != "x86_64"
            or not isinstance(manifest.get("version"), str) or not manifest["version"]
            or not isinstance(manifest.get("files"), dict) or set(manifest["files"]) != CRT_NAMES):
        raise ValueError("CRT receipt must identify the exact three release CRT DLLs, source and version")
    # An explicit redistributable directory is required, never an implicit
    # system DLL search. Its selected bytes are pinned by the reviewable receipt.
    binaries = {}
    for name, expected in manifest["files"].items():
        regular_file(directory / name, expected)
        binaries["bin/" + name] = directory / name
    notices = manifest.get("license_files")
    if not isinstance(notices, list) or not 1 <= len(notices) <= 8:
        raise ValueError("CRT receipt must retain redistribution license/notices")
    files = {}
    for notice in notices:
        if not isinstance(notice, dict) or not isinstance(notice.get("path"), str):
            raise ValueError("Invalid CRT license declaration")
        path = ROOT / relative_name(notice["path"])
        name = relative_name(notice.get("destination", ""))
        if "/" in name:
            raise ValueError("CRT license must have a simple destination name")
        if not isinstance(notice.get("sha256"), str):
            raise ValueError("CRT license must have a locked SHA-256")
        destination = "licenses/MicrosoftVC/" + name
        if destination in files:
            raise ValueError("Duplicate CRT license destination")
        files[destination] = regular_file(path, notice.get("sha256"), maximum=8 * 1024 * 1024)
    return binaries, files, manifest


def archive_notices(archive: Path, entry: dict) -> dict[str, bytes]:
    files = source_files(regular_file(archive, entry["sha256"], MAX_ARCHIVE), entry)
    return {relative_name(name): files[name] for name in entry["license_files"]}


def windows_version_info(path: Path) -> dict:
    """Read the selected DLL's version resource, without invoking a shell."""
    import ctypes
    from ctypes import wintypes

    version = ctypes.WinDLL("version", use_last_error=True)
    version.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    version.GetFileVersionInfoSizeW.restype = wintypes.DWORD
    version.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID]
    version.GetFileVersionInfoW.restype = wintypes.BOOL
    version.VerQueryValueW.argtypes = [wintypes.LPCVOID, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    version.VerQueryValueW.restype = wintypes.BOOL
    ignored = wintypes.DWORD()
    size = version.GetFileVersionInfoSizeW(str(path), ctypes.byref(ignored))
    if not 0 < size <= 1024 * 1024:
        raise ValueError(f"CRT lacks a bounded Windows version resource: {path}")
    buffer = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(str(path), 0, size, buffer):
        raise ValueError(f"Cannot read CRT version resource: {path}")

    def query(key: str) -> tuple[int, int]:
        address, length = ctypes.c_void_p(), wintypes.UINT()
        if not version.VerQueryValueW(buffer, key, ctypes.byref(address), ctypes.byref(length)) or not address.value:
            raise ValueError(f"CRT version resource lacks {key}: {path}")
        return address.value, length.value

    address, length = query("\\")
    if length < 13 * 4:
        raise ValueError("CRT fixed version resource is truncated")
    words = ctypes.cast(address, ctypes.POINTER(wintypes.DWORD * 13)).contents
    if words[0] != 0xFEEF04BD:
        raise ValueError("CRT fixed version resource has an invalid signature")
    file_version = ".".join(str(value) for value in (words[2] >> 16, words[2] & 0xffff, words[3] >> 16, words[3] & 0xffff))
    address, length = query("\\VarFileInfo\\Translation")
    if length < 4:
        raise ValueError("CRT resource has no language translation")
    language, codepage = ctypes.cast(address, ctypes.POINTER(wintypes.WORD * 2)).contents
    fields = {}
    for field in ("FileVersion", "ProductVersion", "LegalCopyright", "CompanyName"):
        address, length = query(f"\\StringFileInfo\\{language:04x}{codepage:04x}\\{field}")
        if not 1 <= length <= 8192:
            raise ValueError("CRT version string exceeds its bound")
        fields[field] = ctypes.wstring_at(address, length).rstrip("\0")
    return {"file_version": file_version, "strings": fields}


def dependency_notices(build_dir: Path, lock: dict, validation: dict) -> tuple[dict[str, bytes], dict]:
    files = {}
    inputs = {}
    for name in ("GLFW", "NVRHI", "VulkanHeaders", "VulkanLoader"):
        entry = lock["dependencies"][name]
        lower = name.lower()
        archive_name = entry["url"].rsplit("/", 1)[-1]
        archive = build_dir / f"_deps/{lower}-subbuild/{lower}-populate-prefix/src" / archive_name
        notices = archive_notices(archive, entry)
        for relative, content in notices.items():
            files[f"licenses/{name}/{relative}"] = content
        inputs[name] = {"archive_sha256": entry["sha256"], "license": entry["license"],
                        "license_files": {name: digest(data) for name, data in notices.items()}}
    # The validation preparation receipt provides source locations; the adjacent
    # immutable archives are rehashed. Its copied notices alone are not trusted.
    validation_entries = {"VulkanHeaders": lock["dependencies"]["VulkanHeaders"],
                          **lock["graphics_validation"]["dependencies"]}
    for name, entry in validation_entries.items():
        record = validation["inputs"][name]
        if record.get("archive_sha256") != entry["sha256"]:
            raise ValueError(f"Validation source receipt is not locked: {name}")
        source = Path(record["source_directory"])
        if not source.is_absolute():
            raise ValueError("Validation source receipt requires absolute source paths")
        archive = source.parent / (entry["sha256"] + ".tar.gz")
        notices = archive_notices(archive, entry)
        for relative, content in notices.items():
            files[f"licenses/{name}/{relative}"] = content
        inputs[name] = {"archive_sha256": entry["sha256"], "license": entry["license"],
                        "license_files": {name: digest(data) for name, data in notices.items()}}
    return files, inputs


def json_bytes(value: dict) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")


def package_layer_manifest(source: dict) -> bytes:
    # The locked Windows loader recognizes a manifest-relative library only
    # when it contains DIRECTORY_SYMBOL ('\\'). A forward-slash './' remains
    # unexpanded, then fails LoadLibraryExW with ERROR_INVALID_PARAMETER.
    layer = {**source["layer"], "library_path": ".\\VkLayer_khronos_validation.dll"}
    return json_bytes({**source, "layer": layer})


def prepare_payload(args: argparse.Namespace) -> tuple[dict[str, bytes], dict]:
    packager_path = ROOT / "tools/package_graphics_probe.py"
    packager_hash = digest(regular_file(packager_path, maximum=1024 * 1024))
    executable, build_dir = args.executable.absolute(), args.build_dir.absolute()
    cache_values(build_dir, executable)
    lock_path = ROOT / "dependencies.lock.json"
    lock_data = regular_file(lock_path)
    lock_digest = digest(lock_data)
    lock = read_json(lock_path)
    source_digest = probe_source_digest()
    regular_file(args.validation_manifest, maximum=8 * 1024 * 1024)
    validation = validation_tools(args.validation_manifest, lock_digest)
    assert_no_links(args.shader_dir)
    for name in (*SHADERS, "shader-manifest.json"):
        regular_file(args.shader_dir / name, maximum=8 * 1024 * 1024)
    shaders = shader_inputs(args.shader_dir, lock_digest)
    binaries, payload, crt = crt_inputs(args.crt_dir, lock["graphics_distribution"]["WindowsCRT"])
    crt_metadata = {name: windows_version_info(args.crt_dir / name) for name in sorted(CRT_NAMES)}
    if any(item["file_version"] != crt.get("file_version") for item in crt_metadata.values()):
        raise ValueError("CRT version resource disagrees with the locked full file version")
    crt = {**crt, "binary_metadata": crt_metadata}
    binaries.update({"bin/GraphicsProbe.exe": executable,
                     "bin/vulkan-1.dll": executable.parent / "vulkan-1.dll",
                     "validation/VkLayer_khronos_validation.dll": Path(validation["validation_layer"]["library_path"])})
    inspector_name = shutil.which("dumpbin")
    if inspector_name is None:
        raise ValueError("dumpbin is required; run packaging in tools/windows_dev.ps1")
    closure = inspect_closure(binaries, Path(inspector_name))
    for name, binary in binaries.items():
        payload[name] = regular_file(binary, closure["binaries"][name]["sha256"])
    described = run_process([str(executable), "--describe"], cwd=ROOT, timeout=15)
    description, errors = inspect_result(described, "describe", lock_digest, source_digest)
    if errors or described.returncode != 0:
        raise ValueError("Release executable identity check failed: " + "; ".join(errors))
    for name in SHADERS:
        record = next(item for item in shaders["shaders"] if item["file"] == name)
        payload["shaders/" + name] = regular_file(args.shader_dir / name, record["sha256"], 4 * 1024 * 1024)
    layer = read_json(Path(validation["validation_layer"]["manifest_path"]))
    payload["validation/VkLayer_khronos_validation.json"] = package_layer_manifest(layer)
    payload["validation/vk_layer_settings.txt"] = b""
    notices, dependencies = dependency_notices(build_dir, lock, validation)
    payload.update(notices)
    payload["Run-GraphicsProbe.ps1"] = regular_file(ROOT / "tools/graphics_package/Run-GraphicsProbe.ps1", maximum=1024 * 1024)
    payload["provenance/dependencies.lock.json"] = lock_data
    payload["provenance/shader-manifest.json"] = json_bytes(shaders)
    payload["provenance/crt-manifest.json"] = json_bytes(crt)
    payload["provenance/dependency-closure.json"] = json_bytes(closure)
    source = source_state()
    source["input_sha256"]["tools/package_graphics_probe.py"] = packager_hash
    source["input_sha256"]["tools/graphics_package/Run-GraphicsProbe.ps1"] = digest(payload["Run-GraphicsProbe.ps1"])
    payload["provenance/source.json"] = json_bytes(source)
    payload["provenance/toolchain.json"] = json_bytes(collect(build_dir))
    payload["provenance/probe-description.json"] = json_bytes(description)
    payload["README.txt"] = (
        "Shark assignment 02 Windows x64 graphics probe fixture\n\n"
        "This is a development/qualification probe, not the game editor or a game export.\n"
        "Requires Windows 11 25H2, the selected GPU's installed Vulkan driver, an interactive\n"
        "desktop, and Windows PowerShell 5.1. No Python, Visual Studio/C++ compiler, Vulkan SDK or source checkout\n"
        "is required to run this package. Use an ordinary (not elevated) terminal.\n\n"
        "From this directory: powershell -NoProfile -ExecutionPolicy Bypass -File .\\Run-GraphicsProbe.ps1 -Mode inventory -Output <fresh-evidence-directory>\n"
        "Then use -Mode cases -Device <inventory-UUID> -Output <fresh-evidence-directory>.\n"
        "The launcher verifies package hashes, selects the bundled validation layer, captures\n"
        "bounded machine-readable evidence, and requires explicit device selection for cases.\n"
        "Read the launcher's Get-Help output for lifecycle/soak budgets.\n\n"
        "Package construction PASS does not qualify a GPU, clean machine, or shipping release.\n"
        "Run qualification on the named native hardware; missing hardware is NOT_RUN.\n"
        "The package is unsigned. Source/binary notices are retained under licenses/.\n"
        "The app-local Microsoft release CRT must be serviced by rebuilding this package with\n"
        "a reviewed updated runtime pin; a machine-wide runtime update does not replace it.\n"
        "Offline HLSL compilers and spirv-val are deliberately absent; prepared SPIR-V and\n"
        "runtime Vulkan validation are included. Vendor GPU drivers are system prerequisites.\n"
    ).encode("utf-8")
    # Recheck original receipts and binaries after all preparation, before any
    # package is made visible. Changing an input never publishes partial output.
    if sha256(lock_path) != lock_digest or probe_source_digest() != source_digest:
        raise ValueError("Source/lock changed during packaging")
    regular_file(packager_path, packager_hash)
    regular_file(ROOT / "tools/graphics_package/Run-GraphicsProbe.ps1", digest(payload["Run-GraphicsProbe.ps1"]))
    if validation_tools(args.validation_manifest, lock_digest) != validation or shader_inputs(args.shader_dir, lock_digest) != shaders:
        raise ValueError("Shader/validation receipt changed during packaging")
    for name, binary in binaries.items():
        regular_file(binary, digest(payload[name]))
    manifest = {"schema_version": 1, "assignment": "02d", "status": "PASS", "qualification": "NOT_RUN",
                "scope": "Windows x64 package construction and dependency closure; hardware and clean-target execution remain separate",
                "timestamp_utc": datetime.now(timezone.utc).isoformat(), "platform": "windows-x86_64",
                "configuration": "Release", "dependency_lock_sha256": lock_digest,
                "probe_source_sha256": source_digest,
                "probe": {"schema_version": 2, "dependency_lock_sha256": lock_digest,
                          "probe_source_sha256": source_digest, "executable": "bin/GraphicsProbe.exe"},
                "commands": {"launcher": "Run-GraphicsProbe.ps1", "modes": ["describe", "inventory", "smoke", "cases"],
                             "probe_status_codes": {"0": "PASS", "1": "FAIL", "2": "FAIL", "3": "NOT_RUN"}},
                "dependencies": dependencies, "crt": crt,
                "validation": {"api_version": validation["validation_layer"]["api_version"],
                               "manifest": "validation/VkLayer_khronos_validation.json",
                               "library": "validation/VkLayer_khronos_validation.dll"}}
    return payload, manifest


def publish(output: Path, payload: dict[str, bytes], manifest: dict) -> dict:
    assert_no_links(output)
    if output.exists():
        raise ValueError("Package output must be fresh; existing output was preserved")
    if not payload or len(payload) > MAX_FILES or sum(map(len, payload.values())) > MAX_PACKAGE:
        raise ValueError("Package file/byte budget exceeded")
    names = [relative_name(name) for name in payload]
    if len({name.lower() for name in names}) != len(names) or "package-manifest.json" in payload:
        raise ValueError("Duplicate or reserved package path")
    for name, content in payload.items():
        if len(content) > MAX_FILE:
            raise ValueError(f"Package file byte budget exceeded: {name}")
    manifest = {**manifest, "files": {name: {"sha256": digest(data), "bytes": len(data)}
                                      for name, data in sorted(payload.items())}}
    output.parent.mkdir(parents=True, exist_ok=True)
    assert_no_links(output.parent)
    staging = output.parent / (".graphics-package-" + uuid.uuid4().hex)
    staging.mkdir()
    try:
        for name, content in payload.items():
            destination = staging / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(content)
            regular_file(destination, digest(content))
        (staging / "validation/implicit").mkdir(parents=True, exist_ok=True)
        (staging / "package-manifest.json").write_bytes(json_bytes(manifest))
        if output.exists():
            raise ValueError("Package output appeared during preparation; existing output was preserved")
        staging.rename(output)
    finally:
        if staging.exists():
            assert_no_links(staging)
            if staging.resolve().parent != output.parent.resolve():
                raise ValueError("Refusing cleanup outside the owned package staging directory")
            shutil.rmtree(staging)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("executable", "build-dir", "shader-dir", "validation-manifest", "crt-dir", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    report = {"schema_version": 1, "assignment": "02d", "status": "FAIL", "qualification": "NOT_RUN"}
    try:
        assert_no_links(args.output)
        if args.output.exists():
            raise ValueError("Package output must be fresh; existing output was preserved")
        if os.name != "nt":
            report.update(status="NOT_RUN", reason="Native Windows packaging requires Windows x64 and dumpbin")
            print(json.dumps(report, indent=2))
            return 3
        payload, manifest = prepare_payload(args)
        manifest = publish(args.output.absolute(), payload, manifest)
        report.update(status="PASS", package_directory=str(args.output.absolute()),
                      manifest_sha256=sha256(args.output / "package-manifest.json"),
                      files=len(manifest["files"]), bytes=sum(item["bytes"] for item in manifest["files"].values()))
        code = 0
    except (OSError, ValueError, KeyError, TypeError, UnicodeError) as error:
        report["reason"] = str(error)
        code = 1
    print(json.dumps(report, indent=2))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
