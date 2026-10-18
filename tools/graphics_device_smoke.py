"""Run the owned 05a graphics device fixture with bounded, reproducible evidence.

This opens hidden windows and creates graphics devices, but never submits GPU
work or presents. Validation-off runs are diagnostic and cannot qualify 05a.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import sys
import time

from graphics_probe import (Unavailable, check_layer_process, finite_float, layer_environment,
                            reject_constant, sha256, unique_object, validation_tools)
from process_runner import ProcessResult, run_process
from toolchain_report import ROOT, collect, command_output

CYCLES = 16
FEATURES = {"vulkan_1_3", "timeline_semaphore", "synchronization2", "dynamic_rendering",
            "swapchain_extension", "graphics_compute_present_queue", "srgb_surface", "fifo_present"}
FORMATS = {"rgba8_srgb", "rgba8_unorm", "rgba16_float", "d32_float_depth_array"}
LIMITS = {"maxImageDimension2D": 4096, "maxImageDimensionCube": 2048,
          "maxImageArrayLayers": 12, "maxColorAttachments": 4, "maxBoundDescriptorSets": 4,
          "maxPerStageDescriptorSampledImages": 16, "maxPerStageDescriptorSamplers": 16,
          "maxUniformBufferRange": 16384, "maxStorageBufferRange": 134217728,
          "maxComputeWorkGroupInvocations": 128}


def source_inputs() -> dict[str, str]:
    paths = {"CMakeLists.txt", "dependencies.lock.json", "engine/CMakeLists.txt",
             "tests/CMakeLists.txt", "tests/graphics_device_smoke.cpp", "probes/graphics/report.hpp"}
    for directory in ("engine/core", "engine/client"):
        paths.update(path.relative_to(ROOT).as_posix() for path in (ROOT / directory).rglob("*")
                     if path.is_file() and (path.suffix in (".cpp", ".hpp", ".manifest")
                                            or path.name == "CMakeLists.txt"))
    paths.update(path.relative_to(ROOT).as_posix() for path in (ROOT / "cmake").glob("*.cmake"))
    return {path: sha256(ROOT / path) for path in sorted(paths)}


def source_digest(inputs: dict[str, str]) -> str:
    material = "".join(f"{path}\n{digest}\n" for path, digest in sorted(inputs.items()))
    return hashlib.sha256(material.encode("utf-8")).hexdigest()


def adapter_errors(adapter: object, *, require_supported: bool) -> list[str]:
    if not isinstance(adapter, dict):
        return ["Adapter metadata must be an object"]
    errors = []
    if not isinstance(adapter.get("uuid"), str) or not re.fullmatch(r"[0-9a-f]{32}", adapter["uuid"]):
        errors.append("Adapter UUID is invalid")
    for field in ("name", "driver_name", "driver_info"):
        value = adapter.get(field)
        if not isinstance(value, str):
            errors.append(f"Adapter {field} is missing")
        elif len(value) > 256 or "\0" in value or (field == "name" and not value):
            errors.append(f"Adapter {field} exceeds its bounded metadata contract")
        else:
            # The test-only report writer emits each original byte as U+00xx.
            # Reconstruct those bytes before applying the public UTF-8 contract.
            try:
                value.encode("latin-1").decode("utf-8")
            except UnicodeError:
                errors.append(f"Adapter {field} does not contain bounded UTF-8 metadata")
    for field in ("vendor_id", "device_id", "driver_version", "api_version", "queue_family"):
        if type(adapter.get(field)) is not int or not 0 <= adapter[field] <= 0xFFFFFFFF:
            errors.append(f"Adapter {field} is invalid")
    if adapter.get("kind") not in ("other", "integrated", "discrete", "software"):
        errors.append("Adapter kind is invalid")
    for field, names in (("features", FEATURES), ("formats", FORMATS)):
        values = adapter.get(field)
        if not isinstance(values, dict) or set(values) != names or any(type(value) is not bool for value in values.values()):
            errors.append(f"Adapter must retain every {field} capability")
        elif require_supported and not all(values.values()):
            errors.append(f"Selected adapter lacks required {field}")
    limits = adapter.get("limits")
    if (not isinstance(limits, dict) or set(limits) != set(LIMITS)
            or any(type(value) is not int or not 0 <= value <= 0xFFFFFFFF for value in limits.values())):
        errors.append("Adapter must retain every numeric limit")
    elif require_supported and any(limits[key] < minimum for key, minimum in LIMITS.items()):
        errors.append("Selected adapter lacks required limits")
    missing = adapter.get("missing_requirements")
    if not isinstance(missing, list) or any(not isinstance(value, str) for value in missing):
        errors.append("Adapter missing requirements must be retained")
    elif require_supported and (missing or adapter.get("kind") == "software"):
        errors.append("Selected adapter is unsupported")
    return errors


def diagnostic_errors(diagnostics: object, *, require_clean: bool) -> list[str]:
    if not isinstance(diagnostics, dict):
        return ["Missing cumulative validation diagnostics"]
    errors = []
    for key in ("warnings", "errors", "dropped"):
        count = diagnostics.get(key)
        if type(count) is not int or not 0 <= count <= 0xFFFFFFFF:
            errors.append(f"Invalid diagnostic {key} count")
        elif require_clean and count != 0:
            errors.append(f"Qualification requires zero validation {key}")
    messages = diagnostics.get("messages")
    if not isinstance(messages, list) or any(not isinstance(message, str) for message in messages):
        errors.append("Validation messages must be retained")
    return errors


def inspect_result(result: ProcessResult, mode: str, device: str, validation: str,
                   lock_digest: str, inputs_digest: str) -> tuple[list[dict], list[str]]:
    errors = list(result.cleanup_errors)
    if result.timed_out:
        errors.append("Graphics device deadline exceeded; the owned process tree was terminated")
    if result.returncode != 0:
        errors.append(f"Graphics device fixture exited {result.returncode}")
    if len(result.stdout.encode("utf-8")) > 8 * 1024 * 1024:
        return [], [*errors, "Graphics report exceeds its byte budget"]
    records = []
    try:
        for line in result.stdout.splitlines():
            record = json.loads(line, object_pairs_hook=unique_object,
                                parse_constant=reject_constant, parse_float=finite_float)
            if not isinstance(record, dict):
                raise ValueError("Each graphics report row must be an object")
            records.append(record)
    except (ValueError, TypeError) as error:
        return records, [*errors, f"Invalid graphics JSONL: {error}"]
    if not records:
        return records, [*errors, "Graphics report is empty"]
    identity = records[0]
    for key, expected in (("kind", "identity"), ("schema_version", 1), ("mode", mode),
                          ("dependency_lock_sha256", lock_digest), ("source_sha256", inputs_digest),
                          ("validation", validation), ("selection", device), ("assignment_05", "NOT_RUN")):
        if identity.get(key) != expected:
            errors.append(f"Graphics identity {key} disagrees with the current command or sources")
    if type(identity.get("schema_version")) is not int or not isinstance(identity.get("compiler"), str):
        errors.append("Graphics identity lacks its schema/compiler metadata")
    if mode == "describe":
        if (len(records) != 2 or records[-1].get("kind") != "description"
                or records[-1].get("status") != "PASS" or records[-1].get("device_accessed") is not False
                or records[-1].get("qualification") != "NOT_RUN"):
            errors.append("Device-free description report is incomplete")
    elif mode == "inventory":
        if (len(records) != 2 or records[-1].get("kind") != "inventory"
                or records[-1].get("status") != "PASS" or records[-1].get("qualification") != "NOT_RUN"):
            errors.append("Inventory report is incomplete")
        adapters = records[-1].get("adapters")
        if not isinstance(adapters, list) or not adapters or len(adapters) > 64:
            errors.append("Inventory must contain 1..64 adapters")
        else:
            for adapter in adapters:
                errors.extend(adapter_errors(adapter, require_supported=False))
            uuids = [adapter["uuid"] for adapter in adapters
                     if isinstance(adapter, dict) and isinstance(adapter.get("uuid"), str)]
            if len(uuids) != len(set(uuids)):
                errors.append("Inventory adapter UUIDs must be unique")
    else:
        cycles = [record for record in records if record.get("kind") == "cycle"]
        if (len(records) != CYCLES + 2 or any(type(record.get("cycle")) is not int for record in cycles)
                or [record.get("cycle") for record in cycles] != list(range(1, CYCLES + 1))):
            errors.append("Smoke must retain all 16 ordered lifecycle cycles")
        selected = set()
        for cycle in cycles:
            required = validation == "required"
            if cycle.get("status") != ("PASS" if required else "DIAGNOSTIC"):
                errors.append("Graphics lifecycle cycle failed")
            for key in ("second_owner_rejected", "idempotent_close", "selection_matches"):
                if cycle.get(key) is not True:
                    errors.append(f"Lifecycle check failed: {key}")
            rejection = cycle.get("second_owner_error")
            if not isinstance(rejection, dict) or rejection.get("code") != "conflict":
                errors.append("Second owner must return a conflict error")
            adapter = cycle.get("adapter")
            errors.extend(adapter_errors(adapter, require_supported=True))
            if isinstance(adapter, dict) and isinstance(adapter.get("uuid"), str):
                selected.add(adapter["uuid"])
                if device != "auto" and adapter["uuid"] != device:
                    errors.append("Selected UUID differs from explicit request")
            for key in ("before_close", "after_close", "after_second_close"):
                errors.extend(diagnostic_errors(cycle.get(key), require_clean=required))
            if cycle.get("after_close") != cycle.get("after_second_close"):
                errors.append("Repeated close changed cumulative diagnostics")
        if len(selected) != 1:
            errors.append("Repeated ownership must retain the same selected adapter")
        expected = "PASS" if validation == "required" else "NOT_RUN"
        final = records[-1]
        if (final.get("kind") != "complete" or final.get("cycles") != CYCLES
                or final.get("status") != expected or final.get("qualification") != expected
                or final.get("execution_status") != "PASS" or final.get("assignment_05") != "NOT_RUN"):
            errors.append("Final graphics completion/qualification report is inconsistent")
    return records, errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--mode", choices=("describe", "inventory", "smoke"), required=True)
    parser.add_argument("--device", default="auto", help="auto or exact lowercase 32-hex Vulkan UUID")
    parser.add_argument("--validation", choices=("required", "off"), default="required")
    parser.add_argument("--validation-manifest", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="Fresh evidence directory")
    args = parser.parse_args()
    if args.device != "auto" and not re.fullmatch(r"[0-9a-f]{32}", args.device):
        parser.error("--device must be auto or exactly 32 lowercase hex characters")
    if args.mode != "smoke" and args.device != "auto":
        parser.error("--device applies only to smoke")
    if args.mode == "describe" and args.validation != "required":
        parser.error("Description mode performs no validation/device setup")
    executable = args.executable.resolve()
    if sys.platform != "win32" or not executable.is_file():
        parser.error("Requires Windows and the built GraphicsDeviceSmoke executable")
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(f"Choose a fresh writable evidence directory: {error}")
    command = [str(executable), "--" + args.mode]
    if args.mode != "describe":
        command.extend(["--validation", args.validation])
    if args.mode == "smoke":
        command.extend(["--device", args.device])
    summary = {"schema_version": 1, "checkpoint": "05a", "status": "FAIL", "qualification": "NOT_RUN",
               "assignment_05": "NOT_RUN", "started_utc": datetime.now(timezone.utc).isoformat(),
               "command": command, "mode": args.mode, "validation": args.validation,
               "timeout_seconds": 60, "scope": "Owned device lifecycle only; no rendering/submission/presentation"}
    exit_code = 1
    started = time.monotonic()
    try:
        digest = sha256(ROOT / "dependencies.lock.json")
        inputs = source_inputs()
        inputs_digest = source_digest(inputs)
        summary.update(dependency_lock_sha256=digest, source_sha256=inputs_digest,
                       executable_sha256=sha256(executable), input_sha256=inputs,
                       git_revision=command_output(["git", "rev-parse", "HEAD"]),
                       git_status=command_output(["git", "status", "--short"]))
        summary["tool_inputs"] = {name: sha256(ROOT / "tools" / name) for name in
                                  ("graphics_device_smoke.py", "graphics_probe.py", "process_runner.py",
                                   "toolchain_report.py", "windows_unelevated.py")}
        validation = None
        if args.mode != "describe" and args.validation == "required":
            if args.validation_manifest is None:
                raise Unavailable("Required validation needs --validation-manifest from the locked tool preparation")
            validation = validation_tools(args.validation_manifest.resolve(), digest)
            summary["validation_tools"] = {"manifest_path": str(args.validation_manifest.resolve()),
                                           "manifest_sha256": sha256(args.validation_manifest), "receipt": validation}
            summary["process_security"] = check_layer_process()
        settings = output / "vk_layer_settings.txt"
        settings.write_text("# Explicit Vulkan core/synchronization and NVRHI validation; no overrides.\n", encoding="utf-8")
        implicit = output / "implicit-layers-empty"
        implicit.mkdir()
        environment, summary["layer_environment"] = layer_environment(validation, settings, implicit)
        toolchain = collect(args.build_dir.resolve() if args.build_dir else None)
        (output / "environment.json").write_text(json.dumps(toolchain, indent=2) + "\n", encoding="utf-8")
        result = run_process(command, cwd=executable.parent, timeout=60, env=environment)
        (output / "stdout.jsonl").write_text(result.stdout, encoding="utf-8")
        (output / "stderr.log").write_text(result.stderr, encoding="utf-8")
        records, errors = inspect_result(result, args.mode, args.device, args.validation, digest, inputs_digest)
        if validation is not None and validation_tools(args.validation_manifest.resolve(), digest) != validation:
            errors.append("Pinned validation receipt changed during execution")
        if source_inputs() != inputs or sha256(executable) != summary["executable_sha256"]:
            errors.append("Sources, dependency lock or executable changed during execution")
        summary.update(records=records, errors=errors, process_exit_code=result.returncode,
                       timed_out=result.timed_out, cleanup_errors=list(result.cleanup_errors))
        exit_code = 124 if result.timed_out else (1 if errors else result.returncode)
        if exit_code == 0:
            diagnostic_only = args.mode == "smoke" and args.validation == "off"
            summary["status"] = "NOT_RUN" if diagnostic_only else "PASS"
            if args.mode == "smoke" and args.validation == "required":
                summary["qualification"] = "PASS"
    except Unavailable as error:
        summary.update(status="NOT_RUN", errors=[str(error)])
        exit_code = 3
    except (OSError, ValueError, TypeError) as error:
        summary["errors"] = [str(error)]
    finally:
        summary.update(exit_code=exit_code, elapsed_seconds=round(time.monotonic() - started, 3),
                       finished_utc=datetime.now(timezone.utc).isoformat())
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": summary["status"], "qualification": summary["qualification"],
                      "exit_code": exit_code, "summary": str(output / "summary.json")}))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
