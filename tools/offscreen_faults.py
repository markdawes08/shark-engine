"""Exercise bounded native offscreen resource failures on an explicitly selected GPU."""
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
from presentation_faults import FATAL_PREFIX, FATAL_SUFFIX, clean, count, require
from process_runner import ProcessResult, run_process
from toolchain_report import ROOT, collect, command_output

FAULTS = ("partial-creation", "submission-timeout", "recording-error")
RESOURCE_FIELDS = ("offscreen_resources", "offscreen_submissions", "offscreen_recordings",
                   "live_swapchains", "presentation_command_lists", "pending_queue_workers")


def source_inputs() -> dict[str, str]:
    inputs = foundation.source_inputs()
    del inputs["tests/graphics_device_smoke.cpp"]
    inputs["tests/offscreen_faults.cpp"] = sha256(ROOT / "tests/offscreen_faults.cpp")
    return dict(sorted(inputs.items()))


def resources(value: object, *, handles: int = 0, submitted: int = 0, recording: int = 0) -> None:
    require(isinstance(value, dict) and set(value) == set(RESOURCE_FIELDS), "Invalid resource fields")
    require(all(type(item) is int for item in value.values()), "Resource counters require integers")
    require(value == dict(zip(RESOURCE_FIELDS, (handles, submitted, recording, 0, 0, 0))),
            "Offscreen ownership differs from the required bounded lifecycle")


def owned_error(value: object, operation: str) -> None:
    require(isinstance(value, dict) and set(value) == {"code", "message", "operation"}, "Missing owned error")
    require(value["code"] == "execution_failed" and value["operation"] == operation and
            isinstance(value["message"], str) and 0 < len(value["message"]) <= 1024,
            "Wrong error code, operation or bounded diagnostic")


def inspect_result(result: ProcessResult, fault: str, device: str, lock: str,
                   digest: str) -> tuple[list[dict], list[str]]:
    rows: list[dict] = []
    errors = list(result.cleanup_errors)
    if result.timed_out:
        errors.append("Offscreen fault child exceeded its external deadline")
    try:
        require(fault in FAULTS, "Unknown offscreen fault")
        fatal = fault == "recording-error"
        require(result.returncode == (86 if fatal else 1), "Observed fault lost its expected nonzero exit")
        require(len(result.stdout.encode("utf-8")) <= 1024 * 1024, "Fault output exceeds byte bound")
        for line in result.stdout.splitlines():
            row = json.loads(line, object_pairs_hook=unique_object,
                             parse_constant=reject_constant, parse_float=finite_float)
            require(isinstance(row, dict), "Fault record is not an object")
            rows.append(row)
        expected = ["identity", "ready", "fault", "retained", "terminated"] if fatal else [
            "identity", "ready", "fault", "retired", "recovered", "closed"]
        require([row.get("kind") for row in rows] == expected, "Incomplete or unordered fault trace")
        identity = rows[0]
        require(type(identity.get("schema_version")) is int and identity["schema_version"] == 1 and
                identity.get("fault") == fault and identity.get("selection") == device and
                identity.get("validation") == "required" and identity.get("device_accessed") is True and
                identity.get("dependency_lock_sha256") == lock and identity.get("source_sha256") == digest and
                isinstance(identity.get("compiler"), str) and bool(identity["compiler"]),
                "Fault identity differs from requested device, validation or current inputs")
        adapter = rows[1].get("adapter")
        require(isinstance(adapter, dict) and adapter.get("uuid") == device and
                all(isinstance(adapter.get(key), str) and bool(adapter[key])
                    for key in ("name", "driver_name", "driver_info")) and count(adapter.get("driver_version")),
                "Missing selected hardware and driver identity")
        resources(rows[1].get("resources"))
        observed = rows[2]
        require(observed.get("fired") is True and type(observed.get("teardown_calls")) is int and
                observed["teardown_calls"] == 0, "Fault did not execute or prematurely destroyed device owners")
        operations = {"partial-creation": "graphics.test.offscreen_partial_creation",
                      "submission-timeout": "graphics.offscreen.wait_timeout",
                      "recording-error": "graphics.test.offscreen_recording_error"}
        owned_error(observed.get("error"), operations[fault])
        resources(observed.get("resources"), handles=0 if fault == "partial-creation" else 17,
                  submitted=int(fault == "submission-timeout"), recording=int(fatal))
        if fatal:
            retained = rows[3]
            resources(retained.get("resources"), handles=17, recording=1)
            owned_error(retained.get("error"), "graphics.offscreen.drain")
            require(type(retained.get("teardown_calls")) is int and retained["teardown_calls"] == 0 and
                    rows[4].get("expected") is True and type(rows[4].get("teardown_calls")) is int and
                    rows[4]["teardown_calls"] == 0, "Fatal teardown released unresolved owners")
            require(result.stderr == FATAL_PREFIX + retained["error"]["message"] + FATAL_SUFFIX,
                    "Expected production fatal diagnostic is missing or contaminated")
            checked_rows = rows[1:4]
        else:
            require(not result.stderr, "Recovered failure emitted unexpected stderr")
            resources(rows[3].get("resources"))
            require(type(rows[3].get("teardown_calls")) is int and rows[3]["teardown_calls"] == 0,
                    "Drain destroyed the device rather than retiring completed resources")
            recovered = rows[4]
            require(count(recovered.get("submitted")) and recovered["submitted"] > 0 and
                    type(recovered.get("completed")) is int and recovered["completed"] == recovered["submitted"] and
                    type(recovered.get("created_resources")) is int and recovered["created_resources"] == 17 and
                    type(recovered.get("retired_resources")) is int and recovered["retired_resources"] == 17 and
                    recovered.get("center_rgba8") == [64, 128, 192, 255] and recovered.get("center_depth") == 0.5,
                    "Recovered draw lacks complete resource retirement and known-pixel evidence")
            resources(rows[5].get("resources"))
            require(type(rows[5].get("teardown_calls")) is int and rows[5]["teardown_calls"] == 5,
                    "Recovered offscreen-only device did not fully close")
            checked_rows = rows[1:]
        for row in checked_rows:
            clean(row.get("diagnostics"))
    except (ValueError, TypeError, KeyError) as error:
        errors.append(f"Invalid offscreen fault evidence: {error}")
    return rows, errors


def cli_checks(executable: Path) -> int:
    errors = []
    for flag in ("--help", "--describe"):
        result = run_process([str(executable), flag], timeout=5, cwd=executable.parent)
        try:
            row = json.loads(result.stdout)
            require(result.returncode == 0 and not result.stderr and not result.timed_out and
                    not result.cleanup_errors and row.get("device_accessed") is False and
                    row.get("qualification") == "NOT_RUN" and row.get("faults") == list(FAULTS),
                    "Help/description changed the workload or accessed graphics")
        except (ValueError, TypeError, AttributeError) as error:
            errors.append(str(error))
    for args in ([], ["--native-fault"], ["--describe", "extra"],
                 ["--native-fault", "other", "--device", "a" * 32, "--vertex", "a", "--pixel", "b"],
                 ["--native-fault", "recording-error", "--device", "auto", "--vertex", "a", "--pixel", "b"]):
        result = run_process([str(executable), *args], timeout=5, cwd=executable.parent)
        if result.returncode != 2 or result.stdout or result.timed_out or result.cleanup_errors:
            errors.append("Malformed arguments did not reject before device access")
    print(json.dumps({"mode": "cpu-cli", "status": "FAIL" if errors else "PASS", "errors": errors}))
    return 1 if errors else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--cli-only", action="store_true")
    parser.add_argument("--device")
    parser.add_argument("--vertex", type=Path)
    parser.add_argument("--pixel", type=Path)
    parser.add_argument("--validation-manifest", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error("Requires the built OffscreenFaults executable")
    if args.cli_only:
        if args.device or args.vertex or args.pixel or args.validation_manifest or args.output:
            parser.error("CLI-only mode does not accept native qualification inputs")
        return cli_checks(executable)
    if sys.platform != "win32" or not args.device or not re.fullmatch(r"[0-9a-f]{32}", args.device) or not args.output:
        parser.error("Native mode requires Windows, an explicit device UUID and a fresh output directory")
    if not args.vertex or not args.pixel or not args.vertex.is_file() or not args.pixel.is_file():
        parser.error("Provide both previously verified offline SPIR-V artifacts")
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(f"Choose a fresh writable evidence directory: {error}")
    summary = {"schema_version": 1, "checkpoint": "06b", "status": "FAIL", "qualification": "NOT_RUN",
               "device": args.device, "scope": "Injected resource ownership failures; no driver fault induced",
               "timeout_seconds_per_child": 30, "started_utc": datetime.now(timezone.utc).isoformat(),
               "runs": [], "errors": []}
    started = time.monotonic()
    exit_code = 1
    try:
        lock = sha256(ROOT / "dependencies.lock.json")
        inputs = source_inputs()
        digest = foundation.source_digest(inputs)
        shaders = {stage: {"path": str(path.resolve()), "sha256": sha256(path)}
                   for stage, path in (("vertex", args.vertex), ("pixel", args.pixel))}
        summary.update(dependency_lock_sha256=lock, source_sha256=digest, input_sha256=inputs,
                       executable_sha256=sha256(executable), shaders=shaders,
                       git_revision=command_output(["git", "rev-parse", "HEAD"]),
                       git_status=command_output(["git", "status", "--short"]))
        summary["tool_inputs"] = {name: sha256(ROOT / "tools" / name) for name in (
            "offscreen_faults.py", "presentation_faults.py", "graphics_device_smoke.py", "graphics_probe.py",
            "process_runner.py", "toolchain_report.py", "windows_unelevated.py")}
        if not args.validation_manifest:
            raise Unavailable("Native fault qualification requires the pinned validation-tools receipt")
        validation = validation_tools(args.validation_manifest.resolve(), lock)
        summary["validation_tools"] = {"manifest_path": str(args.validation_manifest.resolve()),
                                       "manifest_sha256": sha256(args.validation_manifest), "receipt": validation}
        summary["process_security"] = check_layer_process()
        settings = output / "vk_layer_settings.txt"
        settings.write_text("# Required core, synchronization and NVRHI validation; no overrides.\n", encoding="utf-8")
        implicit = output / "implicit-layers-empty"
        implicit.mkdir()
        environment, summary["layer_environment"] = layer_environment(validation, settings, implicit)
        (output / "environment.json").write_text(json.dumps(collect(args.build_dir.resolve() if args.build_dir else None), indent=2) + "\n", encoding="utf-8")
        for fault in FAULTS:
            command = [str(executable), "--native-fault", fault, "--device", args.device,
                       "--vertex", shaders["vertex"]["path"], "--pixel", shaders["pixel"]["path"]]
            child_started = time.monotonic()
            result = run_process(command, timeout=30, cwd=executable.parent, env=environment)
            (output / f"{fault}.jsonl").write_text(result.stdout, encoding="utf-8")
            (output / f"{fault}.stderr.log").write_text(result.stderr, encoding="utf-8")
            rows, errors = inspect_result(result, fault, args.device, lock, digest)
            summary["runs"].append({"fault": fault, "command": command, "process_exit_code": result.returncode,
                                    "timed_out": result.timed_out, "elapsed_seconds": round(time.monotonic() - child_started, 3),
                                    "cleanup_errors": list(result.cleanup_errors), "records": rows, "errors": errors})
            summary["errors"].extend(f"{fault}: {error}" for error in errors)
        if source_inputs() != inputs or sha256(executable) != summary["executable_sha256"] or validation_tools(args.validation_manifest.resolve(), lock) != validation:
            summary["errors"].append("Source, executable or pinned validation inputs changed during execution")
        if any(sha256(ROOT / "tools" / name) != value for name, value in summary["tool_inputs"].items()):
            summary["errors"].append("Verification tools changed during execution")
        if any(sha256(Path(value["path"])) != value["sha256"] for value in shaders.values()):
            summary["errors"].append("Shader artifact changed during execution")
        if not summary["errors"]:
            summary.update(status="PASS", qualification="PASS")
            exit_code = 0
    except Unavailable as error:
        summary.update(status="NOT_RUN", errors=[str(error)])
        exit_code = 3
    except (OSError, ValueError, TypeError) as error:
        summary["errors"].append(str(error))
    finally:
        summary.update(exit_code=exit_code, elapsed_seconds=round(time.monotonic() - started, 3),
                       finished_utc=datetime.now(timezone.utc).isoformat())
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": summary["status"], "qualification": summary["qualification"],
                      "summary": str(output / "summary.json")}))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
