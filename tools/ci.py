"""Run the same configure/build/CTest sequence locally and in hosted CPU CI."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from toolchain_report import ROOT, pinned_errors, write_report
from process_runner import run_process

PRESETS = (
    "windows-debug", "windows-release", "windows-asan",
    "linux-debug", "linux-release", "linux-asan-ubsan",
    "macos-debug", "macos-release", "macos-asan-ubsan",
)


def run_phase(name: str, command: list[str], output: Path, timeout: int) -> dict:
    print(f"[{name}] {subprocess.list2cmdline(command)}", flush=True)
    started = time.monotonic()
    try:
        result = run_process(command, cwd=ROOT, timeout=timeout)
        log = result.stdout + result.stderr
        exit_code = result.returncode
        if result.timed_out:
            log += f"\nTIMEOUT after {timeout} seconds\n"
        if result.cleanup_errors:
            log += "\n" + "\n".join(result.cleanup_errors) + "\n"
            if exit_code == 0:
                exit_code = 125
    except OSError as error:
        log = str(error) + "\n"
        exit_code = 127
    (output / f"{name}.log").write_text(log, encoding="utf-8")
    print(log, end="" if log.endswith("\n") else "\n", flush=True)
    return {"phase": name, "command": command, "exit_code": exit_code,
            "elapsed_seconds": round(time.monotonic() - started, 3),
            "status": "PASS" if exit_code == 0 else "FAIL"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", required=True, choices=PRESETS)
    parser.add_argument("--inject-test-failure", action="store_true",
                        help="Enable the real deliberate failing CTest; this invocation must fail")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=1200, help="Seconds per subprocess phase")
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout < 1:
        parser.error("--jobs and --timeout must be positive")
    output = ROOT / "out/ci" / args.preset
    output.mkdir(parents=True, exist_ok=True)
    # Prevent a prior run's evidence from appearing in a failed current run.
    for name in ("ctest.xml", "configure.log", "build.log", "test.log"):
        (output / name).unlink(missing_ok=True)
    summary = {"schema_version": 1, "preset": args.preset,
               "started_utc": datetime.now(timezone.utc).isoformat(),
               "injected_failure": args.inject_test_failure, "status": "FAIL", "phases": []}
    exit_code = 1
    try:
        errors = pinned_errors(write_report(output / "toolchain.json"))
        if errors:
            summary["toolchain_errors"] = errors
            print("\n".join(errors), file=sys.stderr)
            return 1
        configure = ["cmake", "--preset", args.preset,
                     "-DSHARK_INJECT_TEST_FAILURE=" + ("ON" if args.inject_test_failure else "OFF"),
                     "-DPython3_EXECUTABLE=" + sys.executable]
        if os.environ.get("CXX"):
            configure.append("-DCMAKE_CXX_COMPILER=" + os.environ["CXX"])
        commands = [
            ("configure", configure),
            ("build", ["cmake", "--build", "--preset", args.preset, "--parallel", str(args.jobs)]),
            ("test", ["ctest", "--preset", args.preset, "--output-on-failure", "--no-tests=error",
                      "--output-junit", str(output / "ctest.xml")]),
        ]
        for name, command in commands:
            phase = run_phase(name, command, output, args.timeout)
            summary["phases"].append(phase)
            if name == "configure":
                write_report(output / "toolchain.json", ROOT / "build" / args.preset)
            exit_code = phase["exit_code"]
            if exit_code != 0:
                return exit_code if exit_code > 0 else 1
        summary["status"] = "PASS"
        return 0
    finally:
        summary["exit_code"] = exit_code
        summary["finished_utc"] = datetime.now(timezone.utc).isoformat()
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
