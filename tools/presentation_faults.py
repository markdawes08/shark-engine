"""Qualify 05c recover-or-exit policy using CPU and explicit native child faults.

Native errors never claim real device/surface loss. A real NVRHI recording is
abandoned only in the two recording cases; process teardown retains its owners.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys
import time

import graphics_device_smoke as foundation
from graphics_probe import (Unavailable, check_layer_process, finite_float, layer_environment,
                            reject_constant, sha256, unique_object, validation_tools)
from process_runner import ProcessResult, run_process
from toolchain_report import ROOT, collect, command_output

CPU_FAULTS = ("waiting", "error", "exception")
NATIVE_FAULTS = ("recording-error", "recording-exception", "surface-error", "device-error")
RESOURCE_FIELDS = ("live_swapchains", "image_wrappers", "image_views", "present_semaphores",
                   "present_fences", "command_lists", "acquire_semaphores", "acquire_fences",
                   "pending_queue_workers", "retained_recordings")
FATAL_PREFIX = "Shark graphics cleanup cannot safely release live resources: "
FATAL_SUFFIX = "\nTerminating with GPU owners retained.\n"


def source_inputs() -> dict[str, str]:
    inputs = foundation.source_inputs()
    del inputs["tests/graphics_device_smoke.cpp"]
    inputs["tests/presentation_faults.cpp"] = sha256(ROOT / "tests/presentation_faults.cpp")
    return dict(sorted(inputs.items()))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def count(value: object) -> bool:
    return type(value) is int and 0 <= value <= 0xFFFFFFFFFFFFFFFF


def stats(value: object) -> dict:
    require(isinstance(value, dict), "Missing presentation counters")
    require(all(count(value.get(key)) for key in ("submitted", "completed", "presented", "generation", "retired_generations")), "Invalid presentation counter")
    require(all(type(value.get(key)) is bool for key in ("active", "closed", "faulted")), "Invalid state flag")
    require(value["completed"] <= value["submitted"] and value["presented"] <= value["submitted"], "Completion/presentation exceeds submission")
    require(value["retired_generations"] <= value["generation"], "Retirement exceeds generation")
    return value


def clean(value: object) -> None:
    require(isinstance(value, dict) and set(value) == {"warnings", "errors", "dropped", "messages"}, "Missing validation diagnostics")
    require(all(type(value[key]) is int and value[key] == 0 for key in ("warnings", "errors", "dropped")) and value["messages"] == [], "Unexpected Vulkan/NVRHI diagnostics")


def resources(value: object) -> dict:
    require(isinstance(value, dict) and set(value) == set(RESOURCE_FIELDS), "Resource observation fields differ")
    require(all(count(item) for item in value.values()), "Resource observations require unsigned counters")
    for key in RESOURCE_FIELDS:
        bound = 8 if key.startswith("image_") or key.startswith("present_") else 2
        if key in ("live_swapchains", "pending_queue_workers"):
            bound = 1
        require(value[key] <= bound, f"Resource count exceeded fixed ownership bound: {key}")
    return value


def inspect_result(result: ProcessResult, mode: str, fault: str, device: str = "",
                   lock_digest: str = "", source_digest: str = "") -> tuple[list[dict], list[str]]:
    errors = list(result.cleanup_errors)
    if result.timed_out:
        errors.append("Fault child exceeded its external deadline")
    rows = []
    fatal = mode == "cpu" or fault.startswith("recording-")
    try:
        require(mode in ("cpu", "native") and fault in (CPU_FAULTS if mode == "cpu" else NATIVE_FAULTS), "Unknown fault workload")
        require(result.returncode == (86 if fatal else 1), "Child did not preserve the required nonzero fault exit")
        require(len(result.stdout.encode("utf-8")) <= 1024 * 1024, "Fault output exceeded byte bound")
        for line in result.stdout.splitlines():
            row = json.loads(line, object_pairs_hook=unique_object, parse_constant=reject_constant, parse_float=finite_float)
            require(isinstance(row, dict), "Fault row must be an object")
            rows.append(row)
        expected_kinds = ["identity", "retained", "terminated"] if mode == "cpu" else ["identity", "healthy", "fault", "retained", "terminated"] if fatal else ["identity", "healthy", "fault", "closed"]
        require([row.get("kind") for row in rows] == expected_kinds, "Fault trace is incomplete or out of order")
        identity = rows[0]
        require(type(identity.get("schema_version")) is int and identity["schema_version"] == 1 and identity.get("mode") == mode and identity.get("fault") == fault and identity.get("device_accessed") is (mode == "native"), "Fault identity disagrees with request")
        final = rows[-2] if fatal else rows[-1]
        final_stats = stats(final.get("stats"))
        require(count(final.get("teardown_calls")) and final["teardown_calls"] == (0 if fatal else 7), "Native owners were released before safe completion or not released afterward")
        error = final.get("error")
        require(isinstance(error, dict) and set(error) == {"code", "operation", "message"} and error["code"] == "execution_failed" and all(isinstance(error[key], str) and 0 < len(error[key]) <= 1024 for key in ("operation", "message")), "Missing owned failure context")
        if fatal:
            require(final_stats["active"] and not final_stats["closed"], "Fatal drain released live resources")
            require(rows[-1].get("expected") is True and type(rows[-1].get("teardown_calls")) is int and rows[-1]["teardown_calls"] == 0, "Terminate handler was not reached with retained owners")
            require(result.stderr == FATAL_PREFIX + error["message"] + FATAL_SUFFIX, "Bounded production fatal diagnostic is missing or contaminated")
        else:
            require(not result.stderr, "Safely drained policy error emitted unexpected stderr")
            require(final_stats["closed"] and not final_stats["active"] and final_stats["faulted"] and final_stats["completed"] == final_stats["submitted"] and final_stats["retired_generations"] == final_stats["generation"], "Policy failure did not complete real cleanup")
        if mode == "cpu":
            require(final_stats["submitted"] == 1 and final_stats["presented"] == 1, "CPU child lacks pre-fault submitted use")
            operation = {"waiting": "graphics.presentation.drain_timeout", "error": "graphics.test.cpu_drain", "exception": "graphics.presentation.close"}[fault]
            require(error["operation"] == operation, "CPU failure did not exercise the requested drain path")
            return rows, errors
        require(identity.get("selection") == device and identity.get("validation") == "required" and identity.get("dependency_lock_sha256") == lock_digest and identity.get("source_sha256") == source_digest and isinstance(identity.get("compiler"), str) and bool(identity["compiler"]), "Native fault identity/validation differs from current inputs")
        healthy = rows[1]
        adapter = healthy.get("adapter")
        require(isinstance(adapter, dict) and adapter.get("uuid") == device and all(isinstance(adapter.get(key), str) and bool(adapter[key]) for key in ("name", "driver_name", "driver_info")) and count(adapter.get("driver_version")), "Native fault lacks requested device/driver metadata")
        before = stats(healthy.get("stats"))
        require(before["submitted"] == 1 and before["presented"] == 1 and before["active"] and not before["closed"] and not before["faulted"], "Native child lacks one healthy presented frame")
        failed = rows[2]
        require(failed.get("fired") is True and failed.get("primary") == failed.get("repeated") == error, "Primary failure was lost or changed by repeat/cleanup")
        expected_operation = "graphics.presentation.frame" if fault == "recording-exception" else "graphics.test." + fault.replace("-", "_")
        require(error["operation"] == expected_operation, "Wrong native fault was observed")
        after = stats(failed.get("stats"))
        require(after["faulted"] and after["active"] and not after["closed"] and after["submitted"] == before["submitted"] and after["presented"] == before["presented"], "Fault caused additional submission or premature teardown")
        require(final_stats["faulted"] and all(final_stats[key] == after[key] for key in ("submitted", "presented", "generation")), "Cleanup changed the failed submission/generation counters")
        for row in (healthy, failed, final):
            clean(row.get("diagnostics"))
        during = resources(failed.get("resources"))
        require(during["live_swapchains"] == 1 and during["image_views"] > 0 and during["image_wrappers"] == during["image_views"] and during["command_lists"] > 0 and during["retained_recordings"] == (1 if fatal else 0), "Injected recording ownership was not observed")
        end = resources(final.get("resources"))
        require(end == during if fatal else all(value == 0 for value in end.values()), "Fatal cleanup changed resources or safe cleanup leaked resources")
    except (ValueError, TypeError, KeyError) as error:
        errors.append(f"Invalid fault evidence: {error}")
    return rows, errors


def cpu_checks(executable: Path) -> int:
    errors = []
    for flag in ("--help", "--describe"):
        result = run_process([str(executable), flag], timeout=5, cwd=executable.parent)
        try:
            row = json.loads(result.stdout)
            require(result.returncode == 0 and not result.stderr and not result.timed_out and not result.cleanup_errors and row.get("device_accessed") is False and row.get("qualification") == "NOT_RUN" and row.get("cpu_faults") == list(CPU_FAULTS) and row.get("native_faults") == list(NATIVE_FAULTS), "Description/help accessed graphics or changed workload")
        except (ValueError, TypeError, AttributeError) as error:
            errors.append(str(error))
    for args in ([], ["--cpu-fault"], ["--cpu-fault", "other"], ["--native-fault", "other", "--device", "a" * 32], ["--native-fault", "recording-error", "--device", "auto"], ["--describe", "extra"]):
        result = run_process([str(executable), *args], timeout=5, cwd=executable.parent)
        if result.returncode != 2 or result.stdout or result.timed_out or result.cleanup_errors:
            errors.append("Invalid arguments did not fail before graphics initialization")
    for fault in CPU_FAULTS:
        result = run_process([str(executable), "--cpu-fault", fault], timeout=5, cwd=executable.parent)
        _, problems = inspect_result(result, "cpu", fault)
        errors.extend(f"{fault}: {problem}" for problem in problems)
    print(json.dumps({"mode": "cpu", "status": "FAIL" if errors else "PASS", "cases": len(CPU_FAULTS), "errors": errors}))
    return 1 if errors else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--mode", choices=("cpu", "native"), required=True)
    parser.add_argument("--device")
    parser.add_argument("--validation-manifest", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error("Requires the built PresentationFaults executable")
    if args.mode == "cpu":
        if args.device or args.validation_manifest or args.output:
            parser.error("CPU mode does not accept native qualification options")
        return cpu_checks(executable)
    if sys.platform != "win32" or not args.device or not re.fullmatch(r"[0-9a-f]{32}", args.device) or not args.output:
        parser.error("Native mode requires Windows, an explicit device UUID and a fresh output directory")
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(f"Choose a fresh writable evidence directory: {error}")
    summary = {"schema_version": 1, "checkpoint": "05c", "status": "FAIL", "qualification": "NOT_RUN", "mode": "native", "device": args.device,
               "scope": "Injected policy failures on a healthy driver; no actual hardware/surface loss induced", "timeout_seconds_per_child": 30,
               "started_utc": datetime.now(timezone.utc).isoformat(), "runs": [], "errors": []}
    started = time.monotonic()
    exit_code = 1
    try:
        lock = sha256(ROOT / "dependencies.lock.json")
        inputs = source_inputs()
        digest = foundation.source_digest(inputs)
        summary.update(dependency_lock_sha256=lock, source_sha256=digest, input_sha256=inputs, executable_sha256=sha256(executable),
                       git_revision=command_output(["git", "rev-parse", "HEAD"]), git_status=command_output(["git", "status", "--short"]))
        summary["tool_inputs"] = {name: sha256(ROOT / "tools" / name) for name in ("presentation_faults.py", "graphics_device_smoke.py", "graphics_probe.py", "process_runner.py", "toolchain_report.py", "windows_unelevated.py")}
        if not args.validation_manifest:
            raise Unavailable("Native fault qualification needs the pinned validation-tools receipt")
        validation = validation_tools(args.validation_manifest.resolve(), lock)
        summary["validation_tools"] = {"manifest_path": str(args.validation_manifest.resolve()), "manifest_sha256": sha256(args.validation_manifest), "receipt": validation}
        summary["process_security"] = check_layer_process()
        settings = output / "vk_layer_settings.txt"
        settings.write_text("# Required core, synchronization and NVRHI validation; no overrides.\n", encoding="utf-8")
        implicit = output / "implicit-layers-empty"
        implicit.mkdir()
        environment, summary["layer_environment"] = layer_environment(validation, settings, implicit)
        (output / "environment.json").write_text(json.dumps(collect(args.build_dir.resolve() if args.build_dir else None), indent=2) + "\n", encoding="utf-8")
        for fault in NATIVE_FAULTS:
            command = [str(executable), "--native-fault", fault, "--device", args.device]
            child_started = time.monotonic()
            result = run_process(command, timeout=30, cwd=executable.parent, env=environment)
            (output / f"{fault}.jsonl").write_text(result.stdout, encoding="utf-8")
            (output / f"{fault}.stderr.log").write_text(result.stderr, encoding="utf-8")
            rows, errors = inspect_result(result, "native", fault, args.device, lock, digest)
            summary["runs"].append({"fault": fault, "command": command, "process_exit_code": result.returncode, "timed_out": result.timed_out,
                                    "elapsed_seconds": round(time.monotonic() - child_started, 3), "cleanup_errors": list(result.cleanup_errors), "records": rows, "errors": errors})
            summary["errors"].extend(f"{fault}: {error}" for error in errors)
        if source_inputs() != inputs or sha256(executable) != summary["executable_sha256"] or validation_tools(args.validation_manifest.resolve(), lock) != validation:
            summary["errors"].append("Source, executable or pinned validation inputs changed during execution")
        if any(sha256(ROOT / "tools" / name) != digest for name, digest in summary["tool_inputs"].items()):
            summary["errors"].append("Verification tools changed during execution")
        if not summary["errors"]:
            summary.update(status="PASS", qualification="PASS")
            exit_code = 0
    except Unavailable as error:
        summary.update(status="NOT_RUN", errors=[str(error)])
        exit_code = 3
    except (OSError, ValueError, TypeError) as error:
        summary["errors"].append(str(error))
    finally:
        summary.update(exit_code=exit_code, elapsed_seconds=round(time.monotonic() - started, 3), finished_utc=datetime.now(timezone.utc).isoformat())
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": summary["status"], "qualification": summary["qualification"], "summary": str(output / "summary.json")}))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
