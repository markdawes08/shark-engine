"""Bounded, offline Shark shader recipes and immutable SPIR-V generations.

This module is build/cook tooling. It is never imported or linked by Runtime.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import sys
import uuid

from graphics_probe import checked_file, read_json, sha256
from prepare_graphics_tools import native_host, prepare
from process_runner import run_process

ROOT = Path(__file__).resolve().parents[1]
CONVENTION = "shark-vulkan-1"
STAGES = {"vertex": ("vs_6_0", 0), "pixel": ("ps_6_0", 4), "compute": ("cs_6_0", 5)}
BINDINGS = {"texture": 0, "sampler": 128, "constant_buffer": 256, "storage": 384}
FLAGS = ["-spirv", "-fspv-target-env=vulkan1.3", "-O3", "-Ges", "-WX", "-Zpc"]
MAX_SOURCE = 1024 * 1024
MAX_TOTAL_SOURCE = 4 * 1024 * 1024
MAX_BINARY = 4 * 1024 * 1024
MAX_FILES = 64
MAX_SHADERS = 32
HASH = re.compile(r"[0-9a-f]{64}\Z")
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z_0-9]{0,63}\Z")
ASSET_ID = re.compile(r"[a-z][a-z0-9_-]{0,63}\Z")
BUNDLE_MAGIC = b"SHRKSHD\0"
BUNDLE_HEADER = struct.Struct("<8sIIII32s32s")
BUNDLE_SHADERS = (
    {"id": "textured-cube-vs", "entry": "vsMain", "stage": "vertex",
     "bindings": [{"kind": "constant_buffer", "slot": 0}]},
    {"id": "textured-cube-ps", "entry": "psMain", "stage": "pixel",
     "bindings": [{"kind": "texture", "slot": 0}, {"kind": "sampler", "slot": 0}]},
)


class ShaderError(ValueError):
    def __init__(self, code: str, message: str, *, path: str = "") -> None:
        super().__init__(message)
        self.code, self.path = code, path


def fail(code: str, message: str, path: object = "") -> None:
    raise ShaderError(code, message, path=str(path))


def canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def exact(value: object, fields: set[str], what: str) -> dict:
    if not isinstance(value, dict) or set(value) != fields:
        fail("shader.schema", f"{what} requires exactly: {', '.join(sorted(fields))}")
    return value


def relative(value: object) -> str:
    if (not isinstance(value, str) or not value or len(value) > 240
            or any(ord(char) < 32 for char in value) or any(char in value for char in "\\:*?\"<>|")):
        fail("shader.path", "Expected a normalized relative portable path", value)
    parsed = PurePosixPath(value)
    if parsed.is_absolute() or any(part in {".", "..", ""} for part in value.split("/")):
        fail("shader.path", "Paths must stay inside their source/artifact root", value)
    if any(part.endswith((" ", ".")) for part in parsed.parts):
        fail("shader.path", "Path contains an ambiguous Windows component", value)
    if any(re.fullmatch(r"(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?", part) for part in parsed.parts):
        fail("shader.path", "Reserved Windows device names are not supported", value)
    return value


def contained(root: Path, name: str) -> Path:
    relative(name)
    target = root / name
    for component in (target, *target.parents):
        if component == root.parent:
            break
        if component.is_symlink() or component.is_junction():
            fail("shader.path", "Links and junctions are not supported shader inputs", target)
    if not target.resolve().is_relative_to(root.resolve()):
        fail("shader.path", "Path escapes its declared root", target)
    return target


def no_links(path: Path) -> None:
    for component in (path, *path.parents):
        if component.is_symlink() or component.is_junction():
            fail("shader.path", "Links and junctions are not supported shader paths", path)


def json_file(path: Path) -> dict:
    no_links(path)
    try:
        return read_json(path)
    except (OSError, ValueError, UnicodeError, RecursionError) as error:
        fail("shader.schema", str(error), path)


def recipe(path: Path) -> dict:
    return validate_recipe_value(json_file(path), str(path))


def validate_recipe_value(value: object, path: str = "") -> dict:
    data = exact(value, {"schema_version", "convention", "shaders"}, "Shader recipe")
    if type(data["schema_version"]) is not int or data["schema_version"] != 1 or data["convention"] != CONVENTION:
        fail("shader.version", "Only recipe version 1 / shark-vulkan-1 is supported", path)
    rows = data["shaders"]
    if not isinstance(rows, list) or not 1 <= len(rows) <= MAX_SHADERS:
        fail("shader.schema", "Recipe needs 1..32 shader entries", path)
    ids: set[str] = set()
    for row in rows:
        exact(row, {"id", "source", "entry", "stage", "bindings"}, "Shader entry")
        if not isinstance(row["id"], str) or not ASSET_ID.fullmatch(row["id"]) or row["id"] in ids:
            fail("shader.schema", "Shader IDs must be unique bounded lowercase identifiers", path)
        ids.add(row["id"])
        relative(row["source"])
        if (not isinstance(row["entry"], str) or not IDENTIFIER.fullmatch(row["entry"])
                or not isinstance(row["stage"], str) or row["stage"] not in STAGES):
            fail("shader.schema", "Unsupported shader stage or entry point", path)
        bindings = row["bindings"]
        if not isinstance(bindings, list) or len(bindings) > 64:
            fail("shader.schema", "A shader may declare at most 64 bindings", path)
        seen = set()
        for binding in bindings:
            exact(binding, {"kind", "slot"}, "Binding")
            if (not isinstance(binding["kind"], str) or binding["kind"] not in BINDINGS
                    or type(binding["slot"]) is not int or not 0 <= binding["slot"] < 128):
                fail("shader.layout", "Bindings use set 0 and texture/sampler/constant_buffer/storage slots 0..127")
            number = BINDINGS[binding["kind"]] + binding["slot"]
            if number in seen:
                fail("shader.layout", "Duplicate shader binding")
            seen.add(number)
    return data


def directives(source: str, path: str) -> list[str]:
    # Remove comments while retaining quoted strings and line boundaries. A
    # deliberately small preprocessor contract prevents hidden/macro includes.
    cleaned = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*[\s\S]*?\*/',
                     lambda match: ("\n" * match[0].count("\n") if match[0].startswith(("//", "/*"))
                                    else match[0]), source)
    includes = []
    for line in cleaned.splitlines():
        if not line.lstrip().startswith("#"):
            continue
        match = re.fullmatch(r'\s*#\s*include\s+"([^"\n]+)"\s*', line)
        if not match:
            fail("shader.source", "Only literal quoted #include directives are supported; macros/conditionals are not", path)
        includes.append(relative(match[1]))
    return includes


def source_closure(root: Path, definition: dict) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    active: set[str] = set()
    folded: dict[str, str] = {}

    def visit(name: str, depth: int = 0) -> None:
        if name in active or depth > 16:
            fail("shader.source", "Shader include cycle or depth limit exceeded", name)
        if name in files:
            return
        if len(files) >= MAX_FILES:
            fail("shader.source", "Shader source file count exceeded", name)
        if name.casefold() in folded and folded[name.casefold()] != name:
            fail("shader.path", "Case-ambiguous shader paths are not portable", name)
        target = contained(root, name)
        if not target.is_file() or target.stat().st_size > MAX_SOURCE:
            fail("shader.source", "Missing shader source/include or file exceeds 1 MiB", name)
        data = target.read_bytes()
        if len(data) > MAX_SOURCE or sum(map(len, files.values())) + len(data) > MAX_TOTAL_SOURCE:
            fail("shader.source", "Shader source byte budget exceeded", name)
        try:
            source = data.decode("utf-8")
        except UnicodeError:
            fail("shader.source", "Shader source must be UTF-8", name)
        if "\0" in source or "\ufeff" in source or "\\\n" in source or "\\\r\n" in source:
            fail("shader.source", "NUL/BOM bytes and preprocessor line continuations are unsupported", name)
        if re.search(r"\b__(?:DATE|TIME|TIMESTAMP|FILE|BASE_FILE)__\b", source):
            fail("shader.source", "Time/path-dependent predefined macros are unsupported", name)
        files[name], folded[name.casefold()] = data, name
        active.add(name)
        for include in directives(source, name):
            visit((PurePosixPath(name).parent / include).as_posix(), depth + 1)
        active.remove(name)

    for shader in definition["shaders"]:
        visit(shader["source"])
    return dict(sorted(files.items()))


def inspect_spirv(binary: bytes, shader: dict) -> None:
    if not 20 <= len(binary) <= MAX_BINARY or len(binary) % 4:
        fail("shader.artifact", "SPIR-V byte length is outside its budget", shader["id"])
    words = struct.unpack(f"<{len(binary) // 4}I", binary)
    if (words[0] != 0x07230203 or words[1] not in {0x10000 + minor * 0x100 for minor in range(7)}
            or not 0 < words[3] <= 1_000_000 or words[4] != 0):
        fail("shader.artifact", "Invalid SPIR-V header", shader["id"])
    entries, bindings, sets = [], {}, {}
    cursor = 5
    while cursor < len(words):
        count, opcode = words[cursor] >> 16, words[cursor] & 0xFFFF
        if count == 0 or cursor + count > len(words):
            fail("shader.artifact", "Malformed SPIR-V instruction bounds", shader["id"])
        args = words[cursor + 1:cursor + count]
        if opcode == 15:
            if len(args) < 3:
                fail("shader.artifact", "Malformed SPIR-V entry point", shader["id"])
            raw = struct.pack(f"<{len(args) - 2}I", *args[2:])
            if b"\0" not in raw:
                fail("shader.artifact", "Unterminated SPIR-V entry point", shader["id"])
            try:
                entries.append((args[0], raw.split(b"\0", 1)[0].decode("utf-8")))
            except UnicodeError:
                fail("shader.artifact", "Invalid SPIR-V entry point encoding", shader["id"])
        elif opcode == 71 and len(args) >= 2 and args[1] in (33, 34):
            if len(args) != 3:
                fail("shader.layout", "Malformed descriptor decoration", shader["id"])
            target = bindings if args[1] == 33 else sets
            if args[0] in target:
                fail("shader.layout", "Duplicate descriptor decoration", shader["id"])
            target[args[0]] = args[2]
        cursor += count
    if entries != [(STAGES[shader["stage"]][1], shader["entry"])]:
        fail("shader.layout", "SPIR-V stage/entry point differs from the recipe", shader["id"])
    expected = {BINDINGS[item["kind"]] + item["slot"] for item in shader["bindings"]}
    if (bindings.keys() != sets.keys() or any(value != 0 for value in sets.values())
            or len(set(bindings.values())) != len(bindings) or set(bindings.values()) != expected):
        fail("shader.layout", "SPIR-V descriptor set/binding numbers differ from the recipe", shader["id"])


def tool_inputs(receipt: Path) -> tuple[Path, Path, dict]:
    lock_bytes = (ROOT / "dependencies.lock.json").read_bytes()
    lock = json.loads(lock_bytes)
    manifest = json_file(receipt)
    if (type(manifest.get("schema_version")) is not int or manifest["schema_version"] != 1
            or manifest.get("status") != "PASS" or manifest.get("dependency_lock_sha256") != digest(lock_bytes)):
        fail("shader.toolchain", "Validator receipt is not passing/current; prepare locked validation tooling", receipt)
    try:
        validation = manifest["tools"]["spirv_val"]
        validator = Path(validation["path"])
        if not validator.is_absolute():
            fail("shader.toolchain", "Validator path must be absolute", receipt)
        no_links(validator)
        checked_file(validator, validation["sha256"])
        revision = lock["graphics_validation"]["dependencies"]["SPIRVTools"]["commit"]
        if revision not in validation["version"]:
            fail("shader.toolchain", "Validator receipt lacks the locked source revision", receipt)
        host = native_host()
        if host not in lock["graphics_tools"]["DXC"]["artifacts"]:
            fail("shader.toolchain", "Offline shader tools support Windows x64 and Linux x64 only")
        compiler, provenance = prepare(lock["graphics_tools"]["DXC"], host,
                                       ROOT / ".tools/graphics/dxc", offline=True)
    except (OSError, ValueError, KeyError, TypeError) as error:
        if isinstance(error, ShaderError):
            raise
        fail("shader.toolchain", str(error), receipt)
    return compiler, validator, {"lock_sha256": digest(lock_bytes), "host": host,
                                 "compiler_archive_sha256": provenance["archive_sha256"],
                                 "compiler_files": provenance["files"], "validator_sha256": validation["sha256"],
                                 "validator_revision": revision}


def verify(output: Path) -> dict:
    no_links(output)
    manifest = exact(json_file(contained(output, "shader-manifest.json")),
                     {"schema_version", "convention", "generation", "inputs", "shaders"}, "Shader artifact manifest")
    if type(manifest["schema_version"]) is not int or manifest["schema_version"] != 1 or manifest["convention"] != CONVENTION:
        fail("shader.version", "Unsupported shader artifact version/convention")
    generation = manifest["generation"]
    if not isinstance(generation, str) or not HASH.fullmatch(generation):
        fail("shader.artifact", "Invalid shader generation identity")
    inputs = exact(manifest["inputs"], {"recipe", "sources", "toolchain", "flags", "pipeline_sha256"}, "Shader generation inputs")
    # Reuse recipe validation without reading current source files or tools.
    definition = inputs["recipe"]
    validate_recipe_value(definition)
    if inputs["flags"] != FLAGS or not isinstance(inputs["pipeline_sha256"], str) or not HASH.fullmatch(inputs["pipeline_sha256"]):
        fail("shader.artifact", "Artifact compiler convention/implementation identity is invalid")
    sources = inputs["sources"]
    if not isinstance(sources, dict) or not 1 <= len(sources) <= MAX_FILES:
        fail("shader.artifact", "Artifact dependency inventory is invalid")
    for name, record in sources.items():
        relative(name)
        exact(record, {"sha256", "bytes"}, "Source identity")
        if (not isinstance(record["sha256"], str) or not HASH.fullmatch(record["sha256"])
                or type(record["bytes"]) is not int or not 0 <= record["bytes"] <= MAX_SOURCE):
            fail("shader.artifact", "Artifact source identity is invalid", name)
    if (sum(row["bytes"] for row in sources.values()) > MAX_TOTAL_SOURCE
            or any(shader["source"] not in sources for shader in definition["shaders"])):
        fail("shader.artifact", "Artifact dependency inventory is incomplete or oversized")
    identity = exact(inputs["toolchain"], {"lock_sha256", "host", "compiler_archive_sha256", "compiler_files",
                                          "validator_sha256", "validator_revision"}, "Toolchain identity")
    for name in ("lock_sha256", "compiler_archive_sha256", "validator_sha256"):
        if not isinstance(identity[name], str) or not HASH.fullmatch(identity[name]):
            fail("shader.artifact", "Invalid toolchain digest")
    if (identity["host"] not in ("windows-x86_64", "linux-x86_64")
            or not isinstance(identity["validator_revision"], str)
            or not re.fullmatch(r"[0-9a-f]{40}", identity["validator_revision"])):
        fail("shader.artifact", "Invalid toolchain host/revision")
    compiler_files = identity["compiler_files"]
    if not isinstance(compiler_files, dict) or not 1 <= len(compiler_files) <= 16:
        fail("shader.artifact", "Invalid compiler file inventory")
    for name, expected in compiler_files.items():
        relative(name)
        if not isinstance(expected, str) or not HASH.fullmatch(expected):
            fail("shader.artifact", "Invalid compiler file digest", name)
    if generation != digest(canonical(inputs)):
        fail("shader.artifact", "Shader generation input digest differs")
    rows = manifest["shaders"]
    if not isinstance(rows, list) or len(rows) != len(definition["shaders"]):
        fail("shader.artifact", "Artifact shader inventory differs from recipe")
    generation_root = contained(output, f"generations/{generation}")
    if (not generation_root.is_dir()
            or {path.name for path in generation_root.iterdir()} != {shader["id"] + ".spv" for shader in definition["shaders"]}):
        fail("shader.artifact", "Immutable generation inventory differs from recipe", generation_root)
    for row, shader in zip(rows, definition["shaders"], strict=True):
        exact(row, {"id", "file", "sha256", "bytes"}, "Shader artifact")
        expected = f"generations/{generation}/{shader['id']}.spv"
        if row["id"] != shader["id"] or row["file"] != expected or type(row["bytes"]) is not int:
            fail("shader.artifact", "Shader artifact identity/path differs from recipe")
        target = contained(output, expected)
        try:
            checked_file(target, row["sha256"], maximum=MAX_BINARY)
            data = target.read_bytes()
        except (OSError, ValueError) as error:
            fail("shader.artifact", str(error), expected)
        if len(data) != row["bytes"]:
            fail("shader.artifact", "Shader artifact length differs", expected)
        inspect_spirv(data, shader)
    return manifest


def inspect_bundle(data: bytes) -> dict:
    """Check the fixed diagnostic bundle without sources, tools or a device.

    Runtime additionally checks the supported typed SPIR-V interface. These
    digests detect inconsistent payloads; they are not signatures.
    """
    if not BUNDLE_HEADER.size <= len(data) <= BUNDLE_HEADER.size + 2 * MAX_BINARY:
        fail("shader.bundle", "Shader bundle length is outside its bounded format")
    magic, version, vertex_size, pixel_size, reserved, vertex_hash, pixel_hash = BUNDLE_HEADER.unpack_from(data)
    if magic != BUNDLE_MAGIC or version != 1 or reserved != 0:
        fail("shader.bundle", "Unsupported shader bundle magic, version or reserved field")
    if (any(size < 20 or size > MAX_BINARY or size % 4 for size in (vertex_size, pixel_size))
            or len(data) != BUNDLE_HEADER.size + vertex_size + pixel_size):
        fail("shader.bundle", "Shader bundle payload sizes differ from its exact framing")
    rows = []
    offset = BUNDLE_HEADER.size
    for shader, size, expected in zip(BUNDLE_SHADERS, (vertex_size, pixel_size),
                                      (vertex_hash, pixel_hash), strict=True):
        binary = data[offset:offset + size]
        if hashlib.sha256(binary).digest() != expected:
            fail("shader.bundle", "Shader bundle payload digest differs", shader["id"])
        if struct.unpack_from("<I", binary, 4)[0] != 0x10600:
            fail("shader.bundle", "Runtime shader bundles require SPIR-V 1.6", shader["id"])
        inspect_spirv(binary, shader)
        rows.append({"id": shader["id"], "sha256": expected.hex(), "bytes": size})
        offset += size
    return {"schema_version": 1, "format": "shark-shader-bundle", "bytes": len(data),
            "sha256": digest(data), "shaders": rows}


def bundle_payload(output: Path) -> tuple[bytes, dict]:
    """Export only the fixed VS/PS closure from a complete verified generation."""
    manifest = verify(output)
    definitions = {row["id"]: row for row in manifest["inputs"]["recipe"]["shaders"]}
    artifacts = {row["id"]: row for row in manifest["shaders"]}
    binaries = []
    for expected in BUNDLE_SHADERS:
        definition = definitions.get(expected["id"])
        if definition is None or {key: definition[key] for key in expected} != expected:
            fail("shader.bundle", "Generation lacks the supported textured cube shader contract", expected["id"])
        row = artifacts[expected["id"]]
        with contained(output, row["file"]).open("rb") as stream:
            binary = stream.read(MAX_BINARY + 1)
        if len(binary) != row["bytes"] or digest(binary) != row["sha256"]:
            fail("shader.bundle", "Shader input changed during bundle preparation", row["file"])
        binaries.append(binary)
    vertex, pixel = binaries
    data = BUNDLE_HEADER.pack(BUNDLE_MAGIC, 1, len(vertex), len(pixel), 0,
                              hashlib.sha256(vertex).digest(), hashlib.sha256(pixel).digest()) + vertex + pixel
    record = inspect_bundle(data)
    if verify(output) != manifest:
        fail("shader.bundle", "Shader generation changed during bundle preparation")
    return data, {**record, "generation": manifest["generation"],
                  "artifact_manifest_sha256": digest(canonical(manifest))}


def export_bundle(output: Path, destination: Path) -> dict:
    """Atomically stage a verified bundle beside a developer executable."""
    no_links(destination)
    if destination.resolve().is_relative_to(output.resolve()):
        fail("shader.path", "Bundle destination must be outside the artifact cache", destination)
    data, record = bundle_payload(output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    no_links(destination.parent)
    lock = destination.with_name(destination.name + ".lock")
    staging = destination.with_name(".shader-bundle-" + uuid.uuid4().hex)
    try:
        with lock.open("xb"):
            pass
    except FileExistsError:
        fail("shader.busy", "Shader bundle destination already has an active writer", destination)
    try:
        existing = None
        if destination.is_file() and destination.stat().st_size == len(data):
            with destination.open("rb") as stream:
                existing = stream.read(len(data) + 1)
        if existing == data:
            disposition = "CACHED"
        else:
            with staging.open("xb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            if inspect_bundle(staging.read_bytes()) != inspect_bundle(data):
                fail("shader.bundle", "Staged shader bundle differs before publication")
            no_links(destination)
            os.replace(staging, destination)
            disposition = "EXPORTED"
    finally:
        staging.unlink(missing_ok=True)
        lock.unlink()
    return {"schema_version": 1, "status": "PASS", "operation": "export", "disposition": disposition,
            "destination": str(destination), **record}


@contextmanager
def output_owner(output: Path):
    no_links(output)
    output.mkdir(parents=True, exist_ok=True)
    contained(output, "shader-manifest.json")
    lock = output / ".shader-build-lock"
    try:
        lock.mkdir()
    except FileExistsError:
        fail("shader.busy", "Another shader build owns this output; inspect an abandoned lock before removing it", lock)
    try:
        yield
    finally:
        lock.rmdir()


def execute(command: list[str], cwd: Path, operation: str) -> None:
    result = run_process(command, cwd=cwd, timeout=30)
    if result.timed_out or result.cleanup_errors or result.returncode:
        details = (result.stderr or result.stdout)[-4096:]
        fail("shader.compile" if operation == "compile" else "shader.validation",
             f"{operation} failed (exit {result.returncode}, timeout={result.timed_out}): {details}")


def build(recipe_path: Path, output: Path, receipt: Path) -> dict:
    no_links(recipe_path)
    no_links(output)
    no_links(receipt)
    definition = recipe(recipe_path)
    root = recipe_path.parent.resolve()
    if output.resolve().is_relative_to(root) or root.is_relative_to(output.resolve()):
        fail("shader.path", "Shader output and source roots must be separate", output)
    files = source_closure(root, definition)
    compiler, validator, identity = tool_inputs(receipt)
    inputs = {"recipe": definition,
              "sources": {name: {"sha256": digest(data), "bytes": len(data)} for name, data in files.items()},
              "toolchain": identity, "flags": FLAGS, "pipeline_sha256": sha256(Path(__file__))}
    generation = digest(canonical(inputs))

    def recheck_inputs() -> None:
        if source_closure(root, definition) != files or recipe(recipe_path) != definition:
            fail("shader.changed", "Shader recipe/source changed during build; retry")
        if tool_inputs(receipt)[2] != identity or sha256(Path(__file__)) != inputs["pipeline_sha256"]:
            fail("shader.changed", "Shader compiler/validator/pipeline changed during build; retry")

    with output_owner(output):
        previous = verify(output) if (output / "shader-manifest.json").exists() else None
        if previous and previous["generation"] == generation:
            recheck_inputs()
            return {"schema_version": 1, "status": "PASS", "operation": "build", "disposition": "CACHED", "generation": generation,
                    "shaders": len(definition["shaders"])}
        staging = output / (".shader-staging-" + uuid.uuid4().hex)
        staging.mkdir()
        try:
            source_root = staging / "source"
            for name, data in files.items():
                target = source_root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            binaries = staging / "binaries"
            binaries.mkdir()
            rows = []
            for shader in definition["shaders"]:
                destination = binaries / (shader["id"] + ".spv")
                command = [str(compiler), *FLAGS, "-T", STAGES[shader["stage"]][0], "-E", shader["entry"],
                           "./" + shader["source"], "-Fo", str(destination.resolve())]
                execute(command, source_root, "compile")
                if not destination.is_file() or destination.stat().st_size > MAX_BINARY:
                    fail("shader.artifact", "Compiler did not produce a bounded shader artifact", shader["id"])
                data = destination.read_bytes()
                inspect_spirv(data, shader)
                # The pinned Windows spirv-val uses narrow filename arguments.
                # Set cwd through Python's Unicode process API and pass the
                # recipe's ASCII artifact basename, including in Unicode roots.
                execute([str(validator), "--target-env", "vulkan1.3", destination.name], binaries, "validate")
                if destination.read_bytes() != data:
                    fail("shader.changed", "Shader artifact changed during independent validation", shader["id"])
                rows.append({"id": shader["id"], "file": f"generations/{generation}/{destination.name}",
                             "sha256": digest(data), "bytes": len(data)})
            recheck_inputs()
            manifest = {"schema_version": 1, "convention": CONVENTION, "generation": generation,
                        "inputs": inputs, "shaders": rows}
            destination = contained(output, f"generations/{generation}")
            destination.parent.mkdir(exist_ok=True)
            if destination.exists():
                # Orphan generations can result from interruption before manifest
                # publication. Never overwrite or trust one without byte checking.
                actual = {path.name for path in destination.iterdir()}
                if actual != {row["id"] + ".spv" for row in rows}:
                    fail("shader.artifact", "Existing immutable generation has an unexpected inventory", destination)
                for row in rows:
                    target = contained(output, row["file"])
                    if target.read_bytes() != (binaries / target.name).read_bytes():
                        fail("shader.artifact", "Existing immutable generation differs from compiler output", target)
            else:
                binaries.rename(destination)
            candidate = staging / "shader-manifest.json"
            candidate.write_bytes(canonical(manifest))
            os.replace(candidate, output / "shader-manifest.json")
            return {"schema_version": 1, "status": "PASS", "operation": "build", "disposition": "BUILT", "generation": generation,
                    "shaders": len(rows)}
        finally:
            if staging.resolve().parent != output.resolve() or staging.is_symlink() or staging.is_junction():
                fail("shader.path", "Refusing cleanup outside owned shader staging", staging)
            shutil.rmtree(staging)


class Parser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        fail("shader.arguments", message)


def main(argv: list[str] | None = None) -> int:
    try:
        parser = Parser(description=__doc__)
        commands = parser.add_subparsers(dest="operation", required=True)
        compile_parser = commands.add_parser("build")
        compile_parser.add_argument("--recipe", type=Path, required=True)
        compile_parser.add_argument("--output", type=Path, required=True)
        compile_parser.add_argument("--validation-manifest", type=Path, required=True)
        verify_parser = commands.add_parser("verify")
        verify_parser.add_argument("--output", type=Path, required=True)
        export_parser = commands.add_parser("export")
        export_parser.add_argument("--output", type=Path, required=True)
        export_parser.add_argument("--destination", type=Path, required=True)
        prepare_parser = commands.add_parser("prepare")
        prepare_parser.add_argument("--recipe", type=Path, required=True)
        prepare_parser.add_argument("--output", type=Path, required=True)
        prepare_parser.add_argument("--validation-manifest", type=Path, required=True)
        prepare_parser.add_argument("--bin-dir", type=Path, required=True)
        args = parser.parse_args(argv)
        if args.operation == "build":
            result = build(args.recipe.absolute(), args.output.absolute(), args.validation_manifest.absolute())
        elif args.operation == "export":
            result = export_bundle(args.output.absolute(), args.destination.absolute())
        elif args.operation == "prepare":
            built = build(args.recipe.absolute(), args.output.absolute(), args.validation_manifest.absolute())
            exported = export_bundle(args.output.absolute(), args.bin_dir.absolute() / "assets/cube.sharkshader")
            result = {"schema_version": 1, "status": "PASS", "operation": "prepare",
                      "build": built, "bundle": exported}
        else:
            manifest = verify(args.output.absolute())
            result = {"schema_version": 1, "status": "PASS", "operation": "verify", "generation": manifest["generation"],
                      "shaders": len(manifest["shaders"])}
        print(json.dumps(result, sort_keys=True))
        return 0
    except (ShaderError, OSError, ValueError, KeyError, TypeError) as error:
        print(json.dumps({"schema_version": 1, "status": "FAIL", "operation": "shader-pipeline", "error": {
            "code": error.code if isinstance(error, ShaderError) else "shader.io",
            "message": str(error), "path": error.path if isinstance(error, ShaderError) else ""}}, sort_keys=True))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
