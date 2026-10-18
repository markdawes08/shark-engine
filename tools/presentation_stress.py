"""Run and independently verify the fixed 05c native stress/resource workload."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys
import time

import presentation_smoke as smoke
from graphics_probe import (Unavailable, check_layer_process, finite_float, layer_environment,
                            reject_constant, sha256, unique_object, validation_tools)
from process_runner import ProcessResult, run_process
from toolchain_report import ROOT, collect, command_output

DURATIONS = {"debug": 60, "release": 600, "asan": 60}
MEMORY_LIMITS = {"debug": 64 * 1024 * 1024, "release": 64 * 1024 * 1024, "asan": 256 * 1024 * 1024}
PROCESS_FIELDS = {"private_bytes", "handles", "gdi_objects", "user_objects"}
RESOURCE_FIELDS = {"live_swapchains", "image_wrappers", "image_views", "present_semaphores",
                   "present_fences", "command_lists", "acquire_semaphores", "acquire_fences",
                   "pending_queue_workers", "retained_recordings"}
require = smoke.require
source_digest = smoke.source_digest


def source_inputs() -> dict[str, str]:
    inputs = smoke.foundation.source_inputs()
    del inputs["tests/graphics_device_smoke.cpp"]
    inputs["tests/presentation_stress.cpp"] = sha256(ROOT / "tests/presentation_stress.cpp")
    return dict(sorted(inputs.items()))


def process_resources(value: object) -> dict:
    require(isinstance(value, dict) and set(value) == PROCESS_FIELDS,
            "Process resource observation has unexpected or missing fields")
    require(all(smoke.integer(value[key]) for key in value)
            and value["private_bytes"] > 0 and value["handles"] > 0,
            "Process resource observation must contain valid integer measurements")
    return value


def resource_budget(value: object, baseline: object, profile: str) -> dict:
    value, baseline = process_resources(value), process_resources(baseline)
    require(value["private_bytes"] <= baseline["private_bytes"] + MEMORY_LIMITS[profile],
            "Private bytes exceeded the predeclared resource-growth allowance")
    require(value["handles"] <= baseline["handles"] + 8,
            "Handles exceeded the predeclared resource-growth allowance")
    require(all(value[key] <= baseline[key] for key in ("gdi_objects", "user_objects")),
            "GDI or USER objects grew beyond the initialized baseline")
    return value


def resources(value: object, state: dict) -> dict:
    require(isinstance(value, dict) and set(value) == RESOURCE_FIELDS,
            "Native resource inventory is incomplete")
    require(all(smoke.integer(count, 8) for count in value.values()), "Native resource count is invalid")
    if state["closed"]:
        require(not any(value.values()), "Closed device retained native resources")
    else:
        require(state["active"] and value["live_swapchains"] == 1,
                "Active device must own exactly one swapchain")
        require(value["image_wrappers"] == value["image_views"] == state["image_count"],
                "Native image ownership differs from active swapchain")
        require(value["command_lists"] == value["acquire_semaphores"] == value["acquire_fences"] == 2,
                "Native command/acquisition ownership differs from two fixed slots")
        expected = state["image_count"] if state["synchronization"] == "present_fence" else 0
        require(value["present_semaphores"] == value["present_fences"] == expected,
                "Presentation synchronization resource count differs from selected strategy")
        require(value["pending_queue_workers"] == value["retained_recordings"] == 0,
                "Drained sample retained unresolved worker or command ownership")
    return value


def clean(value: object) -> None:
    require(isinstance(value, dict) and set(value) == {"warnings", "errors", "dropped", "messages"},
            "Validation diagnostics are incomplete")
    require(all(type(value[key]) is int and value[key] == 0 for key in ("warnings", "errors", "dropped"))
            and value["messages"] == [], "Validation diagnostics are not clean")


def workload(profile: str) -> dict:
    return {"kind": "workload", "duration_seconds": DURATIONS[profile], "active_warmup_frames": 32,
            "warmup_minimize_restore_cycles": 1,
            "resize_transitions": 100, "minimize_restore_cycles": 20, "lifetimes": 20,
            "lifetime_frames": 16, "lifetime_warmup": 5, "sample_interval_ms": 5000,
            "attempt_limit": 16, "handle_allowance": 8, "private_bytes_allowance": MEMORY_LIMITS[profile],
            "gdi_allowance": 0, "user_allowance": 0,
            "native_allocation_bytes": "unavailable"}


def inspect_result(result: ProcessResult, mode: str, device: str, validation: str,
                   lock: str, source: str, profile: str) -> tuple[list[dict], list[str]]:
    records, errors = [], []
    try:
        require(profile in DURATIONS, "Unknown compiled workload profile")
        require(result.returncode == 0 and not result.timed_out and not result.cleanup_errors,
                "Stress process failed, timed out or left process-tree cleanup errors")
        require(not result.stderr.strip(), "Stress process wrote unexpected stderr")
        require(0 < len(result.stdout.encode("utf-8")) <= 16 * 1024 * 1024, "Stress output is empty or oversized")
        for line in result.stdout.splitlines():
            row = json.loads(line, object_pairs_hook=unique_object, parse_constant=reject_constant,
                             parse_float=finite_float)
            require(isinstance(row, dict), "Every stress record must be an object")
            records.append(row)
        require(len(records) >= 3, "Stress identity/workload records are missing")
        identity = records[0]
        for key, expected in {"kind": "identity", "schema_version": 1, "mode": mode, "profile": profile,
                              "dependency_lock_sha256": lock, "source_sha256": source,
                              "validation": validation, "selection": device, "assignment_05": "NOT_RUN"}.items():
            require(type(identity.get(key)) is type(expected) and identity[key] == expected,
                    f"Stress identity disagrees with requested/current input: {key}")
        require(identity.get("diagnostic") is False, "Resource-growth diagnostic execution cannot qualify")
        require(isinstance(identity.get("compiler"), str) and identity["compiler"], "Compiler identity is missing")
        expected_workload = workload(profile)
        require(records[1] == expected_workload and all(type(records[1][key]) is type(value)
                for key, value in expected_workload.items()), "Predeclared workload or resource allowances changed")
        if mode == "describe":
            row = records[2]
            require(len(records) == 3 and row.get("kind") == "description" and row.get("status") == "PASS"
                    and row.get("qualification") == "NOT_RUN" and row.get("device_accessed") is False
                    and row.get("native_graphics_available") is True, "Description must be complete and device-free")
            return records, errors
        require(mode == "stress", "Unknown report mode")
        cursor = 2

        def next_row(kind: str) -> dict:
            nonlocal cursor
            require(cursor < len(records), f"Missing {kind}; stress workload is incomplete")
            row = records[cursor]
            cursor += 1
            require(row.get("kind") == kind, f"Expected {kind}, received {row.get('kind')}")
            return row

        def new_owner(row: dict, adapter: dict | None = None) -> tuple[dict, dict]:
            errors = smoke.foundation.adapter_errors(row.get("adapter"), require_supported=True)
            require(not errors, "; ".join(errors))
            selected = row["adapter"]
            require(device == "auto" or selected["uuid"] == device, "Native workload selected a different GPU")
            require(adapter is None or selected == adapter, "Device/driver identity changed between lifetimes")
            smoke.window(row.get("window"), (640, 400))
            state = smoke.stats(row.get("stats"))
            require(not state["active"] and not state["closed"] and all(state[key] == 0 for key in
                    ("submitted", "completed", "presented", "generation", "retired_generations",
                     "in_flight", "peak_in_flight", "suspended_frames", "waiting_frames")),
                    "A new owner inherited an earlier lifetime's state")
            return selected, state

        adapter, previous = new_owner(next_row("stress_begin"))
        content, active_baseline = (640, 400), None
        active_values, closed_values = [], []
        elapsed, resized, minimized, periodic = 0, 0, 0, 0
        periodic_ms, periodic_presented = 0, 32

        def read_sample(phase: str, index: int) -> dict:
            nonlocal previous, elapsed, active_baseline
            row = next_row("sample")
            require(row.get("phase") == phase and type(row.get("index")) is int and row["index"] == index,
                    "Resource sample does not follow its declared transition/index")
            observed = smoke.window(row.get("window"), content)
            state = smoke.stats(row.get("stats"))
            smoke.transition(previous, state)
            if phase in ("warmup", "resize", "restore"):
                require(all(state[key] == previous[key] for key in state if key not in ("completed", "in_flight")),
                        "Immediate transition sample performed undeclared GPU work")
            require(state["active"] and state["completed"] == state["submitted"] and state["in_flight"] == 0
                    and state["extent"] == observed["framebuffer"], "Resource sample was not drained and active")
            resources(row.get("resources"), state)
            value = process_resources(row.get("process"))
            if phase == "warmup":
                require(state["presented"] == 32 and row.get("elapsed_ms") == 0,
                        "Active resource baseline requires exactly 32 warmup frames")
                active_baseline = value
            require(row.get("baseline") == active_baseline and row.get("within_budget") is True,
                    "Active resource baseline changed or native budget failed")
            resource_budget(value, active_baseline, profile)
            require(smoke.integer(row.get("elapsed_ms")) and elapsed <= row["elapsed_ms"] <= DURATIONS[profile] * 1000 + 10000,
                    "Sample elapsed time regressed or exceeded the bounded workload")
            elapsed, previous = row["elapsed_ms"], state
            active_values.append(value)
            return row

        warmup = next_row("warmup_minimize")
        smoke.window(warmup.get("window"), content, True)
        before, suspended = smoke.stats(warmup.get("before")), smoke.stats(warmup.get("stats"))
        smoke.transition(previous, before)
        require(type(warmup.get("index")) is int and warmup["index"] == 1
                and before["presented"] == before["submitted"] == 24 and before["suspended_frames"] == 0
                and warmup.get("status") == "suspended"
                and suspended == dict(before, suspended_frames=1),
                "Fixed warmup must suspend after exactly 24 presentations")
        warmup = next_row("warmup_restore")
        require(type(warmup.get("index")) is int and warmup["index"] == 1 and warmup.get("before") == suspended,
                "Warmup restore does not follow its suspended state")
        smoke.window(warmup.get("window"), content)
        restored = smoke.stats(warmup.get("stats"))
        smoke.transition(suspended, restored)
        require(restored["presented"] == restored["submitted"] == 32 and restored["suspended_frames"] == 1,
                "Fixed warmup restore must complete exactly eight more presentations")
        previous = restored
        read_sample("warmup", 0)
        while cursor < len(records) and records[cursor].get("kind") != "sustained_end":
            if records[cursor].get("kind") == "sample":
                if records[cursor].get("phase") == "final":
                    read_sample("final", 0)
                    require(cursor < len(records) and records[cursor].get("kind") == "sustained_end",
                            "Final resource sample must immediately precede sustained completion")
                    break
                periodic += 1
                row = read_sample("periodic", periodic)
                require(5000 <= elapsed - periodic_ms <= 10000 and previous["presented"] > periodic_presented,
                        "Periodic measurement omitted bounded presentation progress")
                periodic_ms, periodic_presented = elapsed, previous["presented"]
                continue
            row = next_row("resize")
            resized += 1
            require(type(row.get("index")) is int and row["index"] == resized <= 100,
                    "Resize index sequence is incomplete or excessive")
            content = smoke.SIZES[(resized - 1) % 4]
            require(smoke.extent(row.get("requested")) == content, "Resize workload changed dimensions")
            smoke.window(row.get("window"), content)
            before, state = smoke.stats(row.get("before")), smoke.stats(row.get("stats"))
            smoke.transition(previous, before)
            smoke.transition(before, state)
            require(state["generation"] > before["generation"] and state["presented"] == before["presented"] + 16,
                    "Resize did not recreate and present its 16 declared frames")
            previous = state
            read_sample("resize", resized)
            if resized % 5 == 0:
                minimized += 1
                row = next_row("minimize")
                require(type(row.get("index")) is int and row["index"] == minimized
                        and row.get("status") == "suspended" and row.get("before") == previous,
                        "Minimize index/status or prior state is invalid")
                smoke.window(row.get("window"), content, True)
                suspended = smoke.stats(row.get("stats"))
                expected = dict(previous, suspended_frames=previous["suspended_frames"] + 1)
                require(suspended == expected, "Minimize changed GPU state instead of only suspending")
                row = next_row("restore")
                require(type(row.get("index")) is int and row["index"] == minimized, "Restore index is invalid")
                smoke.window(row.get("window"), content)
                restored = smoke.stats(row.get("stats"))
                smoke.transition(suspended, restored)
                require(restored["presented"] == suspended["presented"] + 8,
                        "Restore did not present eight frames")
                previous = restored
                read_sample("restore", minimized)
        require(records[cursor - 1].get("kind") == "sample" and records[cursor - 1].get("phase") == "final",
                "Final resource measurement is missing")
        end = next_row("sustained_end")
        final = smoke.stats(end.get("stats"))
        require(final == previous, "Sustained counters changed after the final drained resource sample")
        require(final["completed"] == final["submitted"] and final["in_flight"] == 0,
                "Sustained owner did not drain")
        require(smoke.integer(end.get("elapsed_ms")) and elapsed <= end["elapsed_ms"]
                and DURATIONS[profile] * 1000 <= end["elapsed_ms"] <= DURATIONS[profile] * 1000 + 10000,
                "Sustained duration is shortened or exceeded its final deadline")
        require(resized == end.get("resize_transitions") == 100 and minimized == end.get("minimize_restore_cycles") == 20
                and periodic == end.get("periodic_samples") and periodic >= DURATIONS[profile] // 5 - 1,
                "Sustained transition/sample workload is incomplete")
        require(end["elapsed_ms"] - periodic_ms <= 10000 and final["suspended_frames"] == 21
                and end.get("presented_frames") == final["presented"] - 32 >= 1760,
                "Sustained progress/minimize counts are incomplete")

        def closed(row: dict, prior: dict, *, no_new_work: bool = True) -> dict:
            state = smoke.stats(row.get("closed"))
            smoke.transition(prior, state)
            if no_new_work:
                require(all(state[key] == prior[key] for key in
                            ("submitted", "presented", "generation", "suspended_frames", "waiting_frames",
                             "peak_in_flight", "frame_limit", "synchronization")),
                        "Close performed undeclared GPU work or changed lifetime counters")
            require(state["closed"] and state["presented"] == prior["presented"]
                    and row.get("idempotent_close") is True and row.get("second_closed") == state,
                    "Close changed presentation counters or failed idempotency")
            resources(row.get("resources"), state)
            for name in ("before_close", "after_close", "after_second_close"):
                clean(row.get(name))
            return state

        closed(next_row("active_close"), final)
        closed_baseline = None
        for cycle in range(1, 21):
            row = next_row("lifetime_begin")
            require(type(row.get("cycle")) is int and row["cycle"] == cycle, "Lifetime start sequence is invalid")
            _, initial = new_owner(row, adapter)
            row = next_row("lifetime_end")
            require(type(row.get("cycle")) is int and row["cycle"] == cycle, "Lifetime end sequence is invalid")
            state = smoke.stats(row.get("closed"))
            require(state["presented"] == 16 and state["submitted"] == 16 and state["generation"] >= 1,
                    "Short owner did not complete 16 actual presentations")
            closed(row, dict(initial, presented=16), no_new_work=False)
            value = process_resources(row.get("process"))
            measured = cycle >= 5
            require(row.get("measured") is measured and row.get("within_budget") is True,
                    "Closed resource warmup or native budget state is incorrect")
            if cycle == 5:
                closed_baseline = value
            require(row.get("baseline") == (closed_baseline if measured else dict.fromkeys(PROCESS_FIELDS, 0)),
                    "Closed resource baseline differs from the fifth lifetime")
            if measured:
                resource_budget(value, closed_baseline, profile)
                closed_values.append(value)
        complete = next_row("complete")
        qualification = "PASS" if validation == "required" else "NOT_RUN"
        require(cursor == len(records) and complete.get("execution_status") == "PASS"
                and complete.get("status") == complete.get("qualification") == qualification
                and complete.get("assignment_05") == "NOT_RUN", "Completion is missing, overclaims or has trailing data")
        require(complete.get("diagnostic") is False and all(type(complete.get(key)) is int and complete[key] == 0
                for key in ("active_budget_failures", "closed_budget_failures")),
                "Completion retains resource-budget failures or diagnostic-only execution")
        expected = {"sustained_duration_ms": end["elapsed_ms"], "sustained_presented_frames": end["presented_frames"],
                    "resize_transitions": 100, "minimize_restore_cycles": 20, "lifetimes": 20,
                    "lifetime_presented_frames": 320, "periodic_samples": periodic}
        require(all(type(complete.get(key)) is int and complete[key] == value for key, value in expected.items()),
                "Final aggregate differs from raw observed workload")
        for label, baseline, values in (("active", active_baseline, active_values), ("closed", closed_baseline, closed_values)):
            require(complete.get(label + "_baseline") == baseline and complete.get(label + "_peak") ==
                    {key: max(value[key] for value in values) for key in PROCESS_FIELDS},
                    "Reported resource peak/baseline does not match raw observations")
    except (ValueError, TypeError, KeyError, IndexError) as error:
        errors.append(f"Invalid presentation stress evidence: {error}")
    return records, errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--mode", choices=("describe", "stress"), required=True)
    parser.add_argument("--profile", choices=tuple(DURATIONS), required=True)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--validation", choices=("required", "off"), default="required")
    parser.add_argument("--validation-manifest", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--diagnose-resource-growth", action="store_true",
                        help="Retain an explicitly nonqualifying trace beyond process-budget failures")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.device != "auto" and not re.fullmatch(r"[0-9a-f]{32}", args.device):
        parser.error("--device must be auto or an exact lowercase 32-hex UUID")
    if args.mode == "describe" and (args.device != "auto" or args.validation != "required" or args.diagnose_resource_growth):
        parser.error("Description is device-free and does not accept device/validation overrides")
    executable = args.executable.resolve()
    if sys.platform != "win32" or not executable.is_file():
        parser.error("Requires Windows and the built PresentationStress executable")
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(f"Choose a fresh writable evidence directory: {error}")
    command = [str(executable), "--" + args.mode]
    if args.mode == "stress":
        command.extend(["--validation", args.validation, "--device", args.device])
    if args.diagnose_resource_growth:
        command.append("--diagnose-resource-growth")
    timeout = 780 if args.profile == "release" else 240
    summary = {"schema_version": 1, "checkpoint": "05c", "status": "FAIL", "qualification": "NOT_RUN",
               "assignment_05": "NOT_RUN", "started_utc": datetime.now(timezone.utc).isoformat(),
               "command": command, "mode": args.mode, "profile": args.profile,
               "validation": args.validation, "timeout_seconds": timeout,
               "duration_seconds": DURATIONS[args.profile], "resizes": 100, "minimize_restore_pairs": 20,
               "short_lifetimes": 20, "private_bytes_allowance": MEMORY_LIMITS[args.profile],
               "handle_allowance": 8, "gdi_allowance": 0, "user_allowance": 0,
               "diagnostic": args.diagnose_resource_growth}
    started, exit_code = time.monotonic(), 1
    try:
        lock = sha256(ROOT / "dependencies.lock.json")
        inputs = source_inputs()
        source = source_digest(inputs)
        summary.update(dependency_lock_sha256=lock, source_sha256=source, input_sha256=inputs,
                       executable_sha256=sha256(executable), git_revision=command_output(["git", "rev-parse", "HEAD"]),
                       git_status=command_output(["git", "status", "--short"]))
        summary["tool_inputs"] = {name: sha256(ROOT / "tools" / name) for name in
                                 ("presentation_stress.py", "presentation_smoke.py", "graphics_device_smoke.py",
                                  "graphics_probe.py", "process_runner.py", "toolchain_report.py", "windows_unelevated.py")}
        validation = None
        if args.mode == "stress" and args.validation == "required":
            if args.validation_manifest is None:
                raise Unavailable("Required validation needs a current locked --validation-manifest")
            validation = validation_tools(args.validation_manifest.resolve(), lock)
            summary["validation_tools"] = {"manifest_path": str(args.validation_manifest.resolve()),
                                           "manifest_sha256": sha256(args.validation_manifest), "receipt": validation}
            summary["process_security"] = check_layer_process()
        settings = output / "vk_layer_settings.txt"
        settings.write_text("# Required core/synchronization and NVRHI validation; no overrides.\n", encoding="utf-8")
        implicit = output / "implicit-layers-empty"
        implicit.mkdir()
        environment, summary["layer_environment"] = layer_environment(validation, settings, implicit)
        (output / "environment.json").write_text(json.dumps(collect(args.build_dir.resolve() if args.build_dir else None),
                                                            indent=2) + "\n", encoding="utf-8")
        result = run_process(command, cwd=executable.parent, timeout=timeout, env=environment)
        (output / "stdout.jsonl").write_text(result.stdout, encoding="utf-8")
        (output / "stderr.log").write_text(result.stderr, encoding="utf-8")
        records, errors = inspect_result(result, args.mode, args.device, args.validation, lock, source, args.profile)
        if validation is not None and validation_tools(args.validation_manifest.resolve(), lock) != validation:
            errors.append("Pinned validation receipt changed during execution")
        if source_inputs() != inputs or sha256(executable) != summary["executable_sha256"]:
            errors.append("Source inputs or executable changed during execution")
        if any(sha256(ROOT / "tools" / name) != digest for name, digest in summary["tool_inputs"].items()):
            errors.append("Verification tools changed during execution")
        summary.update(records=records, errors=errors, process_exit_code=result.returncode,
                       timed_out=result.timed_out, cleanup_errors=list(result.cleanup_errors))
        exit_code = 124 if result.timed_out else (1 if errors else result.returncode)
        if exit_code == 0:
            summary["status"] = "NOT_RUN" if args.mode == "stress" and args.validation == "off" else "PASS"
            if args.mode == "stress" and args.validation == "required":
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
