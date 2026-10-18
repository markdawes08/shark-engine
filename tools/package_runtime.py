"""Build and check the local Windows Runtime deployment fixture (not a game export).

The build command requires an already current runtime-only Release build. The
check command launches an explicit relocation outside the checkout, with a
system-only environment and an observed module inventory. Python and dumpbin
are verification tools, never packaged Runtime dependencies.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shutil
import sys
import uuid

from package_graphics_probe import (CRT_NAMES, MAX_FILE, MAX_FILES, MAX_PACKAGE,
                                   ROOT, archive_notices, assert_no_links,
                                   crt_inputs, digest, inspect_closure, json_bytes,
                                   read_json, regular_file, relative_name, sha256,
                                   windows_version_info)
from graphics_probe import finite_float, reject_constant, unique_object
from process_runner import run_process
from toolchain_report import collect
from shader_pipeline import bundle_payload, canonical, inspect_bundle, verify as verify_shaders

BINARIES = {"bin/Runtime.exe", "bin/vulkan-1.dll", *("bin/" + name for name in CRT_NAMES)}
SHADER_BUNDLE = "bin/assets/cube.sharkshader"
SHADER_MANIFEST = "provenance/shader-manifest.json"


def current_build(output: str) -> bool:
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    return bool(lines) and all(line.startswith("ninja: Entering directory ")
                               or line == "ninja: no work to do."
                               or re.fullmatch(r"\[\d+/\d+\] Checking client transitive dependency boundaries", line)
                               for line in lines) and any(not line.startswith("ninja: Entering directory ")
                                                         for line in lines)


def cache_values(build: Path) -> dict[str, str]:
    text = regular_file(build / "CMakeCache.txt", maximum=4 * 1024 * 1024).decode("utf-8")
    values = dict(re.findall(r"^([^/#:=\n]+):[^=\n]+=(.*)$", text, re.M))
    values = {key: value.rstrip("\r") for key, value in values.items()}
    expected = {"CMAKE_BUILD_TYPE": "Release", "SHARK_SANITIZER": "none",
                "SHARK_BUILD_CLIENT": "ON", "SHARK_BUILD_EDITOR": "OFF",
                "SHARK_BUILD_TESTS": "OFF", "SHARK_BUILD_SERVER": "OFF",
                "SHARK_BUILD_GRAPHICS_PROBE": "OFF", "SHARK_NATIVE_WINDOW": "ON",
                "SHARK_NATIVE_GRAPHICS": "ON", "SHARK_INJECT_TEST_FAILURE": "OFF"}
    for key, value in expected.items():
        if values.get(key) != value:
            raise ValueError(f"Runtime package requires {key}={value}")
    if Path(values.get("CMAKE_HOME_DIRECTORY", "")).resolve() != ROOT.resolve():
        raise ValueError("Runtime build belongs to a different source checkout")
    return values


def production_notices(build: Path, lock: dict) -> tuple[dict[str, bytes], dict]:
    payload, records = {}, {}
    for name in ("EnTT", "GLFW", "NVRHI", "VulkanHeaders", "VulkanLoader"):
        entry = lock["dependencies"][name]
        lower = name.lower()
        archive = build / f"_deps/{lower}-subbuild/{lower}-populate-prefix/src" / entry["url"].rsplit("/", 1)[-1]
        if name == "EnTT":
            archive = Path(cache_values(build).get("SHARK_EnTT_ARCHIVE", str(archive)))
        notices = archive_notices(archive, entry)
        if not notices:
            raise ValueError(f"Missing redistribution notices for {name}")
        payload.update({f"licenses/{name}/{path}": data for path, data in notices.items()})
        records[name] = {"archive_sha256": entry["sha256"], "license": entry["license"],
                         "license_files": {path: digest(data) for path, data in notices.items()}}
    return payload, records


def publish(output: Path, payload: dict[str, bytes], manifest: dict) -> dict:
    assert_no_links(output)
    if output.exists():
        raise ValueError("Package output must be fresh; existing output was preserved")
    names = [relative_name(name) for name in payload]
    if (not payload or len(payload) > MAX_FILES or sum(map(len, payload.values())) > MAX_PACKAGE
            or any(len(data) > MAX_FILE for data in payload.values())
            or len({name.lower() for name in names}) != len(names)
            or "package-manifest.json" in payload):
        raise ValueError("Invalid, duplicate, reserved or oversized package payload")
    manifest = {**manifest, "files": {name: {"sha256": digest(data), "bytes": len(data)}
                                      for name, data in sorted(payload.items())}}
    output.parent.mkdir(parents=True, exist_ok=True)
    assert_no_links(output.parent)
    staging = output.parent / (".runtime-package-" + uuid.uuid4().hex)
    staging.mkdir()
    try:
        for name, content in payload.items():
            destination = staging / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(content)
            regular_file(destination, digest(content))
        (staging / "package-manifest.json").write_bytes(json_bytes(manifest))
        if output.exists():
            raise ValueError("Package output appeared during preparation")
        staging.rename(output)
    finally:
        if staging.exists():
            assert_no_links(staging)
            if staging.resolve().parent != output.parent.resolve():
                raise ValueError("Refusing cleanup outside owned package staging parent")
            shutil.rmtree(staging)
    return manifest


def build_package(build: Path, crt_dir: Path, output: Path, shaders: Path | None = None) -> dict:
    build = build.absolute()
    cache_values(build)
    lock_data = regular_file(ROOT / "dependencies.lock.json")
    lock = read_json(ROOT / "dependencies.lock.json")
    commands = []
    for command in (["ninja", "-C", str(build), "-n", "Runtime"],
                    ["cmake", "--build", str(build), "--target", "shark_check_runtime_boundary"]):
        result = run_process(command, cwd=ROOT, timeout=30)
        commands.append(asdict(result))
        if result.returncode or result.timed_out or result.cleanup_errors:
            raise ValueError("Runtime freshness or target boundary check failed")
        if command[0] == "ninja" and not current_build(result.stdout):
            raise ValueError("Runtime build is stale; build runtime-only before packaging")
    binaries, payload, crt = crt_inputs(crt_dir, lock["graphics_distribution"]["WindowsCRT"])
    metadata = {name: windows_version_info(crt_dir / name) for name in sorted(CRT_NAMES)}
    if any(item["file_version"] != crt.get("file_version") for item in metadata.values()):
        raise ValueError("CRT version differs from locked file version")
    binaries.update({"bin/Runtime.exe": build / "bin/Runtime.exe",
                     "bin/vulkan-1.dll": build / "bin/vulkan-1.dll"})
    inspector = shutil.which("dumpbin")
    if inspector is None:
        raise ValueError("dumpbin is required; use tools/windows_dev.ps1")
    closure = inspect_closure(binaries, Path(inspector))
    for name, binary in binaries.items():
        payload[name] = regular_file(binary, closure["binaries"][name]["sha256"])
    notices, dependencies = production_notices(build, lock)
    payload.update(notices)
    payload["provenance/dependencies.lock.json"] = lock_data
    payload["provenance/dependency-closure.json"] = json_bytes(closure)
    payload["provenance/toolchain.json"] = json_bytes(collect(build))
    payload["provenance/crt-manifest.json"] = json_bytes({**crt, "binary_metadata": metadata})
    shader_record = None
    if shaders is not None:
        bundle, shader_record = bundle_payload(shaders.absolute())
        shader_manifest = verify_shaders(shaders.absolute())
        if (digest(canonical(shader_manifest)) != shader_record["artifact_manifest_sha256"]
                or shader_manifest["inputs"]["toolchain"]["lock_sha256"] != digest(lock_data)):
            raise ValueError("Shader artifact generation changed or belongs to a different dependency lock")
        payload[SHADER_BUNDLE] = bundle
        payload[SHADER_MANIFEST] = canonical(shader_manifest)
    sample = ("Run bin\\Runtime.exe --cube --graphics-ms 1000 --device <Vulkan UUID> --validation off\n"
              "The sample presents a textured cube using bin\\assets\\cube.sharkshader.\n"
              if shader_record is not None else
              "Run bin\\Runtime.exe --graphics --graphics-ms 1000 --device <Vulkan UUID> --validation off\n"
              "The sample presents a clear color.\n")
    payload["README.txt"] = (
        "Shark local Windows x64 Runtime deployment fixture\n\n"
        "Requires the current Windows 11 PC, an interactive desktop and installed vendor Vulkan driver.\n"
        + sample +
        "It has no scene, game, editor UI or authoring API.\n"
        "No Python, compiler, Vulkan SDK, source checkout or Editor is required by Runtime.\n"
        "Validation layers, shader compilers and source payloads are absent. Native correctness\n"
        "qualification is a separate required-validation fixture; this launch is diagnostic.\n"
        "This unsigned package is local deployment evidence, not a game export or release.\n"
        "Licenses are included. Update the pinned app-local CRT by rebuilding the package.\n"
    ).encode("utf-8")
    for name, binary in binaries.items():
        regular_file(binary, digest(payload[name]))
    regular_file(ROOT / "dependencies.lock.json", digest(lock_data))
    manifest = {"schema_version": 1, "checkpoint": "06c" if shaders is not None else "05c", "kind": "runtime-deployment-fixture",
                "status": "PASS", "qualification": "NOT_RUN", "platform": "windows-x86_64",
                "timestamp_utc": datetime.now(timezone.utc).isoformat(), "configuration": "Release",
                "dependency_lock_sha256": digest(lock_data), "dependencies": dependencies,
                "commands": commands, "build_directory": str(build),
                "cache_sha256": sha256(build / "CMakeCache.txt"),
                "builder_sha256": sha256(Path(__file__))}
    if shader_record is not None:
        manifest["shader_bundle"] = shader_record
    return publish(output.absolute(), payload, manifest)


def inspect_package(directory: Path, expected_manifest: str) -> dict:
    manifest_data = regular_file(directory / "package-manifest.json", expected_manifest, 8 * 1024 * 1024)
    manifest = json.loads(manifest_data, object_pairs_hook=unique_object,
                          parse_constant=reject_constant, parse_float=finite_float)
    if not isinstance(manifest, dict):
        raise ValueError("Runtime package manifest must be an object")
    if (manifest.get("schema_version") != 1 or manifest.get("kind") != "runtime-deployment-fixture"
            or manifest.get("status") != "PASS" or manifest.get("qualification") != "NOT_RUN"
            or not isinstance(manifest.get("files"), dict)):
        raise ValueError("Invalid Runtime package manifest")
    files = manifest["files"]
    if not 1 <= len(files) <= MAX_FILES:
        raise ValueError("Invalid package file count")
    actual = set()
    for path in directory.rglob("*"):
        assert_no_links(path)
        if path.is_file():
            actual.add(path.relative_to(directory).as_posix())
    if actual != set(files) | {"package-manifest.json"}:
        raise ValueError("Package inventory differs from its manifest")
    native = {name for name in files if Path(name).suffix.lower() in {".exe", ".dll"}}
    if native != BINARIES:
        raise ValueError("Runtime package contains unexpected or missing native payload")
    total = 0
    has_shaders = "shader_bundle" in manifest
    if has_shaders != (SHADER_BUNDLE in files and SHADER_MANIFEST in files):
        raise ValueError("Runtime shader bundle declaration and payload closure differ")
    for name, record in files.items():
        relative_name(name)
        if (not isinstance(record, dict) or set(record) != {"sha256", "bytes"}
                or not isinstance(record["sha256"], str) or not re.fullmatch("[0-9a-f]{64}", record["sha256"])
                or type(record["bytes"]) is not int or not 0 <= record["bytes"] <= MAX_FILE):
            raise ValueError(f"Invalid Runtime package file record: {name}")
        if (name not in BINARIES | {"README.txt", "provenance/dependencies.lock.json",
                                   "provenance/dependency-closure.json", "provenance/toolchain.json",
                                   "provenance/crt-manifest.json"}
                and not (has_shaders and name in {SHADER_BUNDLE, SHADER_MANIFEST})
                and not name.startswith("licenses/")):
            raise ValueError(f"Unexpected Runtime source or auxiliary payload: {name}")
        data = regular_file(directory / name, record["sha256"])
        if len(data) != record["bytes"]:
            raise ValueError(f"Package size mismatch: {name}")
        total += len(data)
    if total > MAX_PACKAGE:
        raise ValueError("Package exceeds byte budget")
    for name in ("EnTT", "GLFW", "NVRHI", "VulkanHeaders", "VulkanLoader", "MicrosoftVC"):
        if not any(path.startswith(f"licenses/{name}/") for path in files):
            raise ValueError(f"Package lacks {name} license notices")
    if has_shaders:
        bundle = inspect_bundle(regular_file(directory / SHADER_BUNDLE))
        record = manifest["shader_bundle"]
        if not isinstance(record, dict) or set(record) != set(bundle) | {"generation", "artifact_manifest_sha256"}:
            raise ValueError("Invalid packaged shader bundle metadata")
        if {key: record[key] for key in bundle} != bundle:
            raise ValueError("Packaged shader bundle metadata differs from its payload")
        source = regular_file(directory / SHADER_MANIFEST, record["artifact_manifest_sha256"])
        provenance = json.loads(source, object_pairs_hook=unique_object,
                                parse_constant=reject_constant, parse_float=finite_float)
        if not isinstance(provenance, dict) or not isinstance(provenance.get("inputs"), dict):
            raise ValueError("Packaged shader generation provenance is invalid")
        inputs = provenance["inputs"]
        if (manifest.get("checkpoint") != "06c" or provenance.get("schema_version") != 1
                or not isinstance(inputs.get("toolchain"), dict)
                or provenance.get("generation") != record["generation"]
                or digest(canonical(inputs)) != record["generation"]
                or inputs["toolchain"].get("lock_sha256") != manifest.get("dependency_lock_sha256")):
            raise ValueError("Packaged shader generation provenance differs")
        rows = provenance.get("shaders")
        if not isinstance(rows, list) or any(not isinstance(row, dict) for row in rows):
            raise ValueError("Packaged shader generation inventory is invalid")
        for shader in bundle["shaders"]:
            matches = [row for row in rows if row.get("id") == shader["id"]]
            if (len(matches) != 1 or any(matches[0].get(key) != value for key, value in shader.items())
                    or matches[0].get("file") != f"generations/{record['generation']}/{shader['id']}.spv"):
                raise ValueError("Packaged shader dependency closure differs from bundle")
    return manifest


def completion(stdout: str, device: str, shader_hash: str | None = None) -> dict:
    lines = [line for line in stdout.splitlines() if line.strip()]
    if len(lines) != 1:
        raise ValueError("Expected exactly one Runtime completion record")
    row = json.loads(lines[0], object_pairs_hook=unique_object,
                     parse_constant=reject_constant, parse_float=finite_float)
    if not isinstance(row, dict):
        raise ValueError("Runtime completion must be an object")
    keys = {"event", "validation", "adapter_uuid", "submitted", "presented", "completed",
            "generations", "retired_generations", "synchronization", "qualification"}
    if shader_hash is not None:
        keys |= {"draw", "shader_bundle_sha256"}
    if (set(row) != keys or row["event"] != ("cube-sample-complete" if shader_hash else "graphics-sample-complete") or row["adapter_uuid"] != device
            or row["validation"] != "off" or row["qualification"] != "NOT_RUN"
            or row["synchronization"] not in {"present_fence", "host_completion"}):
        raise ValueError("Runtime completion identity or validation policy differs")
    if shader_hash is not None and (row["draw"] != "textured-cube" or row["shader_bundle_sha256"] != shader_hash):
        raise ValueError("Runtime completion shader bundle identity differs")
    for name in ("submitted", "presented", "completed", "generations", "retired_generations"):
        if type(row[name]) is not int or row[name] <= 0:
            raise ValueError(f"Runtime completion requires positive integer {name}")
    if (row["submitted"] != row["completed"] or row["presented"] > row["submitted"]
            or row["generations"] != row["retired_generations"]):
        raise ValueError("Runtime completion has unfinished GPU ownership")
    return row


def module_errors(modules: list[str], directory: Path, system: Path) -> list[str]:
    errors, package_modules = [], set()
    if not modules or len(modules) > 512:
        return ["Missing or excessive Runtime module inventory"]
    for name in modules:
        if not isinstance(name, str) or not Path(name).is_absolute():
            errors.append("Runtime module path must be absolute")
            continue
        path = Path(name).resolve()
        if path.parent == (directory / "bin").resolve():
            package_modules.add("bin/" + path.name.lower())
        elif not path.is_relative_to(system.resolve()):
            errors.append(f"Module loaded outside package/Windows driver closure: {path}")
        if re.search(r"(?:editor|enginetools|imgui|imguizmo|cgltf|dxcompiler|dxil|glslang|shaderc)", path.name, re.I):
            errors.append(f"Forbidden Runtime module: {path}")
    if package_modules != {name.lower() for name in BINARIES}:
        errors.append("Observed app-local modules differ from Runtime/loader/release CRT closure")
    return errors


def check_package(directory: Path, expected_manifest: str, devices: list[str], output: Path) -> dict:
    assert_no_links(directory)
    directory = directory.resolve()
    if directory.is_relative_to(ROOT.resolve()):
        raise ValueError("Deployment check requires a relocation outside the source checkout")
    if not (" " in str(directory) and any(ord(char) > 127 for char in str(directory))):
        raise ValueError("Deployment check requires a Unicode and space relocation path")
    if len(devices) != 2 or len(set(devices)) != 2 or any(not re.fullmatch("[0-9a-f]{32}", item) for item in devices):
        raise ValueError("Deployment check requires two distinct explicit GPU UUIDs")
    manifest = inspect_package(directory, expected_manifest)
    output.mkdir(parents=True, exist_ok=False)
    cube = manifest.get("shader_bundle")
    summary = {"schema_version": 1, "checkpoint": "06c" if cube else "05c", "status": "FAIL", "qualification": "NOT_RUN",
               "scope": "Local relocated Runtime dependency and launch evidence; no independent clean machine or native correctness qualification",
               "package_directory": str(directory), "manifest_sha256": expected_manifest,
               "validation": "off", "runs": [], "errors": [],
               "limits": "Development tools remain installed; child PATH/environment and observed modules are isolated. Module polling observes loaded modules, not transient unload history."}
    try:
        from windows_unelevated import process_security, _assert_standard
        summary["process_security"] = process_security()
        _assert_standard(summary["process_security"])
        system = Path(os.environ["SystemRoot"])
        powershell = system / "System32/WindowsPowerShell/v1.0/powershell.exe"
        helper = ROOT / "tools/runtime_package/Run-IsolatedRuntime.ps1"
        summary["helper_sha256"] = sha256(helper)
        summary["lock_sha256"] = manifest["dependency_lock_sha256"]
        for device in devices:
            command = [str(powershell), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(helper),
                       "-Package", str(directory), "-Device", device]
            if cube:
                command.append("-Cube")
            result = run_process(command, cwd=directory, timeout=30)
            (output / (device + ".stdout.json")).write_text(result.stdout, encoding="utf-8")
            (output / (device + ".stderr.log")).write_text(result.stderr, encoding="utf-8")
            row = {"device": device, "process": asdict(result), "status": "FAIL"}
            summary["runs"].append(row)
            if result.returncode or result.timed_out or result.cleanup_errors or result.stderr.strip():
                raise ValueError(f"Isolated Runtime launcher failed for {device}")
            report = json.loads(result.stdout.lstrip("\ufeff"), object_pairs_hook=unique_object,
                                parse_constant=reject_constant, parse_float=finite_float)
            row["launch"] = report
            if report["exit_code"] != 0 or report["timed_out"] or report["stderr"].strip():
                raise ValueError(f"Runtime process failed for {device}")
            row["completion"] = completion(report["stdout"], device, cube["sha256"] if cube else None)
            errors = module_errors(report["modules"], directory, system)
            if errors:
                raise ValueError("; ".join(errors))
            inspect_package(directory, expected_manifest)
            row["status"] = "PASS"
        if sha256(helper) != summary["helper_sha256"]:
            raise ValueError("Isolated launch helper changed during verification")
        summary["status"] = "PASS"
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        summary["errors"].append(str(error))
    finally:
        (output / "summary.json").write_bytes(json_bytes(summary))
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    build = commands.add_parser("build")
    for name in ("build-dir", "crt-dir", "output"):
        build.add_argument("--" + name, type=Path, required=True)
    build.add_argument("--shaders", type=Path, help="Verified offline artifact cache to package the cube sample")
    check = commands.add_parser("check")
    check.add_argument("--package", type=Path, required=True)
    check.add_argument("--manifest-sha256", required=True)
    check.add_argument("--device", action="append", required=True)
    check.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if os.name != "nt":
        print(json.dumps({"status": "NOT_RUN", "reason": "Requires native Windows x64"}))
        return 3
    try:
        if args.mode == "build":
            manifest = build_package(args.build_dir, args.crt_dir, args.output, args.shaders)
            report = {"status": "PASS", "qualification": "NOT_RUN", "files": len(manifest["files"]),
                      "package_directory": str(args.output.absolute()),
                      "manifest_sha256": sha256(args.output / "package-manifest.json")}
        else:
            report = check_package(args.package, args.manifest_sha256, args.device, args.output)
        print(json.dumps(report, indent=2))
        return 0 if report["status"] == "PASS" else 1
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        print(json.dumps({"status": "FAIL", "reason": str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
