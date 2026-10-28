"""Export Windows startup scenes and build/check diagnostic Runtime packages.

The build command requires an already current runtime-only Release build. The
diagnostic check command launches an explicit relocation outside the checkout, with a
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
import struct
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
from shader_pipeline import (bundle_payload, canonical, inspect_bundle, inspect_spirv,
                             verify as verify_shaders)
from core_dependencies import archive_files as core_archive_files, entry as core_dependency_entry

RUNTIME_CRT_NAMES = CRT_NAMES | {"msvcp140_2.dll"}
BINARIES = {"bin/Runtime.exe", "bin/vulkan-1.dll", *("bin/" + name for name in RUNTIME_CRT_NAMES)}
SHADER_BUNDLE = "bin/assets/cube.sharkshader"
SHADER_MANIFEST = "provenance/shader-manifest.json"
STARTUP = "bin/startup.sharkstart"
STARTUP_SCENE = "bin/content/startup.sharkscene"
SCENE_VERTEX = "bin/assets/scene-vs.spv"
SCENE_PIXEL = "bin/assets/scene-ps.spv"
CONTENT_ATTRIBUTION = "licenses/Content/attribution.json"
CONTENT_NOTICES = "licenses/Content/notices.json"
SCENE_SHADER_MANIFEST = "provenance/scene-shader-manifest.json"
SCENE_SHADERS = (
    {"id": "scene-unlit-vs", "entry": "vsScene", "stage": "vertex",
     "bindings": [{"kind": "constant_buffer", "slot": 0}]},
    {"id": "scene-unlit-ps", "entry": "psScene", "stage": "pixel", "bindings": []},
)
CONTENT_MAXIMUM = 32 * 1024 * 1024


def exact_record(value, fields: set[str], what: str) -> dict:
    if not isinstance(value, dict) or set(value) != fields:
        raise ValueError(f"Invalid {what} fields")
    return value


def content_id(value) -> str:
    if (not isinstance(value, str)
            or not re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}", value)):
        raise ValueError("Invalid client content UUID")
    return value


def content_text(value, maximum: int) -> str:
    if (not isinstance(value, str) or not 1 <= len(value.encode("utf-8")) <= maximum
            or any(ord(char) < 32 or 127 <= ord(char) <= 159 for char in value)):
        raise ValueError("Invalid client content text")
    return value


def file_identity(value, maximum: int) -> dict:
    exact_record(value, {"sha256", "bytes"}, "content file identity")
    if (not isinstance(value["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", value["sha256"])
            or type(value["bytes"]) is not int or not 1 <= value["bytes"] <= maximum):
        raise ValueError("Invalid or oversized content file identity")
    return value


def validate_content_record(record: dict) -> dict:
    exact_record(record, {"format", "schema_version", "project", "world", "scene", "camera", "name",
                          "target", "dirty", "files", "sources"}, "client export content")
    if (record["format"] != "shark.client-export-content" or type(record["schema_version"]) is not int
            or record["schema_version"] != 1 or type(record["dirty"]) is not bool):
        raise ValueError("Unsupported client export content version/state")
    for name in ("project", "world", "scene", "camera"):
        content_id(record[name])
    content_text(record["name"], 256)
    if not isinstance(record["target"], str) or not re.fullmatch(r"[a-z][a-z0-9._-]{0,63}", record["target"]):
        raise ValueError("Invalid client content target profile")
    files = record["files"]
    required = {STARTUP, STARTUP_SCENE, SCENE_VERTEX, SCENE_PIXEL}
    if not isinstance(files, dict) or not required <= files.keys() or len(files) > 130:
        raise ValueError("Incomplete or oversized client export file inventory")
    meshes, total = [], 0
    for path, identity in files.items():
        if path == STARTUP:
            maximum = 64 * 1024
        elif path in (SCENE_VERTEX, SCENE_PIXEL):
            maximum = 4 * 1024 * 1024
        elif path == STARTUP_SCENE:
            maximum = 4 * 1024 * 1024
        else:
            match = re.fullmatch(r"bin/content/([^/]+)\.sharkmesh", path)
            if match is None:
                raise ValueError("Unexpected source or auxiliary client content")
            asset = content_id(match[1])
            if asset in {"00000000-0000-4000-8000-000000000013", "00000000-0000-4000-8000-000000000014"}:
                raise ValueError("Built-in assets must not be external export payloads")
            meshes.append(asset)
            maximum = 2 * 1024 * 1024
        file_identity(identity, maximum)
        if path in (SCENE_VERTEX, SCENE_PIXEL) and (identity["bytes"] < 20 or identity["bytes"] % 4):
            raise ValueError("Invalid scene shader length")
        if path == STARTUP_SCENE or path not in required:
            total += identity["bytes"]
    if total > CONTENT_MAXIMUM:
        raise ValueError("Client scene and meshes exceed combined byte budget")
    sources = record["sources"]
    if not isinstance(sources, list) or len(sources) > 128:
        raise ValueError("Invalid client attribution inventory")
    source_ids = []
    for source in sources:
        exact_record(source, {"asset", "key", "origin", "license"}, "content attribution")
        source_ids.append(content_id(source["asset"]))
        if not isinstance(source["key"], str) or not re.fullmatch(r"[0-9a-f]{64}", source["key"]):
            raise ValueError("Invalid attribution cache key")
        content_text(source["origin"], 1024)
        content_text(source["license"], 128)
    if source_ids != sorted(set(source_ids)) or not set(meshes) <= set(source_ids):
        raise ValueError("Attribution must be unique, sorted and cover every exported mesh")
    return record


def validate_startup_descriptor(data: bytes, record: dict) -> None:
    """Inspect the independent runtime descriptor without source/Tools codecs."""
    if (len(data) < 44 or len(data) > 64 * 1024 or data[:8] != b"SHKSTRT1"
            or struct.unpack_from("<I", data, 8)[0] != 1 or digest(data[44:]) != data[12:44].hex()):
        raise ValueError("Invalid prepared startup framing/version/digest")
    cursor = 44

    def take(size):
        nonlocal cursor
        if size > len(data) - cursor:
            raise ValueError("Truncated prepared startup descriptor")
        value = data[cursor:cursor + size]
        cursor += size
        return value

    def integer():
        return struct.unpack("<I", take(4))[0]

    for name in ("project", "world", "scene", "camera"):
        if take(16) != uuid.UUID(record[name]).bytes:
            raise ValueError(f"Startup {name} identity differs from content")
    for expected in (record["name"], "windows-x86_64"):
        encoded = expected.encode("utf-8")
        if integer() != len(encoded) or take(len(encoded)) != encoded:
            raise ValueError("Startup name or binary target differs from content")

    def identity(path):
        expected = record["files"][path]
        if integer() != expected["bytes"] or take(32).hex() != expected["sha256"]:
            raise ValueError(f"Startup payload identity differs: {path}")

    for path in (STARTUP_SCENE, SCENE_VERTEX, SCENE_PIXEL):
        identity(path)
    meshes = sorted(path for path in record["files"] if path.endswith(".sharkmesh"))
    if integer() != len(meshes):
        raise ValueError("Startup mesh count differs from content")
    for path in meshes:
        asset = Path(path).stem
        if take(16) != uuid.UUID(asset).bytes:
            raise ValueError("Startup mesh identity/order differs from content")
        identity(path)
    if cursor != len(data):
        raise ValueError("Trailing prepared startup bytes")


def scene_shader_inputs(shaders: Path) -> tuple[Path, Path, dict]:
    shaders = shaders.absolute()
    provenance = verify_shaders(shaders)
    definitions = provenance["inputs"]["recipe"]["shaders"]
    if (len(definitions) != 2 or any({key: definition[key] for key in expected} != expected
                                    for definition, expected in zip(definitions, SCENE_SHADERS, strict=True))):
        raise ValueError("Shader generation does not implement scene-unlit-v1")
    rows = provenance["shaders"]
    return shaders / rows[0]["file"], shaders / rows[1]["file"], provenance


def content_attribution(record: dict) -> bytes:
    return canonical({"format": "shark.content-attribution", "schema_version": 1,
                      "project": record["project"], "sources": record["sources"]})


def content_notice_payload(record: dict, notices: dict[str, bytes] | None) -> bytes:
    notices = {} if notices is None else notices
    expected = {source["license"] for source in record["sources"]}
    if not isinstance(notices, dict) or set(notices) != expected:
        raise ValueError("Content notice inputs must exactly cover the used license declarations")
    entries, total = [], 0
    for license_id in sorted(expected):
        data = notices[license_id]
        if not isinstance(data, bytes) or not 1 <= len(data) <= 64 * 1024:
            raise ValueError("Each content notice requires 1..65536 UTF-8 bytes")
        total += len(data)
        if total > 1024 * 1024:
            raise ValueError("Content notices exceed their combined byte budget")
        text = data.decode("utf-8")
        if any((ord(char) < 32 and char not in "\t\r\n") or 127 <= ord(char) <= 159 for char in text):
            raise ValueError("Content notice contains non-text control characters")
        entries.append({"license": license_id, "sha256": digest(data), "bytes": len(data), "text": text})
    return canonical({"format": "shark.content-notices", "schema_version": 1, "entries": entries})


def inspect_content_notices(data: bytes, record: dict) -> None:
    document = json.loads(data, object_pairs_hook=unique_object,
                          parse_constant=reject_constant, parse_float=finite_float)
    exact_record(document, {"format", "schema_version", "entries"}, "content notices")
    if (document["format"] != "shark.content-notices" or type(document["schema_version"]) is not int
            or document["schema_version"] != 1 or not isinstance(document["entries"], list)
            or len(document["entries"]) > 128):
        raise ValueError("Invalid content notice version/inventory")
    notices = {}
    for entry in document["entries"]:
        exact_record(entry, {"license", "sha256", "bytes", "text"}, "content notice entry")
        content_text(entry["license"], 128)
        file_identity({key: entry[key] for key in ("sha256", "bytes")}, 64 * 1024)
        if not isinstance(entry["text"], str) or entry["license"] in notices:
            raise ValueError("Content notice text is invalid or its license is duplicated")
        text = entry["text"].encode("utf-8")
        if len(text) != entry["bytes"] or digest(text) != entry["sha256"]:
            raise ValueError("Content notice bytes/hash differ from the embedded text")
        notices[entry["license"]] = text
    if canonical(document) != content_notice_payload(record, notices):
        raise ValueError("Content notice inventory must be complete and sorted")


def assemble_scene_package(payload: dict[str, bytes], manifest: dict, content_directory: Path,
                           content_record: dict, shaders: Path, expected_generation: str | None = None,
                           *, content_notices: dict[str, bytes] | None = None
                           ) -> tuple[dict[str, bytes], dict]:
    validate_content_record(content_record)
    assert_no_links(content_directory)
    actual = set()
    for index, path in enumerate(content_directory.rglob("*")):
        if index >= 512:
            raise ValueError("Export worker content directory exceeds its entry budget")
        assert_no_links(path)
        if path.is_file():
            actual.add(path.relative_to(content_directory).as_posix())
    if actual != set(content_record["files"]):
        raise ValueError("Export worker content inventory differs from its declaration")
    content = {name: regular_file(content_directory / name, row["sha256"], row["bytes"])
               for name, row in content_record["files"].items()}
    if any(len(content[name]) != row["bytes"] for name, row in content_record["files"].items()):
        raise ValueError("Export worker content size differs")
    validate_startup_descriptor(content[STARTUP], content_record)
    _, _, provenance = scene_shader_inputs(shaders)
    if (provenance["inputs"]["toolchain"]["lock_sha256"] != manifest["dependency_lock_sha256"]
            or expected_generation is not None and provenance["generation"] != expected_generation):
        raise ValueError("Scene shader generation changed or belongs to another dependency lock")
    for path, row in zip((SCENE_VERTEX, SCENE_PIXEL), provenance["shaders"], strict=True):
        if content_record["files"][path] != {key: row[key] for key in ("bytes", "sha256")}:
            raise ValueError("Export worker scene shaders differ from the verified generation")
    notice_payload = content_notice_payload(content_record, content_notices)
    if set(payload) & (set(content) | {CONTENT_ATTRIBUTION, CONTENT_NOTICES, SCENE_SHADER_MANIFEST}):
        raise ValueError("Client export content collides with runtime package inputs")
    result = {**payload, **content, CONTENT_ATTRIBUTION: content_attribution(content_record),
              CONTENT_NOTICES: notice_payload,
              SCENE_SHADER_MANIFEST: canonical(provenance)}
    result["README.txt"] = (
        "Shark early standalone Windows x64 scene export\n\n"
        "Run bin\\Runtime.exe from any working directory. Its adjacent startup.sharkstart\n"
        "selects this package's prepared scene, camera, meshes and offline scene shaders.\n"
        "Requires this Windows 11 PC, an interactive desktop and installed vendor Vulkan driver.\n"
        "No Python, Editor, compiler, Vulkan SDK or source checkout is required at launch.\n"
        "Content is temporary opaque unlit development data, not final cooked content.\n"
        "This unsigned client scene export is not a dedicated server or MMO release.\n"
        "Engine/library and used-source attributions are under licenses/.\n"
    ).encode("utf-8")
    result_manifest = {**manifest, "kind": "runtime-scene-export", "checkpoint": "14b",
                       "content": content_record,
                       "scene_shaders": {"profile": "scene-unlit-v1", "generation": provenance["generation"],
                                         "artifact_manifest_sha256": digest(canonical(provenance))}}
    return result, result_manifest


def inspect_scene_package(directory: Path, manifest: dict) -> None:
    if (manifest.get("checkpoint") != "14b" or manifest.get("platform") != "windows-x86_64"
            or manifest.get("configuration") != "Release" or "shader_bundle" in manifest):
        raise ValueError("Invalid standalone scene export platform/configuration")
    content = validate_content_record(manifest.get("content"))
    files = manifest["files"]
    if any(files.get(path) != identity for path, identity in content["files"].items()):
        raise ValueError("Package and content inventories differ")
    validate_startup_descriptor(regular_file(directory / STARTUP, maximum=64 * 1024), content)
    if regular_file(directory / CONTENT_ATTRIBUTION, maximum=256 * 1024) != content_attribution(content):
        raise ValueError("Packaged attribution differs from the complete used content closure")
    inspect_content_notices(regular_file(directory / CONTENT_NOTICES, maximum=3 * 1024 * 1024), content)
    shader_record = exact_record(manifest.get("scene_shaders"),
                                 {"profile", "generation", "artifact_manifest_sha256"}, "scene shader record")
    if (shader_record["profile"] != "scene-unlit-v1"
            or any(not isinstance(shader_record[key], str) or not re.fullmatch(r"[0-9a-f]{64}", shader_record[key])
                   for key in ("generation", "artifact_manifest_sha256"))):
        raise ValueError("Invalid scene shader profile or digests")
    raw = regular_file(directory / SCENE_SHADER_MANIFEST, shader_record["artifact_manifest_sha256"], 1024 * 1024)
    provenance = json.loads(raw, object_pairs_hook=unique_object,
                            parse_constant=reject_constant, parse_float=finite_float)
    exact_record(provenance, {"schema_version", "convention", "generation", "inputs", "shaders"}, "scene shader provenance")
    inputs = provenance["inputs"]
    if (type(provenance["schema_version"]) is not int or provenance["schema_version"] != 1
            or provenance["convention"] != "shark-vulkan-1"
            or not isinstance(inputs, dict) or provenance["generation"] != shader_record["generation"]
            or digest(canonical(inputs)) != shader_record["generation"]
            or not isinstance(inputs.get("toolchain"), dict)
            or inputs["toolchain"].get("lock_sha256") != manifest.get("dependency_lock_sha256")):
        raise ValueError("Scene shader generation provenance differs")
    recipe = inputs.get("recipe")
    if not isinstance(recipe, dict) or not isinstance(recipe.get("shaders"), list) or len(recipe["shaders"]) != 2:
        raise ValueError("Invalid scene shader recipe")
    rows = provenance["shaders"]
    if not isinstance(rows, list) or len(rows) != 2:
        raise ValueError("Invalid scene shader inventory")
    for definition, expected, row, path in zip(recipe["shaders"], SCENE_SHADERS, rows,
                                               (SCENE_VERTEX, SCENE_PIXEL), strict=True):
        if (not isinstance(definition, dict)
                or {key: definition.get(key) for key in expected} != expected):
            raise ValueError("Packaged shader recipe differs from scene-unlit-v1")
        exact_record(row, {"id", "file", "sha256", "bytes"}, "scene shader artifact")
        if (row["id"] != expected["id"]
                or row["file"] != f"generations/{shader_record['generation']}/{expected['id']}.spv"
                or {key: row[key] for key in ("bytes", "sha256")} != content["files"][path]):
            raise ValueError("Packaged scene shader dependency closure differs")
        inspect_spirv(regular_file(directory / path, maximum=4 * 1024 * 1024), expected)


def scene_license_paths() -> set[str]:
    lock = read_json(ROOT / "dependencies.lock.json")
    paths = {f"licenses/{name}/{path}"
             for name in ("EnTT", "GLM", "MbedTLS", "GLFW", "NVRHI", "VulkanHeaders", "VulkanLoader")
             for path in lock["dependencies"][name]["license_files"]}
    paths.update("licenses/MicrosoftVC/" + row["destination"]
                 for row in lock["graphics_distribution"]["WindowsCRT"]["license_files"])
    return paths | {CONTENT_ATTRIBUTION, CONTENT_NOTICES}


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
                "SHARK_BUILD_CLIENT": "ON", "SHARK_BUILD_EDITOR": "OFF", "SHARK_BUILD_TOOLS": "OFF",
                "SHARK_BUILD_TESTS": "OFF", "SHARK_BUILD_SERVER": "OFF",
                "SHARK_BUILD_GRAPHICS_PROBE": "OFF", "SHARK_NATIVE_WINDOW": "ON",
                "SHARK_NATIVE_GRAPHICS": "ON", "SHARK_INJECT_TEST_FAILURE": "OFF"}
    for key, value in expected.items():
        if values.get(key) != value:
            raise ValueError(f"Runtime package requires {key}={value}")
    if Path(values.get("CMAKE_HOME_DIRECTORY", "")).resolve() != ROOT.resolve():
        raise ValueError("Runtime build belongs to a different source checkout")
    if (build / "_deps/sqlite-src").exists() or values.get("SHARK_SQLITE_SOURCE_DIR"):
        raise ValueError("Runtime build must not contain Server SQLite population")
    return values


def production_notices(build: Path, lock: dict) -> tuple[dict[str, bytes], dict]:
    payload, records = {}, {}
    cache = cache_values(build)
    for name in ("EnTT", "GLM", "MbedTLS", "GLFW", "NVRHI", "VulkanHeaders", "VulkanLoader"):
        entry = lock["dependencies"][name]
        lower = name.lower()
        archive = build / f"_deps/{lower}-subbuild/{lower}-populate-prefix/src" / entry["url"].rsplit("/", 1)[-1]
        if name in ("EnTT", "GLM", "MbedTLS"):
            cache_key = {"EnTT": "SHARK_EnTT_ARCHIVE", "GLM": "SHARK_GLM_ARCHIVE_INPUT",
                         "MbedTLS": "SHARK_MbedTLS_ARCHIVE_INPUT"}[name]
            archive = Path(cache.get(cache_key, str(archive)))
        if name == "MbedTLS":
            # The official bz2 archive includes reviewed unused test symlinks;
            # use the dependency owner's exact-file verifier, not the graphics
            # tar.gz reader or the mutable populated build tree.
            verified = core_archive_files(archive, entry, name)
            notices = {path: verified[path] for path in entry["license_files"]}
        else:
            notices = archive_notices(archive, entry)
        if not notices:
            raise ValueError(f"Missing redistribution notices for {name}")
        payload.update({f"licenses/{name}/{path}": data for path, data in notices.items()})
        records[name] = {"archive_sha256": entry["sha256"], "license": entry["license"],
                         "license_files": {path: digest(data) for path, data in notices.items()}}
    return payload, records


def required_core_notices() -> dict[str, tuple[str, ...]]:
    return {"EnTT": ("LICENSE",), "GLM": ("copying.txt",),
            "MbedTLS": tuple(core_dependency_entry(ROOT, "MbedTLS")["license_files"])}


def runtime_crt_manifest(manifest: dict) -> dict:
    additional = manifest.get("runtime_additional_files")
    if (not isinstance(additional, dict) or set(additional) != RUNTIME_CRT_NAMES - CRT_NAMES
            or not isinstance(manifest.get("files"), dict) or set(manifest["files"]) != CRT_NAMES):
        raise ValueError("Runtime CRT receipt requires its exact base and additional DLL closure")
    return {**manifest, "files": {**manifest["files"], **additional}}


def publish(output: Path, payload: dict[str, bytes], manifest: dict, before_publish=None) -> dict:
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
        if before_publish is not None:
            before_publish()
        staging.rename(output)
    finally:
        if staging.exists():
            assert_no_links(staging)
            if staging.resolve().parent != output.parent.resolve():
                raise ValueError("Refusing cleanup outside owned package staging parent")
            shutil.rmtree(staging)
    return manifest


def collect_runtime_payload(build: Path, crt_dir: Path) -> tuple[dict[str, bytes], dict]:
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
    binaries, payload, crt = crt_inputs(crt_dir, runtime_crt_manifest(lock["graphics_distribution"]["WindowsCRT"]),
                                       required_names=RUNTIME_CRT_NAMES)
    metadata = {name: windows_version_info(crt_dir / name) for name in sorted(RUNTIME_CRT_NAMES)}
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
    for name, binary in binaries.items():
        regular_file(binary, digest(payload[name]))
    regular_file(ROOT / "dependencies.lock.json", digest(lock_data))
    manifest = {"schema_version": 1, "checkpoint": "05c", "kind": "runtime-deployment-fixture",
                "status": "PASS", "qualification": "NOT_RUN", "platform": "windows-x86_64",
                "timestamp_utc": datetime.now(timezone.utc).isoformat(), "configuration": "Release",
                "dependency_lock_sha256": digest(lock_data), "dependencies": dependencies,
                "commands": commands, "build_directory": str(build),
                "cache_sha256": sha256(build / "CMakeCache.txt"),
                "builder_sha256": sha256(Path(__file__))}
    return payload, manifest


def build_package(build: Path, crt_dir: Path, output: Path, shaders: Path | None = None) -> dict:
    payload, manifest = collect_runtime_payload(build, crt_dir)
    shader_record = None
    if shaders is not None:
        bundle, shader_record = bundle_payload(shaders.absolute())
        shader_manifest = verify_shaders(shaders.absolute())
        if (digest(canonical(shader_manifest)) != shader_record["artifact_manifest_sha256"]
                or shader_manifest["inputs"]["toolchain"]["lock_sha256"] != manifest["dependency_lock_sha256"]):
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
    if shader_record is not None:
        manifest["checkpoint"] = "06c"
        manifest["shader_bundle"] = shader_record
    return publish(output.absolute(), payload, manifest)


def inspect_package(directory: Path, expected_manifest: str) -> dict:
    manifest_data = regular_file(directory / "package-manifest.json", expected_manifest, 8 * 1024 * 1024)
    manifest = json.loads(manifest_data, object_pairs_hook=unique_object,
                          parse_constant=reject_constant, parse_float=finite_float)
    if not isinstance(manifest, dict):
        raise ValueError("Runtime package manifest must be an object")
    if (manifest.get("schema_version") != 1 or manifest.get("kind") not in {"runtime-deployment-fixture", "runtime-scene-export"}
            or manifest.get("status") != "PASS" or manifest.get("qualification") != "NOT_RUN"
            or not isinstance(manifest.get("files"), dict)):
        raise ValueError("Invalid Runtime package manifest")
    files = manifest["files"]
    if any(name.casefold().startswith("licenses/sqlite/") for name in files):
        raise ValueError("Server SQLite notice must not enter the Runtime package")
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
    scene_export = manifest["kind"] == "runtime-scene-export"
    scene_paths = set()
    if scene_export:
        scene_paths = set(validate_content_record(manifest.get("content"))["files"]) | {SCENE_SHADER_MANIFEST}
        if {name for name in files if name.startswith("licenses/")} != scene_license_paths():
            raise ValueError("Scene export license inventory differs from required notices and attribution")
    elif "content" in manifest or "scene_shaders" in manifest:
        raise ValueError("Scene content cannot be declared by a diagnostic Runtime package")
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
                and name not in scene_paths
                and not name.startswith("licenses/")):
            raise ValueError(f"Unexpected Runtime source or auxiliary payload: {name}")
        data = regular_file(directory / name, record["sha256"])
        if len(data) != record["bytes"]:
            raise ValueError(f"Package size mismatch: {name}")
        total += len(data)
    if total > MAX_PACKAGE:
        raise ValueError("Package exceeds byte budget")
    for name, required in required_core_notices().items():
        expected = {f"licenses/{name}/{path}" for path in required}
        if not expected <= files.keys():
            raise ValueError(f"Package lacks {name} license notices: {sorted(expected - files.keys())}")
        if name == "MbedTLS" and {path for path in files if path.startswith("licenses/MbedTLS/")} != expected:
            raise ValueError("Unexpected MbedTLS notice or test credential payload")
    for name in ("EnTT", "GLM", "GLFW", "NVRHI", "VulkanHeaders", "VulkanLoader", "MicrosoftVC"):
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
    if scene_export:
        inspect_scene_package(directory, manifest)
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
    if manifest["kind"] == "runtime-scene-export":
        raise ValueError("Scene export deployment acceptance belongs to 14c; the diagnostic checker cannot qualify it")
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


class StructuredParser(argparse.ArgumentParser):
    def error(self, message):
        raise ValueError(message)


def main() -> int:
    parser = StructuredParser(description=__doc__)
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
    export = commands.add_parser("export", help="Export a prepared startup scene for Windows x64")
    for name in ("build-dir", "crt-dir", "output", "shaders", "exporter", "project"):
        export.add_argument("--" + name, type=Path, required=True)
    for name in ("camera", "content-target", "target"):
        export.add_argument("--" + name, required=True)
    export.add_argument("--sidecar", action="append", default=[],
                        help="Explicit project-relative asset sidecar; repeat for the complete inventory")
    export.add_argument("--content-notice", action="append", default=[], metavar="LICENSE=PATH",
                        help="Full UTF-8 redistribution notice for each used asset license identifier")
    try:
        args = parser.parse_args()
        if args.mode == "export":
            from export_runtime import export_package
            report = export_package(args)
            print(json.dumps(report, ensure_ascii=True, indent=2))
            return 0 if report["status"] == "PASS" else (1 if report["published"] else 2)
        if os.name != "nt":
            print(json.dumps({"status": "NOT_RUN", "reason": "Requires native Windows x64"}))
            return 3
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
        if "export" in sys.argv[1:2]:
            print(json.dumps({"format": "shark.client-export", "schema_version": 1,
                              "status": "ERROR", "published": False, "qualification": "NOT_RUN",
                              "renderer_initialized": False, "artifacts": [],
                              "error": {"code": "invalid_argument", "message": str(error)}}))
            return 2
        print(json.dumps({"status": "FAIL", "reason": str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
