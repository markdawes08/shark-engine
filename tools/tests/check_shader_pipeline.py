"""Opt-in, GPU-free qualification with the cached locked DXC and SPIR-V validator.

This uses actual compiler output, separately from the controlled CPU unit fixtures.
All mutable fixtures and retained evidence are created in one fresh output tree.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from process_runner import run_process

ROOT = Path(__file__).resolve().parents[2]
PIPELINE = ROOT / "tools/shader_pipeline.py"
MODULES = ("shader_pipeline.py", "graphics_probe.py", "prepare_graphics_tools.py",
           "process_runner.py", "toolchain_report.py")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(directory: Path) -> dict:
    return {path.relative_to(directory).as_posix(): {
        "bytes": path.stat().st_size, "sha256": digest(path)}
        for path in sorted(directory.rglob("*")) if path.is_file()}


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def manifest(directory: Path) -> dict:
    return json.loads((directory / "shader-manifest.json").read_text(encoding="utf-8"))


def shader_hashes(directory: Path) -> dict[str, str]:
    return {row["id"]: digest(directory / row["file"]) for row in manifest(directory)["shaders"]}


def capture_tool_versions(harness: Harness) -> None:
    identity = harness.report["toolchain"]
    lock = json.loads((ROOT / "dependencies.lock.json").read_text(encoding="utf-8"))["graphics_tools"]["DXC"]
    executable = lock["artifacts"][identity["host"]]["executable"]
    compiler = ROOT / ".tools/graphics/dxc" / (lock["version"] + "-" + identity["host"]) / executable
    validator = Path(harness.report["validator"]["path"])
    rows = (("compiler", compiler, identity["compiler_files"][executable]),
            ("validator", validator, identity["validator_sha256"]))
    harness.report["compiler_locked_version"] = lock["version"]
    harness.report["compiler_source_revision"] = lock["source_revision"]
    harness.report["tool_version_commands"] = []
    for name, path, expected in rows:
        require(digest(path) == expected, f"{name}: binary identity differs from the completed build")
        result = run_process([str(path), "--version"], cwd=ROOT, timeout=30)
        harness.report["tool_version_commands"].append({"name": name, "sha256": expected, **asdict(result)})
        require(result.returncode == 0 and not result.timed_out and not result.cleanup_errors
                and digest(path) == expected, f"{name}: bounded version/identity query failed")


class Harness:
    def __init__(self, output: Path, receipt: Path, report: dict) -> None:
        self.output, self.receipt, self.report = output, receipt, report

    def invoke(self, name: str, arguments: list[str], *, expected_code: str | None = None,
               script: Path = PIPELINE, cwd: Path = ROOT, env: dict | None = None) -> dict:
        command = [sys.executable, "-B", str(script), *arguments]
        result = run_process(command, cwd=cwd, timeout=150, env=env)
        record = {"name": name, "expected_error_code": expected_code, **asdict(result)}
        self.report["commands"].append(record)
        require(not result.timed_out and not result.cleanup_errors,
                f"{name}: process deadline or cleanup failure")
        require(not result.stderr, f"{name}: unexpected stderr: {result.stderr}")
        try:
            parsed = json.loads(result.stdout)
        except ValueError as error:
            raise AssertionError(f"{name}: stdout is not one JSON result") from error
        record["result"] = parsed
        require(parsed.get("schema_version") == 1, f"{name}: missing or unsupported JSON result schema")
        if expected_code is None:
            require(result.returncode == 0 and parsed.get("status") == "PASS",
                    f"{name}: expected success, observed {parsed}")
        else:
            require(result.returncode != 0 and parsed.get("status") == "FAIL"
                    and parsed.get("error", {}).get("code") == expected_code,
                    f"{name}: expected {expected_code} rejection, observed {parsed}")
        record["expectation_status"] = "PASS"
        return parsed

    def build(self, name: str, recipe: Path, destination: Path,
              expected_code: str | None = None) -> dict:
        result = self.invoke(name, ["build", "--recipe", str(recipe), "--output", str(destination),
                                   "--validation-manifest", str(self.receipt)], expected_code=expected_code)
        require(not (destination / ".shader-build-lock").exists()
                and not list(destination.glob(".shader-staging-*")),
                f"{name}: builder retained its output lock or staging directory")
        return result

    def check(self, name: str, **details) -> None:
        self.report["checks"].append({"name": name, "status": "PASS", **details})


def fixtures(directory: Path) -> dict:
    (directory / "common").mkdir(parents=True)
    (directory / "main.hlsl").write_text(
        '#include "common/position.hlsli"\n'
        'float4 vsMain(uint vertex : SV_VertexID) : SV_Position {\n'
        '    return float4(positionFor(vertex), 0.25, 1.0);\n}\n', encoding="utf-8")
    (directory / "common/position.hlsli").write_text(
        '#include "scale.hlsli"\n'
        'float2 positionFor(uint vertex) {\n'
        '    return float2(float(vertex & 1), float(vertex >> 1)) * shaderScale();\n}\n',
        encoding="utf-8")
    (directory / "common/scale.hlsli").write_text(
        'float shaderScale() { return 0.5; }\n', encoding="utf-8")
    (directory / "independent.hlsl").write_text(
        'float4 psMain() : SV_Target0 { return float4(0.125, 0.25, 0.5, 1.0); }\n',
        encoding="utf-8")
    definition = {"schema_version": 1, "convention": "shark-vulkan-1", "shaders": [
        {"id": "dependent-vs", "source": "main.hlsl", "entry": "vsMain", "stage": "vertex", "bindings": []},
        {"id": "independent-ps", "source": "independent.hlsl", "entry": "psMain", "stage": "pixel", "bindings": []}]}
    write_json(directory / "recipe.json", definition)
    return definition


def qualify(harness: Harness, production_recipe: Path | None) -> None:
    output = harness.output
    source, artifacts = output / "fixture sources \u03a9", output / "shader artifacts \u03a9"
    definition = fixtures(source)
    recipe = source / "recipe.json"
    harness.report["initial_fixture_files"] = inventory(source)
    first = harness.build("initial-build", recipe, artifacts)
    require(first.get("disposition") == "BUILT" and first.get("shaders") == 2,
            "Initial build must compile both fixture shaders")
    initial_inventory, initial_hashes = inventory(artifacts), shader_hashes(artifacts)
    dependencies = manifest(artifacts)["inputs"]["sources"]
    require(set(dependencies) == {"main.hlsl", "common/position.hlsli", "common/scale.hlsli", "independent.hlsl"},
            "The generation must record the exact transitive source closure")
    harness.check("real-compile-and-nested-dependency-closure", generation=first["generation"],
                  dependencies=dependencies, artifacts=initial_inventory)
    harness.report["toolchain"] = manifest(artifacts)["inputs"]["toolchain"]
    harness.report["validator"] = json.loads(harness.receipt.read_text(encoding="utf-8"))["tools"]["spirv_val"]
    capture_tool_versions(harness)

    second = harness.build("unchanged-repeat", recipe, artifacts)
    require(second.get("disposition") == "CACHED" and second["generation"] == first["generation"]
            and inventory(artifacts) == initial_inventory, "Unchanged build did not preserve verified output")
    harness.check("unchanged-inputs-reuse-identical-generation")
    cold = output / "independent-cold-build"
    rebuilt = harness.build("independent-cold-build", recipe, cold)
    require(rebuilt.get("disposition") == "BUILT" and rebuilt["generation"] == first["generation"]
            and inventory(cold) == initial_inventory, "Independent cold build is not byte-identical")
    harness.check("independent-cold-build-byte-identical")

    (source / "unreferenced.hlsl").write_text("deliberately invalid unused shader text\n", encoding="utf-8")
    unrelated = harness.build("unreferenced-source-edit", recipe, artifacts)
    require(unrelated.get("disposition") == "CACHED" and inventory(artifacts) == initial_inventory,
            "Unreferenced file changed the shader generation")
    harness.check("unreferenced-source-does-not-invalidate")
    (source / "common/scale.hlsli").write_text("float shaderScale() { return 0.75; }\n", encoding="utf-8")
    changed = harness.build("nested-include-edit", recipe, artifacts)
    changed_hashes = shader_hashes(artifacts)
    require(changed.get("disposition") == "BUILT" and changed["generation"] != first["generation"]
            and changed_hashes["dependent-vs"] != initial_hashes["dependent-vs"]
            and changed_hashes["independent-ps"] == initial_hashes["independent-ps"],
            "Nested include edit must change its shader while preserving independent shader bytes")
    harness.check("nested-include-invalidates-generation", before=initial_hashes, after=changed_hashes,
                  generation=changed["generation"], incremental_scope="Whole generation rebuilt")

    good_inventory = inventory(artifacts)
    scale = source / "common/scale.hlsli"
    good_scale = scale.read_bytes()
    scale.write_text("float shaderScale() { this is not HLSL; }\n", encoding="utf-8")
    harness.build("malformed-hlsl", recipe, artifacts, "shader.compile")
    require(inventory(artifacts) == good_inventory, "Failed compiler changed the last-good artifact tree")
    scale.write_bytes(good_scale)
    missing = source / "common/missing-scale.hlsli"
    scale.rename(missing)
    harness.build("missing-nested-include", recipe, artifacts, "shader.source")
    require(inventory(artifacts) == good_inventory, "Missing include changed the last-good artifact tree")
    missing.rename(scale)
    harness.check("compile-and-source-failures-preserve-last-good-generation")

    invalid = json.loads(json.dumps(definition))
    invalid["schema_version"] = 99
    write_json(recipe, invalid)
    harness.build("unsupported-recipe-version", recipe, artifacts, "shader.version")
    invalid = json.loads(json.dumps(definition))
    invalid["shaders"][0]["bindings"] = [{"kind": "constant_buffer", "slot": 0}]
    write_json(recipe, invalid)
    harness.build("binding-contract-mismatch", recipe, artifacts, "shader.layout")
    write_json(recipe, definition)
    require(inventory(artifacts) == good_inventory, "Recipe/layout rejection changed the last-good artifact tree")
    harness.check("version-and-binding-mismatch-rejected-without-publication")

    # This copied verifier has no repository lock, source tree, cached compilers,
    # validation receipt or layer files. The local Python interpreter is retained.
    relocated = output / "relocated standalone verifier"
    verifier_tools = relocated / "tools"
    verifier_tools.mkdir(parents=True)
    for name in MODULES:
        shutil.copyfile(ROOT / "tools" / name, verifier_tools / name)
    package = relocated / "artifacts"
    shutil.copytree(artifacts, package)
    require(source.resolve().is_relative_to(output.resolve()), "Owned fixture source path escaped output")
    source.rename(output / "source-unavailable")
    environment = {key: value for key, value in os.environ.items()
                   if key.upper() in {"SYSTEMROOT", "WINDIR", "TEMP", "TMP", "HOME", "LANG"}}
    environment.update(PYTHONNOUSERSITE="1", PYTHONDONTWRITEBYTECODE="1",
                       PATH=str(Path(os.environ["SystemRoot"]) / "System32") if os.name == "nt" else "/usr/bin:/bin")
    for tool in ("dxc", "spirv-val"):
        require(shutil.which(tool, path=environment["PATH"]) is None,
                f"Relocation test PATH unexpectedly discovers {tool}")
    verified = harness.invoke("relocated-source-and-tool-free-verify", ["verify", "--output", str(package)],
                              script=verifier_tools / "shader_pipeline.py", cwd=relocated, env=environment)
    require(verified["generation"] == changed["generation"] and inventory(package) == good_inventory,
            "Relocation verification changed or selected the wrong generation")
    harness.check("relocated-verifier-needs-no-source-lock-compiler-or-validator",
                  verifier_files=inventory(verifier_tools), path=environment["PATH"],
                  limits="Current-machine isolation with installed Python, not a clean physical machine")

    binary = package / manifest(package)["shaders"][0]["file"]
    good_binary = binary.read_bytes()
    binary.write_bytes(good_binary[:-1] + bytes([good_binary[-1] ^ 0xFF]))
    harness.invoke("corrupt-artifact", ["verify", "--output", str(package)], expected_code="shader.artifact")
    binary.write_bytes(good_binary)
    binary.unlink()
    harness.invoke("missing-artifact", ["verify", "--output", str(package)], expected_code="shader.artifact")
    binary.write_bytes(good_binary)
    harness.invoke("restored-artifact", ["verify", "--output", str(package)])
    require(inventory(package) == good_inventory, "Restored artifact differs from the qualified output")
    harness.check("corrupt-and-missing-artifacts-rejected")

    if production_recipe is not None:
        production = output / "production-artifacts"
        result = harness.build("checked-in-production-recipe", production_recipe, production)
        require(result.get("disposition") == "BUILT", "Production recipe was not compiled")
        harness.invoke("checked-in-production-artifact-verify", ["verify", "--output", str(production)])
        harness.check("checked-in-recipe-compiles-and-validates", recipe=str(production_recipe),
                      recipe_sha256=digest(production_recipe), generation=result["generation"],
                      artifacts=inventory(production))

    compute_source = output / "compute-source"
    compute_source.mkdir()
    (compute_source / "compute.hlsl").write_text(
        '[[vk::binding(384, 0)]] RWStructuredBuffer<uint> outputData : register(u0);\n'
        '[numthreads(4, 1, 1)]\n'
        'void csMain(uint3 dispatchId : SV_DispatchThreadID) {\n'
        '    outputData[dispatchId.x] = dispatchId.x + 1;\n}\n', encoding="utf-8")
    compute_recipe = compute_source / "recipe.json"
    write_json(compute_recipe, {"schema_version": 1, "convention": "shark-vulkan-1", "shaders": [
        {"id": "storage-cs", "source": "compute.hlsl", "entry": "csMain", "stage": "compute",
         "bindings": [{"kind": "storage", "slot": 0}]}]})
    compute_artifacts = output / "compute-artifacts"
    result = harness.build("compute-storage-recipe", compute_recipe, compute_artifacts)
    require(result.get("disposition") == "BUILT" and result.get("shaders") == 1,
            "Compute storage shader was not compiled")
    verified = harness.invoke("compute-storage-artifact-verify", ["verify", "--output", str(compute_artifacts)])
    require(verified["generation"] == result["generation"], "Compute verification selected the wrong generation")
    harness.check("compute-storage-shader-compiles-and-validates", source=inventory(compute_source),
                  generation=result["generation"], artifacts=inventory(compute_artifacts),
                  limits="Offline stage/binding validation only; no GPU dispatch")
    harness.report["final_artifacts"] = inventory(artifacts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validation-manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="Fresh output directory; existing directories are rejected")
    parser.add_argument("--recipe", type=Path, help="Also compile and validate this checked-in production recipe")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error("--output must be a fresh directory")
    output.mkdir(parents=True)
    report = {"schema_version": 1, "assignment": "06a", "status": "FAIL", "qualification": "NOT_RUN",
              "scope": "Offline shader toolchain only; no device/rendering/05c qualification",
              "timestamp_utc": datetime.now(timezone.utc).isoformat(), "os": platform.platform(),
              "python": platform.python_version(), "commands": [], "checks": []}
    try:
        source_hashes = {name: digest(ROOT / "tools" / name) for name in MODULES}
        report.update(tool_source_sha256=source_hashes, harness_sha256=digest(Path(__file__)),
                      validation_manifest=str(args.validation_manifest.resolve()),
                      validation_manifest_sha256=digest(args.validation_manifest.resolve()),
                      dependency_lock_sha256=digest(ROOT / "dependencies.lock.json"))
        qualify(Harness(output, args.validation_manifest.resolve(), report),
                args.recipe.resolve() if args.recipe else None)
        require(source_hashes == {name: digest(ROOT / "tools" / name) for name in MODULES},
                "Tool implementation changed during qualification; rerun on a stable worktree")
        report["status"] = "PASS"
        returncode = 0
    except (AssertionError, OSError, ValueError, KeyError, TypeError) as error:
        report["error"] = str(error)
        returncode = 1
    write_json(output / "summary.json", report)
    print(json.dumps({"status": report["status"], "checks": len(report["checks"]),
                      "commands": len(report["commands"]), "summary": str(output / "summary.json"),
                      "error": report.get("error")}, sort_keys=True))
    return returncode


if __name__ == "__main__":
    raise SystemExit(main())
