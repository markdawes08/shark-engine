"""Run the bounded 05b presentation lifecycle workload and retain strict evidence.

This requires a real Windows display/GPU. Four lifetimes each present 96 clears,
resize four times and minimize/restore twice. It is not the sustained 05c gate.
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

CYCLES = 4
FRAMES = 96
ATTEMPTS = 16
SIZES = ((700, 420), (530, 350), (800, 450), (640, 400))
source_digest = foundation.source_digest


def source_inputs() -> dict[str, str]:
    inputs = foundation.source_inputs()
    del inputs["tests/graphics_device_smoke.cpp"]
    inputs["tests/presentation_smoke.cpp"] = sha256(ROOT / "tests/presentation_smoke.cpp")
    return dict(sorted(inputs.items()))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def integer(value: object, maximum: int = 0xFFFFFFFFFFFFFFFF) -> bool:
    return type(value) is int and 0 <= value <= maximum


def extent(value: object, *, nonzero: bool = True) -> tuple[int, int]:
    require(isinstance(value, dict) and set(value) == {"width", "height"}, "Extent fields are invalid")
    require(all(integer(value[key], 0xFFFFFFFF) for key in ("width", "height")), "Extent must contain unsigned integers")
    size = value["width"], value["height"]
    require(not nonzero or all(size), "Active framebuffer extent must be nonzero")
    return size


def window(value: object, content: tuple[int, int], iconified: bool = False) -> dict:
    require(isinstance(value, dict), "Missing native window observation")
    observed = extent(value.get("content"), nonzero=not iconified)
    require(observed == content or (iconified and observed == (0, 0)),
            "Observed content extent differs from requested workload")
    extent(value.get("framebuffer"), nonzero=not iconified)
    require(value.get("visible") is True and value.get("iconified") is iconified
            and value.get("close_requested") is False, "Native window state interrupted the declared workload")
    return value


def stats(value: object) -> dict:
    require(isinstance(value, dict), "Missing presentation counters")
    fields = ("image_count", "frame_limit", "in_flight", "peak_in_flight", "submitted", "completed",
              "presented", "generation", "retired_generations", "suspended_frames", "waiting_frames")
    require(all(integer(value.get(key)) for key in fields), "Presentation counters must be unsigned integers")
    require(all(type(value.get(key)) is bool for key in ("active", "faulted", "closed")), "Presentation state flags must be boolean")
    require(not value["faulted"], "Faulted presentation cannot qualify")
    require(value.get("synchronization") in ("present_fence", "host_completion"), "Unknown synchronization strategy")
    require(1 <= value["frame_limit"] <= 2, "Presentation frame limit must be bounded by two")
    require(0 <= value["in_flight"] <= value["peak_in_flight"] <= value["frame_limit"], "Frames in flight exceed their declared bound")
    require(value["completed"] <= value["submitted"] and value["presented"] <= value["submitted"], "GPU completion/presentation exceeds submission")
    require(value["submitted"] - value["completed"] <= value["frame_limit"], "Uncompleted submissions exceed their declared bound")
    require(value["retired_generations"] <= value["generation"], "Retirement exceeds created generations")
    extent(value.get("extent"), nonzero=value["active"])
    if value["generation"]:
        expected = 2 if value["synchronization"] == "present_fence" else 1
        require(value["frame_limit"] == expected, "Frame limit disagrees with native synchronization strategy")
    if value["active"]:
        require(value["generation"] > 0 and 1 <= value["image_count"] <= 8 and not value["closed"], "Active swapchain metadata is invalid")
        require(value["retired_generations"] == value["generation"] - 1, "An old generation remains live after replacement")
    if value["closed"]:
        require(not value["active"] and value["in_flight"] == 0
                and value["completed"] == value["submitted"]
                and value["retired_generations"] == value["generation"], "Close has not retired all completed GPU uses")
    return value


def transition(before: dict, after: dict) -> None:
    for key in ("submitted", "completed", "presented", "generation", "retired_generations",
                "suspended_frames", "waiting_frames", "peak_in_flight"):
        require(after[key] >= before[key], f"Presentation counter regressed: {key}")
    if before["generation"]:
        require(after["synchronization"] == before["synchronization"]
                and after["frame_limit"] == before["frame_limit"], "Synchronization strategy changed during ownership")


def inspect_result(result: ProcessResult, mode: str, device: str, validation: str,
                   lock_digest: str, inputs_digest: str) -> tuple[list[dict], list[str]]:
    errors = list(result.cleanup_errors)
    if result.timed_out:
        errors.append("Presentation deadline exceeded; the owned process tree was terminated")
    if result.returncode != 0:
        errors.append(f"Presentation fixture exited {result.returncode}")
    if result.stderr.strip():
        errors.append("Presentation fixture emitted unexpected stderr; retain and investigate it")
    if len(result.stdout.encode("utf-8")) > 8 * 1024 * 1024:
        return [], [*errors, "Presentation report exceeds its byte budget"]
    records = []
    try:
        for line in result.stdout.splitlines():
            row = json.loads(line, object_pairs_hook=unique_object,
                             parse_constant=reject_constant, parse_float=finite_float)
            require(isinstance(row, dict), "Each presentation report row must be an object")
            records.append(row)
        require(bool(records), "Presentation report is empty")
        identity = records[0]
        for key, expected in (("kind", "identity"), ("schema_version", 1), ("mode", mode),
                              ("dependency_lock_sha256", lock_digest), ("source_sha256", inputs_digest),
                              ("validation", validation), ("selection", device), ("assignment_05", "NOT_RUN")):
            require(identity.get(key) == expected, f"Presentation identity disagrees with command/sources: {key}")
        require(type(identity.get("schema_version")) is int and isinstance(identity.get("compiler"), str)
                and bool(identity["compiler"]), "Presentation identity lacks schema/compiler metadata")
        if mode == "describe":
            require(len(records) == 2, "Description must contain exactly two rows")
            row = records[1]
            require(row.get("kind") == "description" and row.get("status") == "PASS"
                    and row.get("qualification") == "NOT_RUN" and row.get("device_accessed") is False,
                    "Description must remain device-free and cannot qualify presentation")
            for key, expected in (("cycles", CYCLES), ("frames_per_cycle", FRAMES), ("resizes_per_cycle", 4),
                                  ("minimizes_per_cycle", 2), ("attempt_limit", ATTEMPTS)):
                require(type(row.get(key)) is int and row[key] == expected, f"Description changed declared workload: {key}")
            return records, errors
        require(mode == "smoke", "Unknown presentation report mode")
        cursor = 1
        selected = set()
        required = validation == "required"

        def next_row(kind: str, cycle: int | None = None) -> dict:
            nonlocal cursor
            require(cursor < len(records), f"Missing {kind} row; workload is incomplete")
            row = records[cursor]
            cursor += 1
            require(row.get("kind") == kind, f"Expected {kind}, received {row.get('kind')}")
            if cycle is not None:
                require(type(row.get("cycle")) is int and row["cycle"] == cycle, "Lifecycle cycle order is invalid")
            return row

        for cycle in range(1, CYCLES + 1):
            row = next_row("cycle_begin", cycle)
            adapter = row.get("adapter")
            adapter_problems = foundation.adapter_errors(adapter, require_supported=True)
            require(not adapter_problems, "; ".join(adapter_problems))
            selected.add(adapter["uuid"])
            require(device == "auto" or adapter["uuid"] == device, "Selected UUID differs from explicit request")
            content = (640, 400)
            window(row.get("window"), content)
            previous = stats(row.get("stats"))
            require(all(previous[key] == 0 for key in ("submitted", "completed", "presented", "generation",
                                                       "retired_generations", "in_flight", "suspended_frames", "waiting_frames"))
                    and not previous["active"] and not previous["closed"], "New owner reused prior presentation state")

            def frames(stage: str, count: int) -> None:
                nonlocal previous
                for frame in range(1, count + 1):
                    finished = False
                    for attempt in range(1, ATTEMPTS + 1):
                        row = next_row("frame", cycle)
                        require(row.get("stage") == stage and type(row.get("frame")) is int
                                and row["frame"] == frame and type(row.get("attempt")) is int
                                and row["attempt"] == attempt, "Frame/attempt sequence differs from declared workload")
                        observed = window(row.get("window"), content)
                        current = stats(row.get("stats"))
                        transition(previous, current)
                        require(current["submitted"] <= previous["submitted"] + 1,
                                "One present call submitted more than one frame")
                        require(type(row.get("recreated")) is bool
                                and current["generation"] == previous["generation"] + int(row["recreated"]),
                                "Generation change disagrees with recreation report")
                        require(not current["closed"] and current["suspended_frames"] == previous["suspended_frames"],
                                "Visible frame was closed or suspended")
                        status = row.get("status")
                        if status == "presented":
                            require(current["presented"] == previous["presented"] + 1 and current["active"]
                                    and current["extent"] == observed["framebuffer"],
                                    "Presented frame lacks matching extent or advanced presentation count")
                            finished = True
                        else:
                            require(status in ("waiting", "recreate_pending")
                                    and current["presented"] == previous["presented"],
                                    "Visible frame failed without a bounded retry outcome")
                        previous = current
                        if finished:
                            break
                    require(finished, "Frame exhausted the declared retry budget")

            frames("initial", 16)
            row = next_row("owner_conflict", cycle)
            require(row.get("second_owner_rejected") is True and isinstance(row.get("error"), dict)
                    and row["error"].get("code") == "conflict", "Second owner must reject active presentation with conflict")
            require(stats(row.get("stats")) == previous, "Second owner damaged active presentation state")
            for index, content in enumerate(SIZES, 1):
                row = next_row("resize", cycle)
                require(type(row.get("index")) is int and row["index"] == index
                        and extent(row.get("requested")) == content, "Resize sequence differs from declared workload")
                window(row.get("window"), content)
                require(stats(row.get("stats")) == previous, "Resize observation lost prior synchronization state")
                old_generation = previous["generation"]
                frames(f"resize{index}", 16)
                require(previous["generation"] > old_generation, "Resize did not replace its swapchain generation")
                if index in (2, 4):
                    ordinal = index // 2
                    row = next_row("minimize", cycle)
                    require(type(row.get("index")) is int and row["index"] == ordinal
                            and row.get("status") == "suspended", "Minimize must suspend presentation")
                    window(row.get("window"), content, True)
                    require(stats(row.get("before")) == previous, "Minimize omitted prior state")
                    current = stats(row.get("stats"))
                    transition(previous, current)
                    require(current["submitted"] == previous["submitted"]
                            and current["presented"] == previous["presented"]
                            and current["suspended_frames"] == previous["suspended_frames"] + 1,
                            "Minimize submitted/presented work or failed to record suspension")
                    previous = current
                    row = next_row("restore", cycle)
                    require(type(row.get("index")) is int and row["index"] == ordinal, "Restore order differs from workload")
                    window(row.get("window"), content)
                    require(stats(row.get("stats")) == previous, "Restore unexpectedly submitted GPU work")
                    frames(f"restore{ordinal}", 8)
            row = next_row("cycle_end", cycle)
            require(row.get("status") == ("PASS" if required else "DIAGNOSTIC")
                    and row.get("idempotent_close") is True
                    and type(row.get("presented_frames")) is int and row["presented_frames"] == FRAMES,
                    "Lifecycle teardown failed or omitted declared frames")
            require(stats(row.get("before_drain")) == previous, "Drain omitted prior GPU state")
            drained = stats(row.get("drained"))
            transition(previous, drained)
            require(drained["completed"] == drained["submitted"] and drained["in_flight"] == 0
                    and drained["submitted"] == previous["submitted"] and drained["presented"] == FRAMES
                    and drained["suspended_frames"] == 2 and not drained["closed"], "Explicit drain did not complete the workload")
            closed = stats(row.get("closed"))
            transition(drained, closed)
            require(closed["closed"] and closed["generation"] >= 5 and closed["generation"] == drained["generation"]
                    and closed["submitted"] == drained["submitted"] and closed["presented"] == FRAMES
                    and row.get("second_closed") == closed, "Close failed to retire generations or changed on repetition")
            for key in ("before_close", "after_close", "after_second_close"):
                diagnostics = row.get(key)
                problems = foundation.diagnostic_errors(diagnostics, require_clean=required)
                require(not problems, "; ".join(problems))
                require(len(diagnostics["messages"]) <= 64 and all(len(message) <= 2044 for message in diagnostics["messages"]),
                        "Diagnostics exceed callback storage bounds")
                require(not required or not diagnostics["messages"], "Qualification retained validation messages")
            require(row["after_close"] == row["after_second_close"], "Repeated close changed cumulative diagnostics")
        require(len(selected) == 1, "Repeated lifetimes selected different adapters")
        row = next_row("complete")
        expected = "PASS" if required else "NOT_RUN"
        for key, value in (("status", expected), ("qualification", expected), ("execution_status", "PASS"),
                           ("cycles", CYCLES), ("presented_frames", CYCLES * FRAMES),
                           ("resize_transitions", CYCLES * 4), ("minimize_restore_cycles", CYCLES * 2),
                           ("assignment_05", "NOT_RUN")):
            require(type(row.get(key)) is type(value) and row[key] == value, f"Final completion is inconsistent: {key}")
        require(cursor == len(records), "Unexpected records follow completed workload")
    except (ValueError, TypeError, KeyError) as error:
        errors.append(f"Invalid presentation evidence: {error}")
    return records, errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--mode", choices=("describe", "smoke"), required=True)
    parser.add_argument("--device", default="auto", help="auto or exact lowercase 32-hex Vulkan UUID")
    parser.add_argument("--validation", choices=("required", "off"), default="required")
    parser.add_argument("--validation-manifest", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="Fresh evidence directory")
    args = parser.parse_args()
    if args.device != "auto" and not re.fullmatch(r"[0-9a-f]{32}", args.device):
        parser.error("--device must be auto or exactly 32 lowercase hex characters")
    if args.mode == "describe" and (args.device != "auto" or args.validation != "required"):
        parser.error("Description mode performs no device selection or validation setup")
    executable = args.executable.resolve()
    if sys.platform != "win32" or not executable.is_file():
        parser.error("Requires Windows and the built PresentationSmoke executable")
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(f"Choose a fresh writable evidence directory: {error}")
    command = [str(executable), "--" + args.mode]
    if args.mode == "smoke":
        command.extend(["--validation", args.validation, "--device", args.device])
    summary = {"schema_version": 1, "checkpoint": "05b", "status": "FAIL", "qualification": "NOT_RUN",
               "assignment_05": "NOT_RUN", "started_utc": datetime.now(timezone.utc).isoformat(),
               "command": command, "mode": args.mode, "validation": args.validation,
               "timeout_seconds": 120, "cycles": CYCLES, "frames_per_cycle": FRAMES,
               "resizes_per_cycle": 4, "minimizes_per_cycle": 2, "attempt_limit": ATTEMPTS,
               "scope": "Bounded presentation clears/lifetimes; no readback, scene or sustained 05c qualification"}
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
                                  ("presentation_smoke.py", "graphics_device_smoke.py", "graphics_probe.py",
                                   "process_runner.py", "toolchain_report.py", "windows_unelevated.py")}
        validation = None
        if args.mode == "smoke" and args.validation == "required":
            if args.validation_manifest is None:
                raise Unavailable("Required validation needs --validation-manifest from locked tool preparation")
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
        result = run_process(command, cwd=executable.parent, timeout=120, env=environment)
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
            summary["status"] = "NOT_RUN" if args.mode == "smoke" and args.validation == "off" else "PASS"
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
