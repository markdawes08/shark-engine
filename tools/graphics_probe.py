"""Run graphics cases with bounded execution and retain verified evidence."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import time

from process_runner import ProcessResult, run_process
from toolchain_report import ROOT, collect, command_output


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate report key: {key}")
        result[key] = value
    return result


def reject_constant(value: str) -> None:
    raise ValueError(f"Nonfinite JSON value: {value}")


def finite_float(value: str) -> float:
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"Nonfinite JSON number: {value}")
    return number


def probe_source_digest() -> str:
    paths = sorted({"probes/graphics/CMakeLists.txt", *(path.relative_to(ROOT).as_posix()
                    for path in (ROOT / "probes/graphics").rglob("*")
                    if path.is_file() and path.suffix in (".cpp", ".hpp", ".hlsl", ".manifest"))})
    text = "".join(f"{path}\n{sha256(ROOT / path)}\n" for path in paths)
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def read_json(path: Path) -> dict:
    if not path.is_file() or path.stat().st_size > 8 * 1024 * 1024:
        raise ValueError(f"Missing or oversized evidence manifest: {path}")
    report = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object,
                        parse_constant=reject_constant, parse_float=finite_float)
    if not isinstance(report, dict):
        raise ValueError(f"Evidence manifest must be an object: {path}")
    return report


def checked_file(path: Path, expected: object, *, maximum: int = 256 * 1024 * 1024) -> str:
    if (not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected)
            or not path.is_file() or path.is_symlink() or path.stat().st_size > maximum
            or sha256(path) != expected):
        raise ValueError(f"Evidence file missing, changed or outside its byte budget: {path}")
    return expected


def validation_tools(manifest_path: Path, lock_digest: str) -> dict:
    """Bind tools/layer selection to the locked preparation receipt, never PATH."""
    manifest = read_json(manifest_path)
    if (manifest.get("schema_version") != 1 or type(manifest.get("schema_version")) is not int
            or manifest.get("status") != "PASS" or manifest.get("dependency_lock_sha256") != lock_digest):
        raise ValueError("Validation tools receipt is not passing/current; prepare the locked tools again")
    try:
        validator = manifest["tools"]["spirv_val"]
        layer = manifest["validation_layer"]
        for record, path_key, hash_key in ((validator, "path", "sha256"),
                                          (layer, "manifest_path", "manifest_sha256"),
                                          (layer, "library_path", "library_sha256")):
            if not isinstance(record[path_key], str) or not Path(record[path_key]).is_absolute():
                raise ValueError("Validation receipt tool paths must be absolute")
            checked_file(Path(record[path_key]), record[hash_key])
        layer_json = read_json(Path(layer["manifest_path"]))
        declaration = layer_json["layer"]
        declared_library = Path(declaration["library_path"])
        if not declared_library.is_absolute():
            declared_library = Path(layer["manifest_path"]).parent / declared_library
        if (layer["name"] != "VK_LAYER_KHRONOS_validation" or declaration["name"] != layer["name"]
                or declaration["api_version"] != layer["api_version"]
                or declared_library.resolve() != Path(layer["library_path"]).resolve()):
            raise ValueError("Layer declaration disagrees with the pinned validation receipt")
    except (KeyError, TypeError) as error:
        raise ValueError(f"Incomplete validation tools receipt: {error}") from error
    return manifest


SHADERS = {"fixture_vs.spv": ("vsMain", "vs_6_0"), "fixture_ps.spv": ("psMain", "ps_6_0"),
           "compute_cs.spv": ("csMain", "cs_6_0"), "sample_ps.spv": ("sampleMain", "ps_6_0")}

# Minimum measured values and maximum ADR tolerances. Coverage cannot shrink to
# one convenient pixel or silently widen depth/integer tolerances and still pass.
REQUIRED_CHECKS = {
    "P02-02": {"textured_depth_winding_uv": (12544, 1 / 255, 0),
               "conventional_depth_readback": (3136, 1e-5, 0)},
    "P02-03": {"integer_buffer_transform": (256, 0, 0),
               "rgba16f_storage_write": (1024, 0.002, 0.002),
               "compute_to_graphics_sample": (1024, 0.002, 0.002)},
    "P02-04": {"hdr_attachment_blending": (64, 0.002, 0.002),
               "unorm_attachment_blending": (64, 1 / 255, 0),
               "srgb_unorm_transfer_roundtrip": (32, 0, 0),
               "srgb_decode_linear_filter": (64, 0.002, 0.002),
               "unorm_linear_filter": (64, 0.002, 0.002),
               "cube_six_faces_three_mips": (1152, 0.002, 0.002),
               "depth_array_twelve_layers_readback": (768, 1e-5, 0),
               "depth_array_twelve_layers_sample": (768, 1e-5, 0),
               "hdr_linear_filter": (128, 0.002, 0.002)},
}

REQUIRED_ARTIFACTS = {
    "P02-02": {"graphics.rgba8": ("rgba8", 64, 64, 1), "graphics.d32": ("d32f-le", 64, 64, 1)},
    "P02-03": {"compute.u32": ("u32-le", 16, 16, 1),
               "compute-storage.rgba16f": ("rgba16f-le", 16, 16, 1),
               "compute-consumed.rgba16f": ("rgba16f-le", 16, 16, 1)},
    "P02-04": {"formats-hdr.rgba16f": ("rgba16f-le", 4, 4, 1),
               "formats-unorm.rgba8": ("rgba8", 4, 4, 1),
               "formats-srgb-upload.rgba8": ("rgba8", 2, 2, 1),
               "formats-linear-upload.rgba8": ("rgba8", 2, 2, 1),
               "formats-samples.rgba16f": ("rgba16f-le", 4, 4, 34),
               "formats-depth-crops.d32": ("d32f-le", 8, 8, 12)},
}


def shader_inputs(directory: Path, lock_digest: str) -> dict:
    manifest_path = directory / "shader-manifest.json"
    manifest = read_json(manifest_path)
    if (type(manifest.get("schema_version")) is not int or manifest["schema_version"] != 1
            or manifest.get("status") != "PASS" or manifest.get("dependency_lock_sha256") != lock_digest
            or manifest.get("invalid_shader_rejected") is not True
            or manifest.get("invalid_spirv_rejected") is not True):
        raise ValueError("Shader manifest is not a passing/current independently validated preparation")
    shaders = manifest.get("shaders")
    if (not isinstance(shaders, list) or len(shaders) != len(SHADERS)
            or any(not isinstance(item, dict) or not isinstance(item.get("file"), str) for item in shaders)
            or {item["file"] for item in shaders} != set(SHADERS)):
        raise ValueError("Shader manifest must contain each expected executable fixture exactly once")
    for item in shaders:
        entry, profile = SHADERS[item["file"]]
        if (item.get("source") != "probes/graphics/shaders/cases.hlsl"
                or item.get("entry") != entry or item.get("profile") != profile
                or item.get("validation_status") != "PASS" or item.get("compile_status") != "PASS"
                or type(item.get("bytes")) is not int or not 20 <= item["bytes"] <= 4 * 1024 * 1024):
            raise ValueError("Shader manifest contains unsupported compilation/validation evidence")
        checked_file(ROOT / item["source"], item.get("source_sha256"), maximum=4 * 1024 * 1024)
        binary = directory / item["file"]
        checked_file(binary, item.get("sha256"), maximum=4 * 1024 * 1024)
        if binary.stat().st_size != item["bytes"]:
            raise ValueError("Shader byte count does not match prepared SPIR-V")
    try:
        for key in ("compiler", "validator"):
            checked_file(Path(manifest[key]["path"]), manifest[key]["sha256"])
    except (KeyError, TypeError) as error:
        raise ValueError(f"Shader manifest lacks compiler/validator identities: {error}") from error
    return {"manifest_path": str(manifest_path), "manifest_sha256": sha256(manifest_path), **manifest}


def inspect_result(result: ProcessResult, mode: str, lock_digest: str, source_digest: str,
                   device: str | None = None, validation: str = "required", *,
                   artifacts_dir: Path | None = None, lifecycle_cycles: int = 2,
                   device_cycles: int = 1, present_seconds: int = 0,
                   inject_failure: str = "none") -> tuple[dict | None, list[str]]:
    """Never turn a timeout, stale build or malformed/contradictory report into a pass."""
    errors = list(result.cleanup_errors)
    if result.timed_out:
        errors.append("Probe deadline exceeded; the owned process tree was terminated")
    try:
        report = json.loads(result.stdout, object_pairs_hook=unique_object,
                            parse_constant=reject_constant, parse_float=finite_float)
    except (ValueError, TypeError) as error:
        return None, [*errors, f"Invalid probe JSON: {error}"]
    if not isinstance(report, dict):
        return None, [*errors, "Probe report must be an object"]
    if type(report.get("schema_version")) is not int or report["schema_version"] != 2:
        errors.append("Unsupported probe report schema")
    if report.get("mode") != mode:
        errors.append("Report mode does not match the command")
    if report.get("dependency_lock_sha256") != lock_digest:
        errors.append("Probe was built with a different dependency lock; reconfigure and rebuild")
    if report.get("probe_source_sha256") != source_digest:
        errors.append("Probe sources changed since the build; reconfigure and rebuild")
    if report.get("qualification") != "NOT_RUN":
        errors.append("A local probe run cannot claim complete assignment 02 hardware qualification")
    cases = report.get("cases")
    if (not isinstance(cases, list) or len(cases) != 8
            or any(not isinstance(case, dict) or not isinstance(case.get("id"), str)
                   or case.get("status") not in ("PASS", "FAIL", "NOT_RUN") for case in cases)
            or {case.get("id") for case in cases} != {f"P02-{number:02d}" for number in range(1, 9)}):
        errors.append("Report must retain exactly all eight assignment 02 cases")
        cases = []
    if any(case["status"] != "NOT_RUN" for case in cases) and mode != "cases":
        errors.append("Setup/description modes cannot claim executable case qualification")
    if validation == "off" and any(case["status"] != "NOT_RUN" for case in cases):
        errors.append("Diagnostic validation-off runs must leave all case qualifications NOT_RUN")
    if any(case["status"] != "NOT_RUN" for case in cases if case["id"] in ("P02-07", "P02-08")):
        errors.append("A single local run cannot qualify packaging or the aggregate failure-path gate")
    expected_status = {0: "PASS", 1: "FAIL", 2: "FAIL", 3: "NOT_RUN"}.get(result.returncode)
    if expected_status is None or report.get("status") != expected_status:
        errors.append("Probe status does not agree with its process exit code")
    if type(report.get("exit_code")) is not int or report["exit_code"] != result.returncode:
        errors.append("Report exit_code does not agree with the process")
    if result.returncode == 0:
        if inject_failure != "none":
            errors.append("An injected failure must not return success")
        for field in ("validation_warnings", "validation_errors"):
            if type(report.get(field)) is not int or report[field] != 0:
                errors.append(f"A passing setup must report zero {field}")
        expected_stages = [] if mode == "describe" else ["loader", "instance", "surface", "inventory"]
        if mode in ("smoke", "cases"):
            expected_stages += ["device", "nvrhi_device_and_validation", "swapchain"]
        if mode == "cases":
            expected_stages += ["readback_cases", "lifecycle"]
        if mode != "describe":
            expected_stages += ["teardown"]
        if report.get("stages") != [{"name": name, "status": "PASS"} for name in expected_stages]:
            errors.append("Passing report lacks the actual ordered setup/teardown stages")
        devices = report.get("devices")
        if not isinstance(devices, list) or any(not isinstance(item, dict) for item in devices):
            errors.append("Invalid device inventory")
            devices = []
        if mode == "describe":
            if devices or report.get("selected_device_uuid") is not None:
                errors.append("Description must not select or enumerate devices")
        elif not devices:
            errors.append("Passing device operation requires a nonempty inventory")
        if report.get("vulkan_validation_enabled") is not (mode in ("smoke", "cases") and validation == "required"):
            errors.append("Vulkan validation state does not match the requested operation")
        if report.get("nvrhi_validation_enabled") is not (mode in ("smoke", "cases")):
            errors.append("NVRHI validation state does not match the operation")
        if mode in ("smoke", "cases"):
            selected = [item for item in devices if item.get("uuid") == device]
            if (not device or report.get("selected_device_uuid") != device or len(selected) != 1
                    or selected[0].get("reported_profile_supported") is not True):
                errors.append("Smoke did not select the requested capable device uniquely")
            swapchain = report.get("swapchain")
            if not isinstance(swapchain, dict):
                errors.append("Passing smoke requires swapchain evidence")
            else:
                extent = swapchain.get("extent")
                if (type(swapchain.get("image_count")) is not int or swapchain["image_count"] < 1
                        or not isinstance(extent, list) or len(extent) != 2
                        or any(type(value) is not int or value < 1 for value in extent)
                        or swapchain.get("present_mode") != "FIFO"
                        or type(swapchain.get("presented_frames")) is not int or swapchain["presented_frames"] < 0
                        or (mode == "smoke" and swapchain["presented_frames"] != 0)):
                    errors.append("Invalid setup swapchain evidence")
        if mode == "cases":
            errors.extend(inspect_cases(report, cases, artifacts_dir, validation,
                                        lifecycle_cycles, device_cycles, present_seconds))
    elif result.returncode in (1, 2, 3):
        error = report.get("error")
        if (not isinstance(error, dict) or not isinstance(error.get("code"), str) or not error["code"]
                or not isinstance(error.get("message"), str) or not error["message"]):
            errors.append("Failed/unavailable operations need an actionable structured error")
        # A mismatch is useful failure evidence. Preserve the actual binary
        # identities without treating failed comparisons as successful cases.
        if mode == "cases" and artifacts_dir is not None:
            for case in cases:
                if case.get("artifacts"):
                    if not isinstance(case["artifacts"], list):
                        errors.append("Failed case artifact inventory must be a list")
                        continue
                    try:
                        case["verified_artifacts"] = inspect_artifacts(case["artifacts"], artifacts_dir)
                    except (OSError, ValueError) as error:
                        errors.append(str(error))
    return report, errors


def inspect_cases(report: dict, cases: list[dict], artifacts_dir: Path | None, validation: str,
                  lifecycle_cycles: int, device_cycles: int, present_seconds: int) -> list[str]:
    errors = []
    enabled = validation == "required"
    configuration = report.get("validation_configuration")
    if (not isinstance(configuration, dict) or configuration.get("core") is not enabled
            or configuration.get("synchronization") is not enabled or configuration.get("gpu_assisted") is not False
            or configuration.get("disabled_checks") != []):
        errors.append("Cases require the declared core/synchronization validation configuration")
    by_id = {case["id"]: case for case in cases}
    for identifier in ("P02-01", "P02-02", "P02-03", "P02-04", "P02-06"):
        case = by_id.get(identifier, {})
        expected = "PASS" if validation == "required" else "NOT_RUN"
        if case.get("status") != expected:
            errors.append(f"Passing cases run lacks expected {identifier} qualification state")
        if identifier in ("P02-02", "P02-03", "P02-04"):
            if case.get("execution_status") != "PASS":
                errors.append(f"Passing cases run lacks executed {identifier} evidence")
            checks = case.get("checks")
            if not isinstance(checks, list) or not checks:
                errors.append(f"{identifier} has no measured readback checks")
            else:
                names = set()
                for check in checks:
                    if not isinstance(check, dict):
                        errors.append(f"{identifier} check must be an object")
                        continue
                    name = check.get("name")
                    if not isinstance(name, str) or not name or name in names:
                        errors.append(f"{identifier} check names must be unique and nonempty")
                    elif isinstance(name, str):
                        names.add(name)
                    numeric = ("max_absolute_error", "absolute_tolerance", "relative_tolerance")
                    if (check.get("status") != "PASS" or type(check.get("compared_values")) is not int
                            or check["compared_values"] <= 0
                            or any(type(check.get(key)) not in (int, float) or check[key] < 0 for key in numeric)
                            or (type(check.get("absolute_tolerance")) in (int, float)
                                and check["absolute_tolerance"] > 1 / 255 + 1e-12)
                            or (type(check.get("relative_tolerance")) in (int, float)
                                and check["relative_tolerance"] > 0.002)):
                        errors.append(f"{identifier} contains invalid, failed or over-tolerance readback checks")
                    if type(check.get("failed_values")) is not int or check["failed_values"] != 0:
                        errors.append(f"{identifier} contains failed comparison values")
                    if "max_tolerance_ratio" in check:
                        ratio = check["max_tolerance_ratio"]
                        if type(ratio) not in (int, float) or not 0 <= ratio <= 1:
                            errors.append(f"{identifier} exceeded its reference tolerance")
                    else:
                        errors.append(f"{identifier} lacks its measured tolerance ratio")
                    if (all(type(check.get(key)) in (int, float) for key in numeric)
                          and check["relative_tolerance"] == 0
                          and check["max_absolute_error"] > check["absolute_tolerance"]):
                        errors.append(f"{identifier} exceeded its absolute tolerance")
                    minimum, absolute, relative = REQUIRED_CHECKS[identifier].get(name, (0, 0, 0)) if isinstance(name, str) else (0, 0, 0)
                    if (type(check.get("compared_values")) is int and check["compared_values"] < minimum
                            or type(check.get("absolute_tolerance")) in (int, float) and check["absolute_tolerance"] > absolute + 1e-12
                            or type(check.get("relative_tolerance")) in (int, float) and check["relative_tolerance"] > relative):
                        errors.append(f"{identifier}/{name} reduced required coverage or widened its ADR tolerance")
                if names != set(REQUIRED_CHECKS[identifier]):
                    errors.append(f"{identifier} must report every required named readback check exactly once")
            artifacts = case.get("artifacts")
            if not isinstance(artifacts, list) or not artifacts or artifacts_dir is None:
                errors.append(f"{identifier} lacks retained readback artifacts")
            else:
                try:
                    case["verified_artifacts"] = inspect_artifacts(artifacts, artifacts_dir)
                    actual = {item["path"]: (item["format"], item["width"], item["height"], item["layers"])
                              for item in case["verified_artifacts"]}
                    if actual != REQUIRED_ARTIFACTS[identifier]:
                        errors.append(f"{identifier} readback artifacts do not cover the fixed fixture dimensions")
                except (OSError, ValueError) as error:
                    errors.append(str(error))
    lifecycle = report.get("lifecycle")
    if not isinstance(lifecycle, dict):
        return [*errors, "Passing cases run requires measured lifecycle evidence"]
    for requested, completed, expected in (
            ("requested_resize_cycles", "completed_resize_cycles", lifecycle_cycles),
            ("requested_device_cycles", "completed_device_cycles", device_cycles)):
        if (type(lifecycle.get(requested)) is not int or lifecycle[requested] != expected
                or type(lifecycle.get(completed)) is not int or lifecycle[completed] != expected):
            errors.append("Requested and completed lifecycle counts disagree")
    for field in ("zero_extent_policy_checks", "observed_zero_framebuffer_extents", "minimize_restore_cycles", "presented_frames"):
        if type(lifecycle.get(field)) is not int or lifecycle[field] < 0:
            errors.append(f"Lifecycle requires a nonnegative measured {field}")
    if type(lifecycle.get("minimize_restore_cycles")) is not int or lifecycle["minimize_restore_cycles"] != lifecycle_cycles:
        errors.append("Lifecycle did not complete every requested minimize/restore operation")
    if (type(lifecycle.get("observed_zero_framebuffer_extents")) is int
            and lifecycle["observed_zero_framebuffer_extents"] > lifecycle_cycles):
        errors.append("Observed zero framebuffer extents exceed the minimize/restore cycles")
    minimum_frames = 2 * lifecycle_cycles + device_cycles + (1 if present_seconds > 0 else 0)
    if type(lifecycle.get("presented_frames")) is not int or lifecycle["presented_frames"] < minimum_frames:
        errors.append("Lifecycle frame count cannot cover the requested resize, device and presentation operations")
    if (type(lifecycle.get("requested_present_seconds")) is not int
            or lifecycle["requested_present_seconds"] != present_seconds
            or type(lifecycle.get("present_elapsed_seconds")) not in (int, float)
            or lifecycle["present_elapsed_seconds"] < present_seconds):
        errors.append("Presentation duration evidence does not meet the requested duration")
    qualified = (validation == "required" and lifecycle_cycles == 100 and device_cycles == 20 and present_seconds == 600
                 and type(lifecycle.get("observed_zero_framebuffer_extents")) is int
                 and lifecycle["observed_zero_framebuffer_extents"] > 0
                 and type(lifecycle.get("minimize_restore_cycles")) is int
                 and lifecycle["minimize_restore_cycles"] >= 100
                 and type(lifecycle.get("presented_frames")) is int and lifecycle["presented_frames"] > 0)
    if lifecycle.get("fully_qualified") is not qualified:
        errors.append("Lifecycle full qualification flag disagrees with measured threshold evidence")
    expected = "PASS" if qualified and validation == "required" else "NOT_RUN"
    if by_id.get("P02-05", {}).get("status") != expected:
        errors.append("P02-05 cannot pass without the complete observed lifecycle thresholds")
    return errors


def inspect_artifacts(artifacts: list, directory: Path) -> list[dict]:
    root = directory.resolve()
    verified = []
    names = set()
    pixel_bytes = {"rgba8": 4, "rgba16f-le": 8, "d32f-le": 4, "u32-le": 4}
    total = 0
    for artifact in artifacts:
        if not isinstance(artifact, dict) or not isinstance(artifact.get("path"), str):
            raise ValueError("Readback artifact needs an owned relative path")
        name = artifact["path"]
        relative = Path(name)
        if (name in names or "\\" in name or ":" in name or relative.is_absolute()
                or ".." in relative.parts or not name or "\0" in name):
            raise ValueError("Readback artifact path is duplicate, absolute or escaping")
        names.add(name)
        path = directory / relative
        if (not path.resolve().is_relative_to(root) or path.is_symlink() or not path.is_file()
                or any(parent.is_symlink() for parent in path.parents if parent.is_relative_to(root))):
            raise ValueError("Readback artifact is missing or escapes its owned directory")
        if (not isinstance(artifact.get("format"), str) or artifact["format"] not in pixel_bytes
                or any(type(artifact.get(key)) is not int or artifact[key] <= 0
                       for key in ("width", "height", "layers", "bytes"))):
            raise ValueError("Readback artifact dimensions or format are invalid")
        expected = artifact["width"] * artifact["height"] * artifact["layers"] * pixel_bytes[artifact["format"]]
        total += expected
        if expected != artifact["bytes"] or expected != path.stat().st_size or total > 64 * 1024 * 1024:
            raise ValueError("Readback artifact byte count or budget is invalid")
        verified.append({**artifact, "sha256": sha256(path)})
    return verified


def source_state() -> dict:
    """Record reviewed inputs, including uncommitted sources, without capturing secrets."""
    paths = [ROOT / "CMakeLists.txt", ROOT / "CMakePresets.json", ROOT / "dependencies.lock.json"]
    for directory in ("probes/graphics", "cmake", "tools/graphics_validation"):
        paths.extend(path for path in (ROOT / directory).rglob("*") if path.is_file())
    paths.extend(ROOT / "tools" / name for name in
                 ("graphics_probe.py", "prepare_graphics_tools.py", "prepare_graphics_validation.py", "process_runner.py",
                  "windows_unelevated.py"))
    paths.extend((ROOT / "tools/tests").glob("test_graphics*.py"))
    return {"git_revision": command_output(["git", "rev-parse", "HEAD"]),
            "git_status": command_output(["git", "status", "--short"]),
            "input_sha256": {path.relative_to(ROOT).as_posix(): sha256(path)
                             for path in sorted(set(paths)) if path.is_file()}}


class Unavailable(RuntimeError):
    """A required graphics tool/device input is absent, not a passing test."""


def check_layer_process() -> dict:
    """The pinned loader deliberately ignores layer paths at high integrity."""
    if sys.platform != "win32":
        return {"platform": sys.platform, "windows_integrity_check": "NOT_APPLICABLE"}
    from windows_unelevated import process_security
    security = process_security()
    if security["integrity_rid"] >= 0x3000:
        raise Unavailable("Windows high-integrity processes cannot use the pinned VK_LAYER_PATH: the Vulkan loader "
                          "ignores it. Run from a normal terminal, or use the same-user unelevated wrapper: "
                          "python tools/windows_unelevated.py --timeout 180 -- python tools/graphics_probe.py <arguments>")
    return security


def layer_environment(manifest: dict | None, settings_path: Path | None = None,
                      implicit_directory: Path | None = None) -> tuple[dict[str, str], dict]:
    environment = dict(os.environ)
    controlled = ("VK_INSTANCE_LAYERS", "VK_LAYER_PATH", "VK_ADD_LAYER_PATH", "VK_LAYER_SETTINGS_PATH",
                  "VK_LOADER_LAYERS_ENABLE", "VK_LOADER_LAYERS_DISABLE", "VK_LOADER_LAYERS_ALLOW",
                  "VK_LAYER_ENABLES", "VK_LAYER_DISABLES", "VK_IMPLICIT_LAYER_PATH", "VK_ADD_IMPLICIT_LAYER_PATH")
    removed = sorted(key for key in environment if key in controlled
                     or key.startswith(("VK_LAYER_", "VK_KHRONOS_VALIDATION_")))
    for key in removed:
        environment.pop(key, None)
    # Override implicit discovery with an owned empty directory. Unlike the
    # loader's DISABLE filter this does not emit forced-disabled-layer warnings.
    # No message severity/filter configuration is relaxed.
    if implicit_directory is not None:
        environment["VK_IMPLICIT_LAYER_PATH"] = str(implicit_directory)
    if manifest is not None:
        environment["VK_LAYER_PATH"] = str(Path(manifest["validation_layer"]["manifest_path"]).parent)
    if settings_path is not None:
        environment["VK_LAYER_SETTINGS_PATH"] = str(settings_path)
    return environment, {"removed_ambient_keys": removed,
                         "applied": {key: environment[key] for key in controlled if key in environment},
                         "driver_selection": {key: environment[key] for key in
                                              ("VK_ICD_FILENAMES", "VK_DRIVER_FILES", "VK_ADD_DRIVER_FILES")
                                              if key in environment}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--mode", choices=("describe", "inventory", "smoke", "cases"), required=True)
    parser.add_argument("--device", help="Exact 32-character lower-case Vulkan device UUID")
    parser.add_argument("--validation", choices=("required", "off"), default="required")
    parser.add_argument("--validation-manifest", type=Path, help="Locked validation tools preparation summary")
    parser.add_argument("--shader-dir", type=Path, help="Prepared shader directory with shader-manifest.json")
    parser.add_argument("--lifecycle-cycles", type=int, default=2, help="Resize/minimize/restore iterations (0-100)")
    parser.add_argument("--device-cycles", type=int, default=1, help="Device create/destroy iterations (1-20)")
    parser.add_argument("--present-seconds", type=int, default=0, help="Finite presentation duration (0-600)")
    parser.add_argument("--inject-failure", choices=("none", "missing-feature", "shader", "readback"), default="none")
    parser.add_argument("--timeout", type=int, default=120, help="Process deadline in seconds (1-1200)")
    parser.add_argument("--output", type=Path, required=True, help="New evidence directory; never overwritten")
    parser.add_argument("--build-dir", type=Path, help="Configured build directory for compiler metadata")
    args = parser.parse_args()
    if not 1 <= args.timeout <= 1200:
        parser.error("--timeout must be between 1 and 1200")
    if not 0 <= args.lifecycle_cycles <= 100 or not 1 <= args.device_cycles <= 20 or not 0 <= args.present_seconds <= 600:
        parser.error("Lifecycle bounds are resize 0-100, device 1-20 and presentation 0-600 seconds")
    if args.mode in ("smoke", "cases"):
        if not args.device or not re.fullmatch(r"[0-9a-f]{32}", args.device):
            parser.error("smoke/cases require an explicit 32-character lower-case device UUID")
    elif args.device or args.validation != "required":
        parser.error("--device and --validation off apply only to smoke/cases")
    if args.mode == "cases":
        if args.shader_dir is None:
            parser.error("cases require --shader-dir with the independently validated fixture manifest")
    elif (args.shader_dir or args.lifecycle_cycles != 2 or args.device_cycles != 1
          or args.present_seconds or args.inject_failure != "none"):
        parser.error("Shader, lifecycle and injected-failure options apply only to cases")
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error(f"Probe executable does not exist: {executable}")
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(f"Choose a new writable evidence directory: {error}")
    command = [str(executable), "--" + args.mode]
    if args.mode in ("smoke", "cases"):
        command.extend(["--device", args.device, "--validation", args.validation])
    artifacts_dir = output / "artifacts" if args.mode == "cases" else None
    if args.mode == "cases":
        command.extend(["--shader-dir", str(args.shader_dir.resolve()), "--artifacts-dir", str(artifacts_dir),
                        "--lifecycle-cycles", str(args.lifecycle_cycles), "--device-cycles", str(args.device_cycles),
                        "--present-seconds", str(args.present_seconds), "--inject-failure", args.inject_failure])
    summary = {"schema_version": 2, "assignment": "02", "subchunk": "02b",
               "qualification": "NOT_RUN", "status": "FAIL", "mode": args.mode,
               "started_utc": datetime.now(timezone.utc).isoformat(), "command": command,
               "timeout_seconds": args.timeout, "exit_code": 1}
    started = time.monotonic()
    exit_code = 1
    try:
        digest = sha256(ROOT / "dependencies.lock.json")
        summary.update({"dependency_lock_sha256": digest, "executable_sha256": sha256(executable),
                        "source": source_state()})
        validator_manifest = None
        if args.validation_manifest:
            validator_manifest = validation_tools(args.validation_manifest.resolve(), digest)
            summary["validation_tools"] = {"manifest_sha256": sha256(args.validation_manifest),
                                           "manifest_path": str(args.validation_manifest.resolve()),
                                           "receipt": validator_manifest}
        if args.mode == "cases":
            summary["shader_inputs"] = shader_inputs(args.shader_dir.resolve(), digest)
            if args.validation == "required" and validator_manifest is None:
                raise Unavailable("Cases require the pinned --validation-manifest; prepare validation tools first")
            if validator_manifest is not None:
                shader_validator = summary["shader_inputs"]["validator"]
                prepared_validator = validator_manifest["tools"]["spirv_val"]
                if (shader_validator.get("manifest_sha256") != summary["validation_tools"]["manifest_sha256"]
                        or shader_validator.get("sha256") != prepared_validator["sha256"]
                        or Path(shader_validator["path"]).resolve() != Path(prepared_validator["path"]).resolve()):
                    raise ValueError("Shaders were not validated by the supplied locked validation tools receipt")
            artifacts_dir.mkdir(exist_ok=False)
        if args.mode in ("smoke", "cases") and args.validation == "required" and validator_manifest is not None:
            summary["process_security"] = check_layer_process()
        settings_path = output / "vk_layer_settings.txt"
        settings_path.write_text("# Probe uses explicit core and synchronization validation; no local overrides.\n", encoding="utf-8")
        implicit_directory = output / "implicit-layers-empty"
        implicit_directory.mkdir(exist_ok=False)
        process_environment, summary["layer_environment"] = layer_environment(
            validator_manifest if args.validation == "required" else None, settings_path, implicit_directory)
        environment = collect(args.build_dir.resolve() if args.build_dir else None)
        (output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n", encoding="utf-8")
        result = run_process(command, cwd=ROOT, timeout=args.timeout, env=process_environment)
        (output / "stdout.json").write_text(result.stdout, encoding="utf-8")
        (output / "stderr.log").write_text(result.stderr, encoding="utf-8")
        report, errors = inspect_result(result, args.mode, digest, probe_source_digest(), args.device, args.validation,
                                        artifacts_dir=artifacts_dir, lifecycle_cycles=args.lifecycle_cycles,
                                        device_cycles=args.device_cycles, present_seconds=args.present_seconds,
                                        inject_failure=args.inject_failure)
        if validator_manifest is not None:
            try:
                if validation_tools(args.validation_manifest.resolve(), digest) != validator_manifest:
                    errors.append("Validation receipt changed during probe execution")
            except (OSError, ValueError) as error:
                errors.append(str(error))
        if sha256(ROOT / "dependencies.lock.json") != digest or sha256(executable) != summary["executable_sha256"]:
            errors.append("Dependency lock or probe executable changed during execution")
        if args.mode == "cases":
            try:
                if shader_inputs(args.shader_dir.resolve(), digest) != summary["shader_inputs"]:
                    errors.append("Shader inputs changed while the probe was executing")
            except (OSError, ValueError) as error:
                errors.append(str(error))
        summary.update({"process_exit_code": result.returncode, "timed_out": result.timed_out,
                        "cleanup_errors": list(result.cleanup_errors), "report": report, "errors": errors})
        exit_code = 124 if result.timed_out else (1 if errors else result.returncode)
        if not errors:
            summary["status"] = report["status"]
    except Unavailable as error:
        summary.update(status="NOT_RUN", errors=[str(error)])
        exit_code = 3
    except (OSError, ValueError) as error:
        summary["errors"] = [str(error)]
    finally:
        summary.update({"exit_code": exit_code, "elapsed_seconds": round(time.monotonic() - started, 3),
                        "finished_utc": datetime.now(timezone.utc).isoformat()})
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": summary["status"], "qualification": "NOT_RUN",
                      "exit_code": exit_code, "summary": str(output / "summary.json")}))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
