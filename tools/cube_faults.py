"""Qualify configured window resource failures in bounded native child processes."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys
import time

import graphics_device_smoke as foundation
import shader_pipeline
from graphics_probe import (Unavailable, check_layer_process, finite_float, layer_environment,
                            reject_constant, sha256, unique_object, validation_tools)
from presentation_faults import FATAL_PREFIX, FATAL_SUFFIX, clean, count, require
from process_runner import ProcessResult, run_process
from toolchain_report import ROOT, collect, command_output

FAULTS = ("partial-creation", "capture-timeout", "recording-error")
RESOURCE_FIELDS = ("offscreen_resources", "offscreen_submissions", "offscreen_recordings",
                   "live_swapchains", "presentation_command_lists", "pending_queue_workers",
                   "window_draw_resources", "window_draw_submissions", "retained_recordings")


def source_inputs() -> dict[str, str]:
    inputs = foundation.source_inputs()
    del inputs["tests/graphics_device_smoke.cpp"]
    inputs["tests/cube_faults.cpp"] = sha256(ROOT / "tests/cube_faults.cpp")
    return dict(sorted(inputs.items()))


def resources(value: object, *, active: bool = False, handles: int = 0,
              submitted: int = 0, recording: int = 0) -> None:
    require(isinstance(value, dict) and set(value) == set(RESOURCE_FIELDS), "Invalid resource fields")
    require(all(type(item) is int for item in value.values()), "Resource counters require integers")
    expected = (0, 0, 0, int(active), 2 * int(active), 0, handles, submitted, recording)
    require(value == dict(zip(RESOURCE_FIELDS, expected)), "Unexpected window ownership or retirement")


def stats(value: object, *, attempted: bool = True, presented: bool = False,
          closed: bool = False, faulted: bool = False, drained: bool = False) -> None:
    fields = {"submitted", "completed", "presented", "generation", "retired_generations",
              "active", "closed", "faulted"}
    require(isinstance(value, dict) and set(value) == fields, "Invalid presentation stats")
    require(all(count(value[key]) for key in fields - {"active", "closed", "faulted"}),
            "Presentation counters require bounded integers")
    require(value["submitted"] == value["presented"] == int(presented) and
            value["completed"] <= int(presented) and value["generation"] == int(attempted) and
            value["retired_generations"] == int(closed and attempted) and
            value["active"] is (attempted and not closed) and value["closed"] is closed and
            value["faulted"] is faulted, "Presentation state differs from the required fault lifecycle")
    if drained or closed:
        require(value["completed"] == int(presented), "Completed work was not retired before close")


def owned_error(value: object, operation: str) -> None:
    require(isinstance(value, dict) and set(value) == {"code", "message", "operation"}, "Missing owned error")
    require(value["code"] == "execution_failed" and value["operation"] == operation and
            isinstance(value["message"], str) and 0 < len(value["message"]) <= 1024,
            "Wrong error code, operation or bounded diagnostic")


def inspect_result(result: ProcessResult, fault: str, device: str, lock: str,
                   digest: str, bundle_hash: str) -> tuple[list[dict], list[str]]:
    rows: list[dict] = []
    errors = list(result.cleanup_errors)
    if result.timed_out:
        errors.append("Cube fault child exceeded its external deadline")
    try:
        require(fault in FAULTS, "Unknown cube fault")
        fatal, captured = fault == "recording-error", fault == "capture-timeout"
        require(result.returncode == (86 if fatal else 1), "Observed fault lost its expected nonzero exit")
        require(len(result.stdout.encode("utf-8")) <= 1024 * 1024, "Fault output exceeds byte bound")
        for line in result.stdout.splitlines():
            row = json.loads(line, object_pairs_hook=unique_object,
                             parse_constant=reject_constant, parse_float=finite_float)
            require(isinstance(row, dict), "Fault record is not an object")
            rows.append(row)
        expected = ["identity", "ready", "fault"] + (["retained", "terminated"] if fatal else
                    ["drained", "recovered", "closed"] if captured else ["closed"])
        require([row.get("kind") for row in rows] == expected, "Incomplete or unordered fault trace")
        identity = rows[0]
        expected_identity = {"kind": "identity", "schema_version": 1, "fault": fault,
                             "selection": device, "validation": "required", "device_accessed": True,
                             "dependency_lock_sha256": lock, "source_sha256": digest,
                             "bundle_sha256": bundle_hash}
        require(set(identity) == {*expected_identity, "compiler"} and
                all(type(identity[key]) is type(value) and identity[key] == value
                    for key, value in expected_identity.items()) and
                isinstance(identity["compiler"], str) and 0 < len(identity["compiler"]) <= 256,
                "Fault identity differs from current device, validation, source or bundle")
        ready = rows[1]
        require(set(ready) == {"kind", "adapter", "resources", "stats", "diagnostics"}, "Invalid ready fields")
        adapter = ready["adapter"]
        require(isinstance(adapter, dict) and set(adapter) == {"uuid", "name", "driver_name", "driver_info", "driver_version"} and
                adapter["uuid"] == device and all(isinstance(adapter[key], str) and 0 < len(adapter[key]) <= 256
                for key in ("name", "driver_name", "driver_info")) and count(adapter["driver_version"]),
                "Missing selected hardware and driver identity")
        resources(ready["resources"])
        stats(ready["stats"], attempted=False)
        observed = rows[2]
        require(set(observed) == {"kind", "fired", "error", "resources", "stats", "diagnostics", "teardown_calls"} and
                observed["fired"] is True and type(observed["teardown_calls"]) is int and
                observed["teardown_calls"] == 0, "Fault did not execute or prematurely destroyed device owners")
        operations = {"partial-creation": "graphics.test.window_draw_partial_creation",
                      "capture-timeout": "graphics.capture.wait", "recording-error": "graphics.test.recording_error"}
        owned_error(observed["error"], operations[fault])
        resources(observed["resources"], active=True, handles=15 if captured or fatal else 0,
                  submitted=int(captured), recording=int(fatal))
        stats(observed["stats"], presented=captured, faulted=not captured)
        checked_rows = [ready, observed]
        if fatal:
            retained = rows[3]
            require(set(retained) == {"kind", "error", "resources", "stats", "diagnostics", "teardown_calls"},
                    "Invalid retained fields")
            resources(retained["resources"], active=True, handles=15, recording=1)
            stats(retained["stats"], faulted=True)
            owned_error(retained["error"], operations[fault])
            require(retained["error"] == observed["error"], "Cleanup replaced the primary recording failure")
            require(type(retained["teardown_calls"]) is int and retained["teardown_calls"] == 0 and
                    rows[4] == {"kind": "terminated", "expected": True, "teardown_calls": 0} and
                    rows[4].get("expected") is True and type(rows[4]["teardown_calls"]) is int,
                    "Fatal teardown released unresolved owners")
            require(result.stderr == FATAL_PREFIX + retained["error"]["message"] + FATAL_SUFFIX,
                    "Expected production fatal diagnostic is missing or contaminated")
            checked_rows.append(retained)
        else:
            require(not result.stderr, "Recovered failure emitted unexpected stderr")
            if captured:
                drained = rows[3]
                require(set(drained) == {"kind", "resources", "stats", "diagnostics", "teardown_calls"} and
                        type(drained["teardown_calls"]) is int and drained["teardown_calls"] == 0,
                        "Drain destroyed reusable resources or changed its schema")
                resources(drained["resources"], active=True, handles=15, submitted=1)
                stats(drained["stats"], presented=True, drained=True)
                recovered = rows[4]
                require(set(recovered) == {"kind", "submitted", "completed", "created_resources", "retired_resources",
                                           "center_rgba8", "center_depth", "diagnostics"}, "Invalid recovered fields")
                require(count(recovered["submitted"]) and recovered["submitted"] > 0 and
                        type(recovered["completed"]) is int and recovered["completed"] == recovered["submitted"] and
                        type(recovered["created_resources"]) is int and recovered["created_resources"] == 15 and
                        type(recovered["retired_resources"]) is int and recovered["retired_resources"] == 0,
                        "Recovered readback lacks completion or changed persistent ownership")
                color, depth = recovered["center_rgba8"], recovered["center_depth"]
                require(isinstance(color, list) and len(color) == 4 and all(type(c) is int for c in color) and
                        color[0] == color[3] == 255 and abs(color[1] - 99) <= 1 and abs(color[2] - 71) <= 1 and
                        type(depth) in (int, float) and abs(depth - 0.2687578548442049) <= 0.00001,
                        "Recovered draw lacks fixed known-pixel/depth evidence")
                checked_rows.extend([drained, recovered])
            closed = rows[-1]
            require(set(closed) == {"kind", "resources", "stats", "diagnostics", "teardown_calls"} | ({"error"} if not captured else set()),
                    "Invalid closed fields")
            resources(closed["resources"])
            stats(closed["stats"], presented=captured, closed=True, faulted=not captured)
            require(type(closed["teardown_calls"]) is int and closed["teardown_calls"] == 7,
                    "Window/device resources did not fully close")
            if not captured:
                owned_error(closed["error"], operations[fault])
            checked_rows.append(closed)
        for row in checked_rows:
            clean(row["diagnostics"])
    except (ValueError, TypeError, KeyError) as error:
        errors.append(f"Invalid cube fault evidence: {error}")
    return rows, errors


def cli_checks(executable: Path) -> int:
    errors = []
    for flag in ("--help", "--describe"):
        result = run_process([str(executable), flag], timeout=5, cwd=executable.parent)
        try:
            row = json.loads(result.stdout, object_pairs_hook=unique_object, parse_constant=reject_constant)
            require(result.returncode == 0 and not result.stderr and not result.timed_out and
                    not result.cleanup_errors and row == {"kind": "description", "schema_version": 1,
                    "device_accessed": False, "qualification": "NOT_RUN", "faults": list(FAULTS)},
                    "Help/description changed the workload or accessed graphics")
        except (ValueError, TypeError) as error:
            errors.append(str(error))
    for args in ([], ["--native-fault"], ["--describe", "extra"],
                 ["--native-fault", "other", "--device", "a" * 32, "--bundle", "a"],
                 ["--native-fault", "recording-error", "--device", "auto", "--bundle", "a"],
                 ["--native-fault", "recording-error", "--device", "a" * 32, "--bundle", ""]):
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
    parser.add_argument("--bundle", type=Path)
    parser.add_argument("--validation-manifest", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error("Requires the built CubeFaults executable")
    if args.cli_only:
        if args.device or args.bundle or args.validation_manifest or args.output:
            parser.error("CLI-only mode does not accept native qualification inputs")
        return cli_checks(executable)
    if sys.platform != "win32" or not args.device or not re.fullmatch(r"[0-9a-f]{32}", args.device) or not args.output:
        parser.error("Native mode requires Windows, an explicit device UUID and a fresh output directory")
    if not args.bundle or not args.bundle.is_file():
        parser.error("Provide a previously exported precompiled shader bundle")
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(f"Choose a fresh writable evidence directory: {error}")
    summary = {"schema_version": 1, "checkpoint": "06c", "status": "FAIL", "qualification": "NOT_RUN",
               "device": args.device, "scope": "Injected window ownership failures; no driver fault induced",
               "timeout_seconds_per_child": 30, "started_utc": datetime.now(timezone.utc).isoformat(),
               "runs": [], "errors": []}
    started, exit_code = time.monotonic(), 1
    try:
        lock, inputs = sha256(ROOT / "dependencies.lock.json"), source_inputs()
        digest = foundation.source_digest(inputs)
        bundle = args.bundle.resolve()
        require(bundle.stat().st_size <= shader_pipeline.BUNDLE_HEADER.size + 2 * shader_pipeline.MAX_BINARY,
                "Shader bundle exceeds its fixed byte bound")
        bundle_record = shader_pipeline.inspect_bundle(bundle.read_bytes())
        bundle_hash = sha256(bundle)
        summary.update(dependency_lock_sha256=lock, source_sha256=digest, input_sha256=inputs,
                       executable_sha256=sha256(executable), bundle={"path": str(bundle), "sha256": bundle_hash, "receipt": bundle_record},
                       git_revision=command_output(["git", "rev-parse", "HEAD"]),
                       git_status=command_output(["git", "status", "--short"]))
        summary["tool_inputs"] = {name: sha256(ROOT / "tools" / name) for name in (
            "cube_faults.py", "presentation_faults.py", "graphics_device_smoke.py", "graphics_probe.py",
            "shader_pipeline.py", "process_runner.py", "toolchain_report.py", "windows_unelevated.py")}
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
            command = [str(executable), "--native-fault", fault, "--device", args.device, "--bundle", str(bundle)]
            child_started = time.monotonic()
            result = run_process(command, timeout=30, cwd=executable.parent, env=environment)
            (output / f"{fault}.jsonl").write_text(result.stdout, encoding="utf-8")
            (output / f"{fault}.stderr.log").write_text(result.stderr, encoding="utf-8")
            rows, errors = inspect_result(result, fault, args.device, lock, digest, bundle_hash)
            summary["runs"].append({"fault": fault, "command": command, "process_exit_code": result.returncode,
                                    "timed_out": result.timed_out, "elapsed_seconds": round(time.monotonic() - child_started, 3),
                                    "cleanup_errors": list(result.cleanup_errors), "records": rows, "errors": errors})
            summary["errors"].extend(f"{fault}: {error}" for error in errors)
        if source_inputs() != inputs or sha256(ROOT / "dependencies.lock.json") != lock or sha256(executable) != summary["executable_sha256"] or validation_tools(args.validation_manifest.resolve(), lock) != validation:
            summary["errors"].append("Source, executable or pinned validation inputs changed during execution")
        if any(sha256(ROOT / "tools" / name) != value for name, value in summary["tool_inputs"].items()):
            summary["errors"].append("Verification tools changed during execution")
        if sha256(bundle) != bundle_hash or shader_pipeline.inspect_bundle(bundle.read_bytes()) != bundle_record:
            summary["errors"].append("Shader bundle changed during execution")
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
