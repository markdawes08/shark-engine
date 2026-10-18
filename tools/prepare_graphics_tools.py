"""Prepare locked offline DXC and independently validate executable probe shaders.

This is development tooling only. Offline compilation is not GPU execution or
hardware qualification. Use --cases with the locked validation tools receipt.
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
import stat
import struct
import sys
import tarfile
import tempfile
import urllib.request
import uuid
import zipfile

from process_runner import run_process
from graphics_probe import SHADERS, sha256, validation_tools

ROOT = Path(__file__).resolve().parents[1]
MAX_ARCHIVE = 64 * 1024 * 1024
MAX_MEMBER = 48 * 1024 * 1024
MAX_EXTRACTED = 64 * 1024 * 1024
MAX_MEMBERS = 512


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def native_host() -> str:
    machine = platform.machine().lower()
    cpu = "x86_64" if machine in ("amd64", "x86_64") else machine
    return f"{platform.system().lower()}-{cpu}"


def member_name(name: str) -> str:
    # Tar archives commonly prefix paths with './'. Reject Windows spellings as
    # well, even when the current host is POSIX. Microsoft's ZIP uses backslash
    # separators; normalize those before the same containment checks.
    name = name.replace("\\", "/")
    while name.startswith("./"):
        name = name[2:]
    path = PurePosixPath(name)
    if not name or "\0" in name or ":" in name or path.is_absolute() or ".." in path.parts:
        raise ValueError(f"Unsafe archive path: {name!r}")
    return str(path)


def selected_files(data: bytes, artifact: dict) -> dict[str, bytes]:
    """Verify the entire archive before bounded, regular-file-only extraction."""
    if len(data) > MAX_ARCHIVE or digest(data) != artifact["sha256"]:
        raise ValueError("DXC archive size or SHA-256 does not match the dependency lock")
    wanted = set(artifact["files"])
    if not wanted or len(wanted) != len(artifact["files"]):
        raise ValueError("Invalid DXC file allowlist")
    for name in wanted:
        if member_name(name) != name:
            raise ValueError("DXC allowlist must use normalized relative paths")
    result: dict[str, bytes] = {}
    seen: set[str] = set()
    total = 0

    def take(name: str, size: int, read) -> None:
        nonlocal total
        name = member_name(name)
        if name in seen:
            raise ValueError(f"Duplicate archive member: {name}")
        seen.add(name)
        if name not in wanted:
            return
        if size < 0 or size > MAX_MEMBER or total + size > MAX_EXTRACTED:
            raise ValueError("DXC extracted file budget exceeded")
        content = read(size + 1)
        if len(content) != size:
            raise ValueError(f"Invalid archive member size: {name}")
        total += size
        result[name] = content

    if artifact["archive_format"] == "zip":
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            members = archive.infolist()
            if len(members) > MAX_MEMBERS:
                raise ValueError("DXC archive member count exceeded")
            for member in members:
                member_name(member.orig_filename)
                if member.is_dir():
                    member_name(member.filename)
                    continue
                mode = member.external_attr >> 16
                if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) not in (0, stat.S_IFREG)):
                    raise ValueError("DXC archive contains a nonregular file")
                with archive.open(member) as stream:
                    take(member.filename, member.file_size, stream.read)
    elif artifact["archive_format"] == "tar.gz":
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
            for count, member in enumerate(archive, 1):
                if count > MAX_MEMBERS:
                    raise ValueError("DXC archive member count exceeded")
                if member.isdir():
                    if member.name not in (".", "./"):
                        member_name(member.name)
                    continue
                if not member.isfile():
                    raise ValueError("DXC archive contains a nonregular file")
                with archive.extractfile(member) as stream:
                    take(member.name, member.size, stream.read)
    else:
        raise ValueError("Unsupported locked DXC archive format")
    if result.keys() != wanted:
        raise ValueError(f"DXC archive is missing required files: {sorted(wanted - result.keys())}")
    return result


def remove_staging(temporary: Path, cache: Path) -> None:
    resolved = temporary.resolve(strict=True)
    cache_root = cache.resolve(strict=True)
    if (resolved == cache_root or not resolved.is_relative_to(cache_root)
            or temporary.is_symlink() or temporary.is_junction()):
        raise ValueError("Refusing recursive cleanup outside the owned DXC cache staging directory")
    shutil.rmtree(temporary)


def prepare(tool: dict, host: str, cache: Path, *, offline: bool) -> tuple[Path, dict]:
    artifact = tool["artifacts"][host]
    cache.mkdir(parents=True, exist_ok=True)
    archive_path = cache / (artifact["sha256"] + ".archive")
    if archive_path.exists():
        if archive_path.is_symlink() or archive_path.stat().st_size > MAX_ARCHIVE:
            raise ValueError("Invalid cached DXC archive")
        data = archive_path.read_bytes()
    else:
        if offline:
            raise ValueError("Locked DXC archive is not cached; run preparation once without --offline")
        if not artifact["url"].startswith("https://github.com/microsoft/DirectXShaderCompiler/releases/download/"):
            raise ValueError("DXC download must be an official HTTPS release asset")
        with urllib.request.urlopen(artifact["url"], timeout=30) as response:
            data = response.read(MAX_ARCHIVE + 1)
    files = selected_files(data, artifact)
    # A partial download is never published as a usable cache input.
    if not archive_path.exists():
        with tempfile.NamedTemporaryFile(dir=cache, delete=False) as temporary:
            temporary.write(data)
            temporary_path = Path(temporary.name)
        try:
            temporary_path.replace(archive_path)
        finally:
            temporary_path.unlink(missing_ok=True)
    installed = cache / (tool["version"] + "-" + host)
    if installed.is_symlink():
        raise ValueError("DXC install directory must not be a symlink")
    if installed.exists():
        # Recheck installed bytes against the immutable archive every time. Never
        # silently execute a modified local compiler or repair it behind evidence.
        entries = list(installed.rglob("*"))
        if any(entry.is_symlink() for entry in entries):
            raise ValueError("DXC install contains a symlink")
        actual = {entry.relative_to(installed).as_posix() for entry in entries if entry.is_file()}
        if actual != files.keys():
            raise ValueError("DXC installed file inventory does not match the locked archive")
        for name, content in files.items():
            target = installed / name
            if any(parent.is_symlink() for parent in target.parents if parent != cache.parent):
                raise ValueError("DXC installed path contains a symlink")
            if not target.is_file() or target.is_symlink() or target.stat().st_size != len(content):
                raise ValueError(f"DXC installed file is missing or changed: {name}")
            if digest(target.read_bytes()) != digest(content):
                raise ValueError(f"DXC installed file hash mismatch: {name}")
    else:
        # Python 3.13 creates mkdtemp directories with an explicit private ACL on
        # Windows. Renaming one into a persistent cache can make administrator-
        # prepared tools unreadable to the same user's standard token. Inherit
        # the workspace/cache ACL instead; do not rewrite ACLs after extraction.
        temporary = cache / ("dxc-" + uuid.uuid4().hex)
        temporary.mkdir()
        try:
            for name, content in files.items():
                target = temporary / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(content)
            if os.name != "nt":
                (temporary / artifact["executable"]).chmod(0o755)
            temporary.rename(installed)
        finally:
            if temporary.exists():
                remove_staging(temporary, cache)
    return installed / artifact["executable"], {
        "url": artifact["url"], "archive_sha256": artifact["sha256"],
        "files": {name: digest(content) for name, content in sorted(files.items())},
    }


def compile_fixture(executable: Path, output: Path, evidence: dict | None = None) -> dict:
    fixture = ROOT / "probes/graphics/shaders/setup.hlsl"
    flags = ["-spirv", "-fspv-target-env=vulkan1.3", "-T", "cs_6_0", "-E", "main", "-O3", "-Ges", "-WX"]
    commands = []
    evidence = {} if evidence is None else evidence
    evidence.update(status="FAIL", commands=commands,
                    independent_spirv_validation="NOT RUN; locked SPIRV-Tools integration belongs to 02b",
                    gpu_execution="NOT RUN; this is an offline setup fixture")

    def invoke(args: list[str], name: str):
        result = run_process([str(executable), *args], cwd=ROOT, timeout=30)
        (output / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        commands.append({"phase": name, **asdict(result)})
        if result.timed_out or result.cleanup_errors:
            raise ValueError(f"DXC {name} timed out or failed process cleanup")
        return result

    version = invoke(["--version"], "version")
    if version.returncode:
        raise ValueError("Locked DXC could not report its version")
    products = []
    for index in (1, 2):
        destination = output / f"setup-{index}.spv"
        destination.unlink(missing_ok=True)
        compiled = invoke([*flags, str(fixture), "-Fo", str(destination)], f"compile-{index}")
        if compiled.returncode or not destination.is_file():
            raise ValueError(f"DXC fixture compilation {index} failed")
        products.append(destination.read_bytes())
    if products[0] != products[1]:
        raise ValueError("Repeated DXC compilation produced different SPIR-V bytes")
    binary = products[0]
    if len(binary) < 20 or len(binary) % 4 or struct.unpack_from("<I", binary)[0] != 0x07230203:
        raise ValueError("DXC output does not contain a SPIR-V header")
    invalid = output / "invalid.hlsl"
    invalid.write_text("This is deliberately invalid HLSL.\n", encoding="utf-8")
    rejected_path = output / "invalid.spv"
    rejected_path.unlink(missing_ok=True)
    rejected = invoke([*flags, str(invalid), "-Fo", str(rejected_path)], "reject-invalid")
    # The locked release returns E_FAIL on Windows and its low byte (5) on
    # POSIX. A crash/signal is not successful validation of the bad fixture.
    expected_rejection = 0x80004005 if os.name == "nt" else 5
    if (rejected.returncode != expected_rejection or "error:" not in rejected.stderr
            or rejected_path.exists()):
        raise ValueError("DXC did not reject the invalid shader cleanly")
    evidence.update({
        "status": "PASS", "fixture": fixture.relative_to(ROOT).as_posix(),
        "fixture_sha256": digest(fixture.read_bytes()), "spirv_sha256": digest(binary),
        "spirv_bytes": len(binary), "spirv_version_word": hex(struct.unpack_from("<I", binary, 4)[0]),
        "repeat_bytes_equal": True, "invalid_shader_rejected": True,
        "dxc_internal_validation": "enabled (no -Vd)",
        "independent_spirv_validation": "NOT RUN; locked SPIRV-Tools integration belongs to 02b",
        "gpu_execution": "NOT RUN; this is an offline setup fixture", "commands": commands,
    })
    return evidence


def runtime_dependencies(executable: Path, output: Path) -> dict:
    """Record native inspection when available, without requiring a developer SDK."""
    commands = []
    if platform.system() == "Linux":
        inspector = shutil.which("ldd")
        if inspector:
            commands.append([inspector, str(executable)])
        package_query = shutil.which("dpkg-query")
        if package_query:
            commands.append([package_query, "-W", "-f=${binary:Package}\t${Version}\n",
                             "libc6", "libstdc++6", "libgcc-s1", "zlib1g"])
    elif os.name == "nt":
        inspector = shutil.which("dumpbin")
        if inspector:
            for binary in sorted(executable.parent.glob("*")):
                if binary.suffix.lower() in (".exe", ".dll"):
                    commands.append([inspector, "/NOLOGO", "/DEPENDENTS", str(binary)])
    if not commands:
        return {"status": "NOT_RUN", "reason": "Native dependency inspector not available; Windows can use tools/windows_dev.ps1"}
    results = [asdict(run_process(command, cwd=ROOT, timeout=30)) for command in commands]
    (output / "runtime-dependencies.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    passed = all(result["returncode"] == 0 and not result["timed_out"] and not result["cleanup_errors"]
                 for result in results)
    return {"status": "PASS" if passed else "FAIL", "commands": results,
            "scope": "Observed native tool dependencies; not a portable package or engine runtime qualification"}


def compile_cases(executable: Path, output: Path, manifest_path: Path, lock_digest: str,
                  evidence: dict | None = None) -> dict:
    tools = validation_tools(manifest_path, lock_digest)
    validator = Path(tools["tools"]["spirv_val"]["path"])
    fixture = ROOT / "probes/graphics/shaders/cases.hlsl"
    directory = output / "shaders"
    directory.mkdir(exist_ok=False)
    commands = []
    evidence = {} if evidence is None else evidence
    evidence.update(status="FAIL", commands=commands, shaders=[])
    shader_records = evidence["shaders"]

    def invoke(command: list[str], name: str):
        result = run_process(command, cwd=ROOT, timeout=30)
        (output / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        commands.append({"phase": name, **asdict(result)})
        if result.timed_out or result.cleanup_errors:
            raise ValueError(f"Shader tool {name} timed out or failed process cleanup")
        return result

    source_hash = sha256(fixture)
    compiler_hash = sha256(executable)
    for name, (entry, profile) in SHADERS.items():
        flags = ["-spirv", "-fspv-target-env=vulkan1.3", "-T", profile,
                 "-E", entry, "-O3", "-Ges", "-WX"]
        destination = directory / name
        command = [str(executable), *flags, str(fixture), "-Fo", str(destination)]
        compiled = invoke(command, name + "-compile")
        if compiled.returncode or not destination.is_file():
            raise ValueError(f"DXC compilation failed for {name}")
        binary = destination.read_bytes()
        if (not 20 <= len(binary) <= 4 * 1024 * 1024 or len(binary) % 4
                or struct.unpack_from("<I", binary)[0] != 0x07230203):
            raise ValueError(f"Invalid SPIR-V output for {name}")
        repeated = output / (name + ".repeat")
        repeat_command = [str(executable), *flags, str(fixture), "-Fo", str(repeated)]
        repeat = invoke(repeat_command, name + "-repeat")
        if repeat.returncode or not repeated.is_file() or repeated.read_bytes() != binary:
            raise ValueError(f"Repeated shader compilation was not byte-identical for {name}")
        validation_command = [str(validator), "--target-env", "vulkan1.3", str(destination)]
        validated = invoke(validation_command, name + "-validate")
        if validated.returncode:
            raise ValueError(f"Independent SPIR-V validation failed for {name}")
        shader_records.append({"file": name, "sha256": digest(binary), "bytes": len(binary),
                               "source": fixture.relative_to(ROOT).as_posix(), "source_sha256": source_hash,
                               "entry": entry, "profile": profile, "compile_status": "PASS",
                               "validation_status": "PASS", "repeat_bytes_equal": True,
                               "compile_command": command, "validation_command": validation_command})
    invalid_hlsl = output / "invalid-case.hlsl"
    invalid_hlsl.write_text("Deliberately invalid HLSL for bounded rejection.\n", encoding="utf-8")
    invalid_output = output / "invalid-case.spv"
    rejected = invoke([str(executable), "-spirv", "-fspv-target-env=vulkan1.3", "-T", "cs_6_0",
                       "-E", "main", "-Ges", "-WX", str(invalid_hlsl), "-Fo", str(invalid_output)],
                      "cases-reject-invalid-hlsl")
    expected_rejection = 0x80004005 if os.name == "nt" else 5
    if rejected.returncode != expected_rejection or "error:" not in rejected.stderr or invalid_output.exists():
        raise ValueError("DXC did not reject the invalid case shader normally")
    invalid_spirv = output / "invalid-case-binary.spv"
    # A real SPIR-V header followed by an instruction with invalid zero word count.
    invalid_spirv.write_bytes(struct.pack("<6I", 0x07230203, 0x10600, 0, 1, 0, 0))
    rejected = invoke([str(validator), "--target-env", "vulkan1.3", str(invalid_spirv)],
                      "cases-reject-invalid-spirv")
    if rejected.returncode != 1 or "error" not in (rejected.stdout + rejected.stderr).lower():
        raise ValueError("Independent SPIR-V validator did not reject the corrupt binary normally")
    if sha256(fixture) != source_hash:
        raise ValueError("Shader source changed during compilation")
    if sha256(executable) != compiler_hash:
        raise ValueError("Shader compiler changed during compilation")
    # Recheck the receipt and files after execution as well as before it.
    if validation_tools(manifest_path, lock_digest) != tools:
        raise ValueError("Validation tools receipt changed during shader preparation")
    manifest = {"schema_version": 1, "status": "PASS", "dependency_lock_sha256": lock_digest,
                "compiler": {"path": str(executable.resolve()), "sha256": compiler_hash},
                "validator": {"path": str(validator), "sha256": sha256(validator),
                              "manifest_sha256": sha256(manifest_path)},
                "shaders": shader_records, "invalid_shader_rejected": True, "invalid_spirv_rejected": True}
    manifest_file = directory / "shader-manifest.json"
    manifest_file.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    evidence.update(status="PASS", manifest_path=str(manifest_file), manifest_sha256=sha256(manifest_file),
                    invalid_shader_rejected=True, invalid_spirv_rejected=True)
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--offline", action="store_true", help="Require the already cached immutable archive")
    parser.add_argument("--cases", action="store_true", help="Compile and independently validate all executable graphics fixtures")
    parser.add_argument("--validation-manifest", type=Path, help="Locked validation tools preparation receipt, required by --cases")
    parser.add_argument("--output", type=Path, help="Fresh evidence directory (default out/graphics-tools/<host>-<timestamp>)")
    args = parser.parse_args()
    if args.cases != bool(args.validation_manifest):
        parser.error("--cases and --validation-manifest must be provided together")
    host = native_host()
    timestamp = datetime.now(timezone.utc)
    output = (args.output or ROOT / "out/graphics-tools" /
              f"{host}-{timestamp.strftime('%Y%m%dT%H%M%S%fZ')}").resolve()
    report = {"schema_version": 1, "assignment": "02b" if args.cases else "02a", "host": host,
              "timestamp_utc": timestamp.isoformat(), "evidence_directory": str(output),
              "os": platform.platform(), "python": platform.python_version(),
              "status": "FAIL", "scope": "offline DXC setup, no GPU qualification"}
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        report["reason"] = f"Evidence requires a fresh directory; existing evidence was preserved: {error}"
        print(json.dumps(report, indent=2))
        return 2
    code = 1
    try:
        lock_bytes = (ROOT / "dependencies.lock.json").read_bytes()
        tool = json.loads(lock_bytes)["graphics_tools"]["DXC"]
        report["dependency_lock_sha256"] = digest(lock_bytes)
        report["version"] = tool["version"]
        report["source_revision"] = tool["source_revision"]
        if host not in tool["artifacts"]:
            report.update(status="NOT_RUN", reason="No locked official DXC binary for this native host; macOS source/toolchain qualification remains open")
            code = 3
        else:
            executable, provenance = prepare(tool, host, ROOT / ".tools/graphics/dxc", offline=args.offline)
            report["provenance"] = provenance
            report["runtime_dependencies"] = runtime_dependencies(executable, output)
            if report["runtime_dependencies"]["status"] == "FAIL":
                raise ValueError("Available native tool dependency inspection failed")
            report["compile"] = {}
            compile_fixture(executable, output, report["compile"])
            if args.cases:
                report["cases"] = {}
                compile_cases(executable, output, args.validation_manifest.resolve(), digest(lock_bytes), report["cases"])
            report["status"] = "PASS"
            code = 0
    except (OSError, ValueError, KeyError, tarfile.TarError, zipfile.BadZipFile) as error:
        report["reason"] = str(error)
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
