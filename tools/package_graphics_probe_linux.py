"""Build an inspected Ubuntu 24.04 x86-64 standalone graphics probe fixture."""

from __future__ import annotations

import argparse
from dataclasses import asdict
from datetime import datetime, timezone
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import uuid

from graphics_probe import (ROOT, SHADERS, inspect_result, probe_source_digest, read_json,
                            sha256, shader_inputs, source_state, validation_tools)
from package_graphics_probe import (dependency_notices, digest, json_bytes, publish as publish_payload,
                                    regular_file)
from prepare_graphics_validation import assert_no_links
from process_runner import run_process
from toolchain_report import collect

# These libraries are Ubuntu 24.04 OS/runtime prerequisites, never resolved from
# arbitrary developer directories. Vulkan's loader and the layer are bundled.
SYSTEM_LIBRARIES = {
    "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
    "libstdc++.so.6", "libgcc_s.so.1", "ld-linux-x86-64.so.2",
    "libX11.so.6", "libXrandr.so.2", "libXinerama.so.1", "libXcursor.so.1",
    "libXi.so.6", "libXext.so.6", "libxcb.so.1", "libXau.so.6", "libXdmcp.so.6",
    "libXrender.so.1", "libXfixes.so.3",
}
# Ubuntu noble's recorded libstdc++6/libgcc-s1 14.2.0-4ubuntu2~24.04.1
# export these ceilings; glibc is 2.39. Revalidate on a baseline amendment.
VERSION_LIMITS = {"GLIBC": (2, 39), "GLIBCXX": (3, 4, 33), "CXXABI": (1, 3, 15), "GCC": (14, 0, 0)}


def cache_values(build_dir: Path, executable: Path) -> dict:
    text = regular_file(build_dir / "CMakeCache.txt", maximum=4 * 1024 * 1024).decode("utf-8")
    values = {key: value.rstrip("\r") for key, value in re.findall(r"^([^/#:=\n]+):[^=\n]+=(.*)$", text, re.M)}
    if values.get("CMAKE_BUILD_TYPE") != "Release" or values.get("SHARK_SANITIZER") != "none":
        raise ValueError("Linux package requires Release with sanitizers disabled")
    if values.get("SHARK_BUILD_GRAPHICS_PROBE") != "ON":
        raise ValueError("Selected build does not enable the graphics probe")
    if Path(values.get("CMAKE_HOME_DIRECTORY", "")).resolve() != ROOT.resolve():
        raise ValueError("Build cache belongs to a different source checkout")
    if executable.resolve() != (build_dir / "bin/GraphicsProbe").resolve():
        raise ValueError("Executable must belong to the selected Release build")
    return values


def elf_x64(data: bytes, name: str) -> None:
    if (len(data) < 64 or data[:6] != b"\x7fELF\x02\x01"
            or struct.unpack_from("<H", data, 18)[0] != 62):
        raise ValueError(f"Native input must be a little-endian x86-64 ELF binary: {name}")


def parse_elf(output: str) -> dict:
    imports = re.findall(r"\(NEEDED\).*Shared library: \[([^\]]+)\]", output)
    if not imports or len(imports) > 128:
        raise ValueError("ELF inspection has no bounded dependency table")
    if any(not re.fullmatch(r"[A-Za-z0-9_.+-]+", name) for name in imports):
        raise ValueError("ELF dependency contains an unsafe path")
    if re.search(r"\((?:RUNPATH|RPATH)\)", output):
        raise ValueError("Package binaries must have no RPATH/RUNPATH; use graphics-linux-release")
    interpreter = re.findall(r"Requesting program interpreter: ([^\]]+)", output)
    if interpreter and interpreter != ["/lib64/ld-linux-x86-64.so.2"]:
        raise ValueError("ELF interpreter does not match the Ubuntu x86-64 baseline")
    versions = sorted(set(re.findall(r"Name: ((?:GLIBC|GLIBCXX|CXXABI|GCC)_[A-Za-z0-9_.]+)", output)))
    for version in versions:
        family, number = version.split("_", 1)
        if not re.fullmatch(r"\d+(?:\.\d+)+", number):
            raise ValueError(f"Unsupported runtime symbol contract: {version}")
        if tuple(map(int, number.split("."))) > VERSION_LIMITS[family]:
            raise ValueError(f"Runtime symbol exceeds Ubuntu 24.04 baseline: {version}")
    return {"imports": sorted(set(imports)), "versions": versions, "interpreter": interpreter}


def inspect_closure(binaries: dict[str, Path], inspector: Path) -> dict:
    inspector = inspector.resolve(strict=True)
    inspector_hash = sha256(inspector)
    bundled = {Path(name).name for name in binaries if name.startswith("lib/")}
    records = {}
    for name, path in sorted(binaries.items()):
        data = regular_file(path)
        elf_x64(data, name)
        result = run_process([str(inspector), "--wide", "--dynamic", "--version-info", "--program-headers", str(path)],
                             cwd=ROOT, timeout=30, env={**os.environ, "LC_ALL": "C"})
        if result.returncode or result.timed_out or result.cleanup_errors:
            raise ValueError(f"ELF inspection failed: {name}")
        record = parse_elf(result.stdout)
        for imported in record["imports"]:
            if imported not in bundled and imported not in SYSTEM_LIBRARIES:
                raise ValueError(f"Unpackaged external dependency: {name} imports {imported}")
        regular_file(path, digest(data))
        records[name] = {**record, "sha256": digest(data), "inspection": asdict(result)}
    if sha256(inspector) != inspector_hash:
        raise ValueError("ELF inspector changed during packaging")
    return {"status": "PASS", "binaries": records,
            "inspector": {"path": str(inspector), "sha256": inspector_hash},
            "system_allowlist": sorted(SYSTEM_LIBRARIES), "maximum_symbol_versions": VERSION_LIMITS,
            "scope": "Recursive packaged ELF imports; system desktop libraries and vendor ICDs remain OS prerequisites"}


def prepare_payload(args: argparse.Namespace) -> tuple[dict[str, bytes], dict]:
    packagers = {ROOT / "tools/package_graphics_probe_linux.py", ROOT / "tools/package_graphics_probe.py"}
    packager_hashes = {path: digest(regular_file(path, maximum=1024 * 1024)) for path in packagers}
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
    loader = args.loader.absolute()
    # The public SONAME may be a build symlink; copy only its explicit regular
    # target, after requiring that the target belongs to this build's bin tree.
    if (not loader.resolve().is_relative_to((build_dir / "bin").resolve())
            or loader.resolve() != (build_dir / "bin/libvulkan.so.1").resolve()):
        raise ValueError("Loader must be the selected build's actual libvulkan.so.1 target")
    binaries = {"bin/GraphicsProbe": executable, "lib/libvulkan.so.1": loader,
                "validation/libVkLayer_khronos_validation.so": Path(validation["validation_layer"]["library_path"])}
    inspector = shutil.which("readelf")
    if not inspector:
        raise ValueError("Native readelf is required for ELF closure inspection")
    closure = inspect_closure(binaries, Path(inspector))
    payload = {name: regular_file(path, closure["binaries"][name]["sha256"]) for name, path in binaries.items()}
    env = {key: value for key, value in os.environ.items() if not key.startswith(("LD_", "VK_"))}
    env["LD_LIBRARY_PATH"] = str(build_dir / "bin")
    described = run_process([str(executable), "--describe"], cwd=ROOT, timeout=15, env=env)
    description, errors = inspect_result(described, "describe", lock_digest, source_digest)
    if errors or described.returncode:
        raise ValueError("Release executable identity check failed: " + "; ".join(errors))
    for record in shaders["shaders"]:
        payload["shaders/" + record["file"]] = regular_file(args.shader_dir / record["file"], record["sha256"], 4 * 1024 * 1024)
    layer = read_json(Path(validation["validation_layer"]["manifest_path"]))
    layer["layer"]["library_path"] = "./libVkLayer_khronos_validation.so"
    payload["validation/VkLayer_khronos_validation.json"] = json_bytes(layer)
    payload["validation/vk_layer_settings.txt"] = b""
    notices, dependencies = dependency_notices(build_dir, lock, validation)
    payload.update(notices)
    helpers = {"run_graphics_probe.py": ROOT / "tools/graphics_package/run_graphics_probe.py",
               **{f"_support/{name}": ROOT / "tools" / name for name in
                  ("graphics_probe.py", "process_runner.py", "toolchain_report.py")}}
    for name, path in helpers.items():
        payload[name] = regular_file(path, maximum=1024 * 1024)
    source = source_state()
    for path in (*packagers, *helpers.values()):
        source["input_sha256"][path.relative_to(ROOT).as_posix()] = sha256(path)
    payload.update({"provenance/dependencies.lock.json": lock_data,
                    "provenance/shader-manifest.json": json_bytes(shaders),
                    "provenance/validation-manifest.json": json_bytes(validation),
                    "provenance/dependency-closure.json": json_bytes(closure),
                    "provenance/source.json": json_bytes(source),
                    "provenance/toolchain.json": json_bytes(collect(build_dir)),
                    "provenance/probe-description.json": json_bytes(description)})
    payload["README.txt"] = (
        "Shark assignment 02 Ubuntu 24.04 x86-64 graphics probe fixture\n\n"
        "Requires Python 3.12+, glibc 2.39, Ubuntu libstdc++6/libgcc-s1, a desktop\n"
        "with X11 client libraries, and the selected GPU's installed Vulkan\n"
        "driver/ICD. Vendor ICDs and desktop system libraries are not redistributed.\n"
        "No compiler, Vulkan SDK, source checkout, DXC or spirv-val is required.\n"
        "Use an ordinary unprivileged desktop account; root execution is NOT_RUN.\n"
        "Run: python3 -I -B run_graphics_probe.py --mode inventory --output <fresh-path>\n"
        "Then: python3 -I -B run_graphics_probe.py --mode cases --device <UUID> --output <fresh-path>\n"
        "Full lifecycle adds --lifecycle-cycles 100 --device-cycles 20 --present-seconds 600 --timeout 1200.\n"
        "--mode describe requires no display or GPU. Each operation verifies the package,\n"
        "records bounded output and preserves failures. Linux child processes must not\n"
        "deliberately escape their private process group. SIGINT/SIGTERM are cleaned up;\n"
        "SIGKILL or machine loss cannot guarantee launcher cleanup.\n"
        "This unsigned package is a probe, not an editor, game exporter or release.\n"
        "Package PASS does not establish clean-machine or native GPU qualification.\n"
    ).encode("utf-8")
    if sha256(lock_path) != lock_digest or probe_source_digest() != source_digest:
        raise ValueError("Source/lock changed during packaging")
    if validation_tools(args.validation_manifest, lock_digest) != validation or shader_inputs(args.shader_dir, lock_digest) != shaders:
        raise ValueError("Shader/validation inputs changed during packaging")
    for name, path in {**binaries, **helpers}.items():
        regular_file(path, digest(payload[name]))
    for path, expected in packager_hashes.items():
        regular_file(path, expected)
    manifest = {"schema_version": 1, "assignment": "02d", "status": "PASS", "qualification": "NOT_RUN",
                "platform": "linux-x86_64", "configuration": "Release", "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                "dependency_lock_sha256": lock_digest, "probe_source_sha256": source_digest,
                "probe": {"schema_version": 2, "dependency_lock_sha256": lock_digest,
                          "probe_source_sha256": source_digest, "executable": "bin/GraphicsProbe"},
                "validation": {"api_version": validation["validation_layer"]["api_version"],
                               "manifest": "validation/VkLayer_khronos_validation.json",
                               "library": "validation/libVkLayer_khronos_validation.so"},
                "dependencies": dependencies, "prerequisites": {"os": "Ubuntu 24.04 x86-64", "python": "3.12+",
                    "system_import_allowlist": sorted(SYSTEM_LIBRARIES),
                    "desktop": "Native X11 session and OS X11 libraries dynamically loaded by the locked GLFW backend",
                    "gpu_driver": "Installed vendor Vulkan ICD; not bundled"}}
    return payload, manifest


def publish(output: Path, payload: dict[str, bytes], manifest: dict) -> dict:
    """Set executable permissions before the package becomes visible."""
    assert_no_links(output)
    if output.exists():
        raise ValueError("Package output must be fresh")
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = output.parent / (".linux-package-" + uuid.uuid4().hex)
    try:
        result = publish_payload(staging, payload, manifest)
        (staging / "bin/GraphicsProbe").chmod(0o755)
        if output.exists():
            raise ValueError("Package output appeared during preparation")
        staging.rename(output)
        return result
    finally:
        if staging.exists():
            assert_no_links(staging)
            if staging.resolve().parent != output.parent.resolve():
                raise ValueError("Refusing cleanup outside owned package staging directory")
            shutil.rmtree(staging)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("executable", "build-dir", "loader", "shader-dir", "validation-manifest", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    report = {"schema_version": 1, "assignment": "02d", "status": "FAIL", "qualification": "NOT_RUN"}
    try:
        assert_no_links(args.output)
        if args.output.exists():
            raise ValueError("Package output must be fresh")
        if platform.system() != "Linux" or platform.machine() not in ("x86_64", "AMD64"):
            report.update(status="NOT_RUN", reason="Linux x86-64 packaging requires a native Linux build and readelf")
            code = 3
        else:
            payload, manifest = prepare_payload(args)
            # Publish only after all inputs pass. The generic publisher also
            # creates the deliberately empty implicit-layer directory.
            manifest = publish(args.output.absolute(), payload, manifest)
            report.update(status="PASS", package_directory=str(args.output.absolute()),
                          manifest_sha256=sha256(args.output / "package-manifest.json"),
                          files=len(manifest["files"]), bytes=sum(item["bytes"] for item in manifest["files"].values()))
            code = 0
    except (OSError, ValueError, KeyError, TypeError, UnicodeError) as error:
        report["reason"] = str(error)
        code = 1
    print(json_bytes(report).decode("utf-8"), end="")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
