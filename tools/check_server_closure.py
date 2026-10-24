"""Fail-closed Linux Server/Core/storage source, build and installed ELF inventory.

Run after a fresh configure with file-api codemodel-v2/cache-v2 queries and
CMAKE_EXPORT_COMPILE_COMMANDS=ON, then build/install --component Server.
This intentionally narrow allowlist must be reviewed when a protected module changes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import sys

from core_dependencies import DEPENDENCIES, MBEDTLS_LICENSE_FILES
from storage_dependency import DEFINITIONS as SQLITE_DEFINITIONS, verify_server_build
from process_runner import run_process


class ClosureError(ValueError):
    pass


TLS_STATIC = {"SharkTLS_mbedtls", "SharkTLS_mbedx509", "SharkTLS_tfpsacrypto"}
TLS_OBJECT = {"SharkTLS_" + name for name in ("builtin", "everest", "p256-m", "pqcp", "extras", "platform", "utilities")}
TLS_UTILITIES = {"SharkTLS_lib", "SharkTLS_mbedtls-apidoc", "SharkTLS_tfpsacrypto-apidoc"}
TLS_DIRECTORIES = {"", "framework", "include", "library", "pkgconfig", "tf-psa-crypto",
                   "tf-psa-crypto/framework", "tf-psa-crypto/include", "tf-psa-crypto/drivers",
                   "tf-psa-crypto/drivers/everest", "tf-psa-crypto/drivers/p256-m",
                   "tf-psa-crypto/drivers/builtin", "tf-psa-crypto/drivers/pqcp",
                   "tf-psa-crypto/extras", "tf-psa-crypto/platform", "tf-psa-crypto/utilities",
                   "tf-psa-crypto/core", "tf-psa-crypto/pkgconfig"}
STORAGE_TARGETS = {"EngineStorage", "SharkSQLite"}
TARGETS = {"EngineCore", "Server", "shark_check_server_boundary"} | STORAGE_TARGETS | TLS_STATIC | TLS_OBJECT | TLS_UTILITIES
PACKAGE_FILES = {"bin/Server", "licenses/EnTT/LICENSE", "licenses/GLM/copying.txt", "licenses/SQLite/LICENSE.txt"} | {
    "licenses/MbedTLS/" + item for item in MBEDTLS_LICENSE_FILES}
FORBIDDEN = re.compile(
    r"(?:^|[/\\\s;:=_-])(?:engineclient|engine_tools|editor|runtime|catch2|"
    r"glfw|vulkan|nvrhi|imgui|imguizmo|cgltf|stb|dr_wav|miniaudio|openal|"
    r"sdl2?|sdl3|shaderc|glslang|spirv-tools|x11|wayland|pulse|asound)(?:$|[/\\\s;:._-])",
    re.IGNORECASE,
)
LIBRARIES = re.compile(
    r"(?:lib(?:c|m|dl|pthread|rt|resolv)\.so\.[0-9]+|libstdc\+\+\.so\.6|"
    r"libgcc_s\.so\.1|libasan\.so\.[0-9]+|libubsan\.so\.[0-9]+|"
    r"ld-linux-x86-64\.so\.2)"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ClosureError(message)


def read_json(path: Path):
    require(path.is_file() and not path.is_symlink(), f"Missing or linked JSON evidence: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def plain_path(path: Path, root: Path) -> Path:
    """Check lexical containment and every link before resolving the input."""
    path = Path(os.path.abspath(path))
    root = Path(os.path.abspath(root))
    try:
        relative = path.relative_to(root)
    except ValueError as error:
        raise ClosureError(f"Path escapes evidence root: {path}") from error
    current = root
    require(not current.is_symlink(), f"Linked root: {root}")
    for part in relative.parts:
        current = current / part
        require(not current.is_symlink(), f"Linked input: {current}")
    require(path.exists(), f"Missing input: {path}")
    return path


def source_path(path: Path, source: Path, build: Path, *, generated=False) -> Path:
    if path.is_absolute() and path.is_relative_to(build):
        generated = True
    base = build if generated else source
    path = plain_path(path if path.is_absolute() else base / path, base)
    relative = path.relative_to(base).as_posix()
    allowed = ("engine/core/", "server/")
    if not generated:
        allowed += ("engine/include/",)
    dependency_header = (generated and relative.startswith(("_deps/entt-src/src/entt/", "_deps/glm-src/glm/"))
                         and path.suffix in (".h", ".hpp", ".inl"))
    network_input = (generated and relative.startswith("_deps/mbedtls-src/")
                     and path.suffix in (".h", ".c")
                     and not set(Path(relative).parts) & {"framework", "tests", "test", "examples", "programs"})
    network_config = not generated and relative == "cmake/mbedtls_user_config.h"
    storage_input = generated and relative in ("_deps/sqlite-src/sqlite3.c", "_deps/sqlite-src/sqlite3.h")
    require(relative.startswith(allowed) or dependency_header or network_input or network_config or storage_input,
            f"Non-Core source/include path: {path}")
    require(not FORBIDDEN.search(relative), f"Forbidden source/include path: {path}")
    return path


def include_path(path: Path, source: Path, build: Path) -> Path:
    path = Path(os.path.abspath(path))
    # Directory roots themselves are valid include paths.
    for base, allowed in ((build, ("engine/core", "server", "_deps/entt-src/src", "_deps/glm-src",
                                  "_deps/mbedtls-src/include", "_deps/mbedtls-src/library",
                                  "_deps/mbedtls-src/tf-psa-crypto/include", "_deps/mbedtls-src/tf-psa-crypto/core",
                                  "_deps/mbedtls-src/tf-psa-crypto/extras", "_deps/mbedtls-src/tf-psa-crypto/platform",
                                  "_deps/mbedtls-src/tf-psa-crypto/utilities",
                                  "_deps/mbedtls-src/tf-psa-crypto/dispatch", "_deps/mbedtls-src/tf-psa-crypto/drivers",
                                  "_deps/mbedtls-build/include", "_deps/mbedtls-build/library", "_deps/mbedtls-build/tf-psa-crypto/include",
                                  "_deps/mbedtls-build/tf-psa-crypto/core", "_deps/sqlite-src")),
                          (source, ("engine/include", "engine/core/include", "server"))):
        if path.is_relative_to(base):
            path = plain_path(path, base)
            relative = path.relative_to(base).as_posix()
            require(any(relative == item or relative.startswith(item + "/") for item in allowed),
                    f"Non-Core include directory: {path}")
            require(not FORBIDDEN.search(relative), f"Forbidden include directory: {path}")
            require(not set(Path(relative).parts) & {"tests", "test", "examples", "programs"},
                    f"Unbuilt network source include: {path}")
            return path
    raise ClosureError(f"External include directory: {path}")


def codemodel_source(entry: dict, source: Path, build: Path) -> Path:
    # File-api source paths are source-root relative even for generated files
    # when the build directory is nested inside the source checkout.
    path = Path(entry["path"])
    return source_path(path if path.is_absolute() else source / path, source, build,
                       generated=entry.get("isGenerated", False))


def file_api(build: Path) -> tuple[Path, dict, list[dict], dict]:
    reply = build / ".cmake/api/v1/reply"
    indexes = sorted(reply.glob("index-*.json"))
    require(bool(indexes), "Missing CMake file-api reply (create codemodel-v2/cache-v2 queries before configure)")
    index = read_json(indexes[-1])
    objects = {}
    for item in index.get("objects", []):
        if item.get("kind") in ("codemodel", "cache") and item.get("version", {}).get("major") == 2:
            require(item["kind"] not in objects, "Duplicate file-api object")
            objects[item["kind"]] = read_json(plain_path(reply / item["jsonFile"], reply))
    require(set(objects) == {"codemodel", "cache"}, "Missing codemodel/cache v2 evidence")
    model = objects["codemodel"]
    source = Path(model["paths"]["source"]).resolve()
    require(Path(model["paths"]["build"]).resolve() == build, "File-api build path mismatch")
    require(source != build and source.is_dir(), "Invalid source tree")
    configurations = model.get("configurations", [])
    require(len(configurations) == 1, "Expected one Ninja server configuration")
    configuration = configurations[0]
    references = configuration.get("targets", [])
    names = [item["name"] for item in references]
    require(len(names) == len(set(names)) and set(names) == TARGETS,
            f"Server-only target closure changed: {names}")
    targets = [read_json(plain_path(reply / item["jsonFile"], reply)) for item in references]
    network_objects = {Path(os.path.abspath(build / artifact["path"]))
               for item in targets if item["name"] in TLS_OBJECT for artifact in item.get("artifacts", [])}
    target_ids = {item["id"] for item in targets}
    require(len(target_ids) == len(targets), "Duplicate target IDs")
    for item in targets:
        require(item["name"] in TARGETS, "File-api target identity mismatch")
        if item["name"] in TLS_STATIC | TLS_OBJECT | TLS_UTILITIES | STORAGE_TARGETS:
            expected_type = "STATIC_LIBRARY" if item["name"] in TLS_STATIC | STORAGE_TARGETS else (
                "OBJECT_LIBRARY" if item["name"] in TLS_OBJECT else "UTILITY")
            require(item.get("type") == expected_type, "Unexpected reviewed TLS target type")
        for dependency in item.get("dependencies", []):
            require(dependency["id"] in target_ids, f"External build dependency: {dependency}")
            reached = next(target["name"] for target in targets if target["id"] == dependency["id"])
            require(item["name"] in {"Server", "EngineStorage"} or reached not in STORAGE_TARGETS,
                    "Storage dependency escaped its Server owner")
        for group in item.get("compileGroups", []):
            language = "C" if item["name"] in TLS_STATIC | TLS_OBJECT | {"SharkSQLite"} else "CXX"
            require(group.get("language") == language, "Unexpected Core source language")
            for include in group.get("includes", []):
                directory = include_path(Path(include["path"]), source, build)
                require(item["name"] in STORAGE_TARGETS or not directory.is_relative_to(build / "_deps/sqlite-src"),
                        "Private SQLite include escaped storage target")
                require(item["name"] != "EngineCore" or not directory.is_relative_to(source / "server"),
                        "Server storage include escaped into shared Core")
        for entry in item.get("sources", []):
            path = Path(entry["path"])
            if item["name"] in TLS_UTILITIES | {"shark_check_server_boundary"}:
                # CMake may expose the utility's generated .rule pseudo-source.
                full = path if path.is_absolute() else source / path
                relative = os.path.relpath(full, build).replace("\\", "/")
                utility = item.get("paths", {}).get("build", ".") + "/CMakeFiles/" + item["name"]
                utility = os.path.normpath(utility).replace("\\", "/")
                require(entry.get("isGenerated") and relative in (utility, utility + ".rule"),
                    f"Unexpected boundary utility input: {entry}")
            elif item["name"] == "SharkTLS_tfpsacrypto" and entry.get("isGenerated") and path.suffix == ".o":
                full = Path(os.path.abspath(path if path.is_absolute() else source / path))
                require(full in network_objects, f"Unreviewed network object source: {full}")
            else:
                suffixes = ((".c",) if item["name"] in TLS_STATIC | TLS_OBJECT | {"SharkSQLite"} else (".cpp", ".cc", ".cxx")) if "compileGroupIndex" in entry else (".h", ".hh", ".hpp", ".hxx", ".inl")
                require(path.suffix in suffixes, f"Unreviewed prebuilt/noncompiled source input: {path}")
                resolved = codemodel_source(entry, source, build)
                require(item["name"] in STORAGE_TARGETS or not resolved.is_relative_to(build / "_deps/sqlite-src"),
                        "Private SQLite source escaped storage target")
                require(item["name"] != "EngineCore" or not resolved.is_relative_to(source / "server"),
                        "Server storage source escaped into shared Core")
                if item["name"] == "SharkSQLite" and "compileGroupIndex" in entry:
                    require(resolved == build / "_deps/sqlite-src/sqlite3.c", "SQLite compiles only the reviewed amalgamation")
    for directory in configuration.get("directories", []):
        relative = directory["source"].replace("\\", "/")
        full = Path(os.path.abspath(source / relative))
        network = build / "_deps/mbedtls-src"
        network_directory = full.is_relative_to(network) and full.relative_to(network).as_posix().replace(".", "", 1) in TLS_DIRECTORIES
        require(relative in (".", "engine", "engine/core", "server") or network_directory,
                f"Non-Core configured directory: {relative}")
    cache_items = objects["cache"].get("entries", [])
    cache = {entry["name"]: entry["value"] for entry in cache_items}
    require(len(cache) == len(cache_items), "Duplicate CMake cache keys")
    expected = {"SHARK_BUILD_CLIENT": "OFF", "SHARK_BUILD_EDITOR": "OFF", "SHARK_BUILD_TESTS": "OFF",
                "SHARK_BUILD_GRAPHICS_PROBE": "OFF",
                "SHARK_BUILD_SERVER": "ON", "CMAKE_EXPORT_COMPILE_COMMANDS": "ON", "CMAKE_GENERATOR": "Ninja"}
    # Upstream's CRT option name contains "runtime"; review its exact locked
    # disabled value rather than treating it as a presentation discovery.
    expected.update(read_json(source / "dependencies.lock.json")["dependencies"]["MbedTLS"].get("options", {}))
    for name, value in expected.items():
        require(cache.get(name) == value, f"Expected {name}={value}, found {cache.get(name)!r}")
    require(cache.get("SHARK_SANITIZER") in ("none", "address", "address-undefined"), "Unknown sanitizer configuration")
    for name, value in cache.items():
        if name in expected or name.startswith("CMAKE_"):
            continue
        require(not FORBIDDEN.search(name), f"Forbidden discovered dependency cache key: {name}")
        if name.endswith(("_SOURCE_DIR", "_BINARY_DIR", "_DIR", "_LIBRARY", "_INCLUDE_DIR")):
            require(not FORBIDDEN.search(value), f"Forbidden discovered dependency cache input: {name}={value}")
    archives = {name: Path(cache[spec["cache"]]) for name, spec in DEPENDENCIES.items() if cache.get(spec["cache"])}
    require(set(archives) == set(DEPENDENCIES), "Missing locked Core archive evidence")
    try:
        require(bool(cache.get("SHARK_SQLITE_ARCHIVE_INPUT")), "Missing locked SQLite archive evidence")
        verify_server_build(source, build, archives, Path(cache["SHARK_SQLITE_ARCHIVE_INPUT"]))
    except ValueError as error:
        raise ClosureError(str(error)) from error
    for relative in ("engine/client", "engine_tools", "editor", "runtime", "tests", "probes"):
        require(not (build / relative).exists(), f"Server-only build contains forbidden build subtree: {relative}")
    return source, configuration, targets, cache


def compile_inventory(build: Path, source: Path, targets: list[dict]) -> list[dict]:
    commands = read_json(build / "compile_commands.json")
    require(isinstance(commands, list) and bool(commands), "Missing compiler invocations")
    compiled = []
    for entry in commands:
        directory = plain_path(Path(entry["directory"]), build)
        path = source_path(Path(entry["file"]), source, build)
        require(path.suffix in (".cpp", ".cc", ".cxx", ".c"), f"Unexpected compile input: {path}")
        args = entry.get("arguments") or shlex.split(entry["command"])
        require(args and not any(arg.startswith("@") for arg in args), "Opaque compiler response file")
        require(not FORBIDDEN.search(" ".join(args)), "Forbidden compiler dependency")
        require(not any(arg.startswith(("-fplugin", "-specs", "--sysroot", "-isysroot", "-Xclang")) for arg in args),
                "Unreviewed compiler injection/sysroot option")
        sqlite_unit = path == build / "_deps/sqlite-src/sqlite3.c"
        storage_unit = path.is_relative_to(source / "server/storage/src")
        sqlite_definitions = [arg.removeprefix("-D") for arg in args if arg.startswith("-DSQLITE_")]
        require(set(sqlite_definitions) == set(SQLITE_DEFINITIONS) and len(sqlite_definitions) == len(SQLITE_DEFINITIONS)
                if sqlite_unit else not sqlite_definitions, "Private SQLite compile definitions escaped or changed")
        require(sqlite_unit or storage_unit or not any("sqlite-src" in arg for arg in args),
                "Private SQLite header escaped storage implementation")
        require(not path.is_relative_to(source / "engine/core") or not any("server/storage" in arg.replace("\\", "/") for arg in args),
                "Storage header escaped into shared Core")
        index = 1
        while index < len(args):
            arg = args[index]
            if arg in ("-I", "-isystem", "-iquote", "-idirafter", "-include", "-imacros"):
                index += 1
                require(index < len(args), "Missing compiler include argument")
                value = Path(args[index])
                value = value if value.is_absolute() else directory / value
                if arg in ("-include", "-imacros"):
                    source_path(value, source, build, generated=value.is_relative_to(build))
                else:
                    include_path(value, source, build)
            elif arg.startswith("-I") and len(arg) > 2:
                value = Path(arg[2:])
                include_path(value if value.is_absolute() else directory / value, source, build)
            elif arg.startswith(("-isystem", "-iquote", "-idirafter", "-include", "-imacros")):
                raise ClosureError(f"Unreviewed joined include option: {arg}")
            index += 1
        require(args.count("-o") == 1 and args.index("-o") + 1 < len(args), "Missing/ambiguous compiler output")
        output = Path(args[args.index("-o") + 1])
        output = plain_path(output if output.is_absolute() else directory / output, build)
        compiled.append({"path": str(path), "sha256": digest(path), "arguments": args,
                         "object": output.relative_to(build).as_posix()})
    expected = {str(codemodel_source(entry, source, build))
                for target in targets for entry in target.get("sources", []) if "compileGroupIndex" in entry}
    actual = [item["path"] for item in compiled]
    require(len(actual) == len(set(actual)) and set(actual) == expected and len(actual) >= 2,
            "Compile commands do not exactly cover Core/Server codemodel sources")
    for target in targets:
        if target["name"] not in TLS_OBJECT:
            continue
        inputs = {str(codemodel_source(item, source, build)) for item in target.get("sources", [])
                  if "compileGroupIndex" in item}
        outputs = {Path(os.path.abspath(build / item["object"])) for item in compiled if item["path"] in inputs}
        artifacts = {Path(os.path.abspath(build / item["path"])) for item in target.get("artifacts", [])}
        require(bool(inputs) and len(inputs) == len(outputs) and outputs == artifacts,
                "Reviewed TLS objects differ from actual compiler outputs: " + target["name"])
    return compiled


def link_inventory(targets: list[dict], build: Path) -> list[dict]:
    server = next(item for item in targets if item["name"] == "Server")
    fragments = server.get("link", {}).get("commandFragments", [])
    require(bool(fragments), "Missing actual Server linker inputs")
    archives = []
    for name in ("EngineStorage", "EngineCore", "SharkTLS_mbedtls", "SharkTLS_mbedx509", "SharkTLS_tfpsacrypto", "SharkSQLite"):
        core = next(item for item in targets if item["name"] == name)
        artifacts = core.get("artifacts", [])
        require(core.get("type") == "STATIC_LIBRARY" and len(artifacts) == 1, "Expected one static Core/TLS artifact")
        archives.append(plain_path(build / artifacts[0]["path"], build))
    libraries = []
    for entry in fragments:
        tokens = shlex.split(entry["fragment"])
        if entry["role"] == "libraries":
            for token in tokens:
                path = Path(token)
                path = plain_path(path if path.is_absolute() else build / path, build)
                require(path in archives, f"Unreviewed static/linker input: {token}")
                libraries.append(path)
        else:
            require(entry["role"] == "flags", f"Unreviewed linker search input: {entry}")
            for token in tokens:
                require(bool(re.fullmatch(r"(?:-O[0-3sg]|-g|-DNDEBUG|-pthread|-fsanitize=address(?:,undefined)?|-fno-omit-frame-pointer)", token)),
                        f"Unreviewed actual link option: {token}")
    require(libraries == archives, "Server must link exactly the reviewed storage/Core/TLS archives")
    return fragments


def ninja_dependencies(output: str) -> dict[str, list[str]]:
    groups = {}
    current = None
    for line in output.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"(.+): #deps ([0-9]+), deps mtime [0-9]+ \((VALID|STALE)\)", line)
        if match:
            current = match.group(1).replace("\\", "/")
            require(current not in groups and match.group(3) == "VALID", "Duplicate/stale Ninja dependency record")
            groups[current] = {"count": int(match.group(2)), "paths": []}
        else:
            require(current is not None and line.startswith("    "), f"Unrecognized Ninja dependency evidence: {line}")
            groups[current]["paths"].append(line.strip())
    require(bool(groups), "No compiler-discovered header evidence")
    for group in groups.values():
        require(len(group["paths"]) == group["count"] and group["count"] > 0, "Incomplete Ninja dependency record")
    return {name: group["paths"] for name, group in groups.items()}


def compiler_headers(output: str, compiled: list[dict], source: Path, build: Path,
                     *, linker_outputs: set[str] | None = None) -> tuple[list[dict], list[str]]:
    groups = ninja_dependencies(output)
    objects = {entry["object"] for entry in compiled}
    # Newer Ninja/CMake also record linker depfiles. They belong to the known
    # executable artifact, not to compiler-discovered header inputs. Preserve
    # them in the probe/report while still rejecting unknown extra records.
    require(objects.issubset(groups) and set(groups) - objects <= (linker_outputs or set()),
            "Ninja dependency objects differ from compiler invocations")
    for entry in compiled:
        owner = Path(entry.get("path", source / "engine/core/unknown.cpp"))
        storage = owner.is_relative_to(source / "server/storage/src") or owner == build / "_deps/sqlite-src/sqlite3.c"
        for value in groups[entry["object"]]:
            path = Path(os.path.abspath(value if Path(value).is_absolute() else build / value))
            require(storage or not path.is_relative_to(build / "_deps/sqlite-src"),
                    "Private SQLite compiler-discovered input escaped storage")
            require(not owner.is_relative_to(source / "engine/core") or not path.is_relative_to(source / "server/storage"),
                    "Storage compiler-discovered input escaped into shared Core")
    headers, system = [], []
    for name in sorted({path for name, group in groups.items() if name in objects for path in group}):
        path = Path(name)
        path = Path(os.path.abspath(path if path.is_absolute() else build / path))
        if path.is_relative_to(source) and not path.is_relative_to(build):
            path = source_path(path, source, build)
        elif path.is_relative_to(build):
            path = source_path(path, source, build, generated=True)
        else:
            require(not FORBIDDEN.search(path.as_posix()), f"Forbidden compiler-discovered header: {path}")
            require(path.is_file() and any(path.is_relative_to(Path(root)) for root in ("/usr/include", "/usr/lib/gcc", "/usr/lib/llvm-18")),
                    f"External compiler-discovered header: {path}")
            system.append(str(path))
        headers.append({"path": str(path), "sha256": digest(path)})
    return headers, system


def system_header_owners(output: str, paths: list[str]) -> dict[str, str]:
    owners = {}
    for line in output.splitlines():
        match = re.fullmatch(r"([^: ]+)(?::[a-z0-9-]+)?: (/.+)", line)
        require(match is not None, f"Unknown system-header package ownership: {line}")
        package, path = match.groups()
        require(package in {"libc6-dev", "linux-libc-dev", "libstdc++-13-dev", "libgcc-13-dev", "libclang-common-18-dev"},
                f"Unreviewed system header package: {package}")
        require(path not in owners, f"Ambiguous system header owner: {path}")
        owners[path] = package
    require(set(owners) == set(paths), "Incomplete system-header package ownership")
    return owners


def package_inventory(package: Path) -> Path:
    plain_path(package, package)
    inventory = []
    for item in package.rglob("*"):
        plain_path(item, package)
        relative = item.relative_to(package).as_posix()
        directories = {parent.as_posix() for name in PACKAGE_FILES for parent in Path(name).parents if str(parent) != "."}
        require((item.is_dir() and relative in directories)
                or (item.is_file() and relative in PACKAGE_FILES),
                f"Unexpected package entry: {relative}")
        if item.is_file():
            inventory.append(relative)
    require(set(inventory) == PACKAGE_FILES, f"Incomplete Server package: {inventory}")
    binary = package / "bin/Server"
    header = binary.read_bytes()[:20]
    require(len(header) == 20 and header[:6] == b"\x7fELF\x02\x01" and header[18:20] == b"\x3e\x00",
            "Server package is not a Linux x86-64 ELF binary")
    require(os.access(binary, os.X_OK), "Packaged Server is not executable")
    return binary


def dynamic_needed(output: str) -> list[str]:
    require("Dynamic section" in output, "Missing readelf dynamic-section evidence")
    require(not re.search(r"\((?:RPATH|RUNPATH)\)", output), "Server has an unreviewed ELF runtime search path")
    names = re.findall(r"\(NEEDED\).*\[([^\]]+)\]", output)
    require(bool(names) and len(names) == len(set(names)), "Missing/duplicate ELF NEEDED entries")
    for name in names:
        require(bool(LIBRARIES.fullmatch(name)), f"Unreviewed ELF dependency: {name}")
    return names


def resolved_libraries(output: str, needed: list[str], *, sanitizer: str) -> list[dict]:
    libraries = []
    for line in output.splitlines():
        line = line.strip()
        if not line:
            continue
        if re.fullmatch(r"linux-vdso\.so\.1 \(0x[0-9a-fA-F]+\)", line):
            continue
        match = re.fullmatch(r"([^\s]+) => (/[^\s]+) \(0x[0-9a-fA-F]+\)", line)
        if match:
            name, path = match.groups()
        else:
            match = re.fullmatch(r"(/[^\s]+) \(0x[0-9a-fA-F]+\)", line)
            require(match is not None, f"Unresolved/unrecognized ldd entry: {line}")
            path = match.group(1)
            name = Path(path).name
        require(bool(LIBRARIES.fullmatch(name)), f"Unreviewed transitive ELF dependency: {name}")
        require(sanitizer != "none" or not name.startswith(("libasan.", "libubsan.")),
                f"Unexpected sanitizer runtime in ordinary Server: {name}")
        actual = Path(path).resolve()
        require(actual.is_file() and any(actual.is_relative_to(Path(root)) for root in ("/usr/lib", "/lib", "/lib64")),
                f"Dependency resolved outside system library roots: {path}")
        libraries.append({"name": name, "resolved_path": str(actual), "sha256": digest(actual)})
    names = [item["name"] for item in libraries]
    require(len(names) == len(set(names)) and set(needed).issubset(names), "Incomplete/duplicate ldd closure")
    return libraries


def probe(command: list[str], *, cwd: Path | None = None) -> dict:
    result = run_process(command, timeout=15, cwd=cwd)
    require(result.returncode == 0 and not result.timed_out and not result.cleanup_errors,
            f"ELF inspection failed: {command}: {result.stderr}")
    return {"command": command, "cwd": str(cwd) if cwd else None, "exit_code": result.returncode, "stdout": result.stdout, "stderr": result.stderr}


def inspect(build: Path, package: Path) -> dict:
    require(platform.system() == "Linux" and platform.machine() in ("x86_64", "AMD64"),
            "Actual package closure gate requires Linux x86-64")
    require(not os.environ.get("LD_PRELOAD") and not os.environ.get("LD_LIBRARY_PATH"),
            "ELF inventory requires no loader environment overrides")
    build, package = build.absolute(), package.absolute()
    source, configuration, targets, cache = file_api(build)
    compiled = compile_inventory(build, source, targets)
    links = link_inventory(targets, build)
    server = next(item for item in targets if item["name"] == "Server")
    artifacts = server.get("artifacts", [])
    require(server.get("type") == "EXECUTABLE" and len(artifacts) == 1, "Missing built Server artifact")
    built_binary = plain_path(build / artifacts[0]["path"], build)
    linker_outputs = {built_binary.relative_to(build).as_posix()}
    ninja, dpkg = shutil.which("ninja"), shutil.which("dpkg-query")
    require(bool(ninja) and bool(dpkg), "Ninja and dpkg-query required for actual compiler header inventory")
    dependencies = probe([ninja, "-t", "deps"], cwd=build)
    headers, system = compiler_headers(dependencies["stdout"], compiled, source, build, linker_outputs=linker_outputs)
    owners = probe([dpkg, "-S", *system]) if system else None
    packages = system_header_owners(owners["stdout"], system) if owners else {}
    core_dependencies = verify_server_build(source, build, {name: Path(cache[spec["cache"]])
                                                    for name, spec in DEPENDENCIES.items()}, Path(cache["SHARK_SQLITE_ARCHIVE_INPUT"]))
    binary = package_inventory(package)
    for name, record in core_dependencies.items():
        for license_file, expected_sha in record["license_files"].items():
            require(digest(package / "licenses" / name / license_file) == expected_sha,
                    "Packaged " + name + " license differs from the locked archive")
    require(digest(binary) == digest(built_binary), "Installed Server differs from the inspected build artifact")
    readelf, ldd = shutil.which("readelf"), shutil.which("ldd")
    require(bool(readelf) and bool(ldd), "readelf and ldd are mandatory for the Linux closure gate")
    dynamic = probe([readelf, "--dynamic", "--wide", str(binary)])
    needed = dynamic_needed(dynamic["stdout"])
    loader = probe([ldd, str(binary)])
    libraries = resolved_libraries(loader["stdout"], needed, sanitizer=cache["SHARK_SANITIZER"])
    return {"schema_version": 1, "status": "PASS", "scope": "Linux x86-64 Server/Core/private-storage source/build/package closure",
            "source_dir": str(source), "build_dir": str(build), "package_dir": str(package),
            "configuration": configuration["name"], "targets": sorted(TARGETS),
            "cache": {name: cache[name] for name in ("SHARK_BUILD_CLIENT", "SHARK_BUILD_SERVER", "SHARK_BUILD_EDITOR",
                       "SHARK_BUILD_TESTS", "SHARK_BUILD_GRAPHICS_PROBE", "SHARK_SANITIZER", "CMAKE_GENERATOR")},
            "compiled_sources": compiled, "compiler_discovered_inputs": headers, "system_header_packages": packages,
            "link_fragments": links, "dependencies": core_dependencies,
            "linker_dependency_records": {name: paths for name, paths in ninja_dependencies(dependencies["stdout"]).items() if name in linker_outputs},
            "package": [{"path": name, "sha256": digest(package / name), "size_bytes": (package / name).stat().st_size}
                        for name in sorted(PACKAGE_FILES)],
            "elf_needed": needed, "resolved_libraries": libraries, "probes": [dependencies, *([owners] if owners else []), dynamic, loader]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--package-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        report = inspect(args.build_dir, args.package_dir)
    except (ClosureError, OSError, ValueError, KeyError, TypeError, IndexError) as error:
        report = {"schema_version": 1, "status": "FAIL", "error": str(error)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "report": str(args.output), "error": report.get("error")}))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
