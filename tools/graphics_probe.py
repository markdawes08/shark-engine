"""Run the graphics setup probe with a deadline and retain machine-readable evidence."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
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


def probe_source_digest() -> str:
    paths = ("probes/graphics/CMakeLists.txt", "probes/graphics/main.cpp", "probes/graphics/report.hpp")
    text = "".join(f"{path}\n{sha256(ROOT / path)}\n" for path in paths)
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def inspect_result(result: ProcessResult, mode: str, lock_digest: str, source_digest: str,
                   device: str | None = None, validation: str = "required") -> tuple[dict | None, list[str]]:
    """Never turn a timeout, stale build or malformed/contradictory report into a pass."""
    errors = list(result.cleanup_errors)
    if result.timed_out:
        errors.append("Probe deadline exceeded; the owned process tree was terminated")
    try:
        report = json.loads(result.stdout, object_pairs_hook=unique_object,
                            parse_constant=reject_constant)
    except (ValueError, TypeError) as error:
        return None, [*errors, f"Invalid probe JSON: {error}"]
    if not isinstance(report, dict):
        return None, [*errors, "Probe report must be an object"]
    if type(report.get("schema_version")) is not int or report["schema_version"] != 1:
        errors.append("Unsupported probe report schema")
    if report.get("mode") != mode:
        errors.append("Report mode does not match the command")
    if report.get("dependency_lock_sha256") != lock_digest:
        errors.append("Probe was built with a different dependency lock; reconfigure and rebuild")
    if report.get("probe_source_sha256") != source_digest:
        errors.append("Probe sources changed since the build; reconfigure and rebuild")
    if report.get("qualification") != "NOT_RUN":
        errors.append("02a setup cannot claim assignment 02 hardware qualification")
    cases = report.get("cases")
    if (not isinstance(cases, list) or len(cases) != 8
            or any(not isinstance(case, dict) or not isinstance(case.get("id"), str)
                   or case.get("status") != "NOT_RUN" for case in cases)
            or {case.get("id") for case in cases} != {f"P02-{number:02d}" for number in range(1, 9)}):
        errors.append("Report must retain all eight unqualified assignment 02 cases")
    expected_status = {0: "PASS", 1: "FAIL", 2: "FAIL", 3: "NOT_RUN"}.get(result.returncode)
    if expected_status is None or report.get("status") != expected_status:
        errors.append("Probe status does not agree with its process exit code")
    if type(report.get("exit_code")) is not int or report["exit_code"] != result.returncode:
        errors.append("Report exit_code does not agree with the process")
    if result.returncode == 0:
        for field in ("validation_warnings", "validation_errors"):
            if type(report.get(field)) is not int or report[field] != 0:
                errors.append(f"A passing setup must report zero {field}")
        expected_stages = [] if mode == "describe" else ["loader", "instance", "surface", "inventory"]
        if mode == "smoke":
            expected_stages += ["device", "nvrhi_device_and_validation", "swapchain"]
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
        if report.get("vulkan_validation_enabled") is not (mode == "smoke" and validation == "required"):
            errors.append("Vulkan validation state does not match the requested operation")
        if report.get("nvrhi_validation_enabled") is not (mode == "smoke"):
            errors.append("NVRHI validation state does not match the operation")
        if mode == "smoke":
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
                        or type(swapchain.get("presented_frames")) is not int or swapchain["presented_frames"] != 0):
                    errors.append("Invalid setup swapchain evidence")
    elif result.returncode in (1, 2, 3):
        error = report.get("error")
        if (not isinstance(error, dict) or not isinstance(error.get("code"), str) or not error["code"]
                or not isinstance(error.get("message"), str) or not error["message"]):
            errors.append("Failed/unavailable operations need an actionable structured error")
    return report, errors


def source_state() -> dict:
    """Record reviewed inputs, including uncommitted sources, without capturing secrets."""
    paths = [ROOT / "CMakeLists.txt", ROOT / "CMakePresets.json", ROOT / "dependencies.lock.json"]
    for directory in ("probes/graphics", "cmake"):
        paths.extend(path for path in (ROOT / directory).rglob("*") if path.is_file())
    paths.extend(ROOT / "tools" / name for name in
                 ("graphics_probe.py", "prepare_graphics_tools.py", "process_runner.py"))
    return {"git_revision": command_output(["git", "rev-parse", "HEAD"]),
            "git_status": command_output(["git", "status", "--short"]),
            "input_sha256": {path.relative_to(ROOT).as_posix(): sha256(path)
                             for path in sorted(set(paths)) if path.is_file()}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--mode", choices=("describe", "inventory", "smoke"), required=True)
    parser.add_argument("--device", help="Exact 32-character lower-case Vulkan device UUID for smoke")
    parser.add_argument("--validation", choices=("required", "off"), default="required")
    parser.add_argument("--timeout", type=int, default=30, help="Process deadline in seconds (1-300)")
    parser.add_argument("--output", type=Path, required=True, help="New evidence directory; never overwritten")
    parser.add_argument("--build-dir", type=Path, help="Configured build directory for compiler metadata")
    args = parser.parse_args()
    if not 1 <= args.timeout <= 300:
        parser.error("--timeout must be between 1 and 300")
    if args.mode == "smoke":
        if not args.device or not re.fullmatch(r"[0-9a-f]{32}", args.device):
            parser.error("smoke requires an explicit 32-character lower-case device UUID")
    elif args.device or args.validation != "required":
        parser.error("--device and --validation off apply only to smoke")
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error(f"Probe executable does not exist: {executable}")
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(f"Choose a new writable evidence directory: {error}")
    command = [str(executable), "--" + args.mode]
    if args.mode == "smoke":
        command.extend(["--device", args.device, "--validation", args.validation])
    summary = {"schema_version": 1, "assignment": "02", "subchunk": "02a",
               "qualification": "NOT_RUN", "status": "FAIL", "mode": args.mode,
               "started_utc": datetime.now(timezone.utc).isoformat(), "command": command,
               "timeout_seconds": args.timeout, "exit_code": 1}
    started = time.monotonic()
    exit_code = 1
    try:
        digest = sha256(ROOT / "dependencies.lock.json")
        summary.update({"dependency_lock_sha256": digest, "executable_sha256": sha256(executable),
                        "source": source_state()})
        environment = collect(args.build_dir.resolve() if args.build_dir else None)
        (output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n", encoding="utf-8")
        result = run_process(command, cwd=ROOT, timeout=args.timeout)
        (output / "stdout.json").write_text(result.stdout, encoding="utf-8")
        (output / "stderr.log").write_text(result.stderr, encoding="utf-8")
        report, errors = inspect_result(result, args.mode, digest, probe_source_digest(), args.device, args.validation)
        summary.update({"process_exit_code": result.returncode, "timed_out": result.timed_out,
                        "report": report, "errors": errors})
        exit_code = 124 if result.timed_out else (1 if errors else result.returncode)
        if not errors:
            summary["status"] = report["status"]
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
