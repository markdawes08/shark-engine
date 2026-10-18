"""Exercise the compiled stress policy and CLI without a graphics driver/display."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import presentation_stress as stress
from graphics_probe import finite_float, reject_constant, sha256, unique_object
from process_runner import ProcessResult, run_process
from toolchain_report import ROOT


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    environment = dict(os.environ)
    environment.pop("DISPLAY", None)
    environment.pop("WAYLAND_DISPLAY", None)
    environment["VK_DRIVER_FILES"] = str(ROOT / "does-not-exist-05c-stress-icd.json")
    lock = sha256(ROOT / "dependencies.lock.json")
    sources = stress.source_digest(stress.source_inputs())
    count = 0

    def run(arguments: list[str], expected: int) -> list[dict]:
        nonlocal count
        result = run_process([str(executable), *arguments], cwd=executable.parent,
                             env=environment, timeout=5)
        assert not result.timed_out and not result.cleanup_errors, result
        assert result.returncode == expected and not result.stderr, result
        rows = [json.loads(line, object_pairs_hook=unique_object,
                           parse_constant=reject_constant, parse_float=finite_float)
                for line in result.stdout.splitlines()]
        assert rows and all(isinstance(row, dict) for row in rows), result
        count += 1
        return rows

    profile = None
    for arguments, mode in (([], "describe"), (["--help"], "help"), (["--describe"], "describe")):
        rows = run(arguments, 0)
        assert rows[0]["mode"] == mode, rows
        current_profile = rows[0]["profile"]
        assert current_profile in stress.DURATIONS, rows
        assert profile is None or current_profile == profile, rows
        profile = current_profile
        rows[0]["mode"] = "describe"
        process = ProcessResult([str(executable)], 0, "\n".join(json.dumps(row) for row in rows), "", False)
        _, errors = stress.inspect_result(process, "describe", "auto", "required", lock, sources, profile)
        assert not errors, errors
        assert rows[2]["usage"], rows

    invalid = [["--unknown"], ["--smoke"], ["--device", "auto"], ["--describe", "--stress"],
               ["--help", "--validation", "required"], ["--describe", "--device", "auto"],
               ["--describe", "--profile", profile], ["--stress", "--device"],
               ["--describe", "--diagnose-resource-growth"],
               ["--help", "--diagnose-resource-growth"],
               ["--stress", "--diagnose-resource-growth", "--diagnose-resource-growth"],
               ["--stress", "--diagnose-resource-growth", "unexpected"],
               ["--stress", "--diagnose-resource-growth", "--validation", "invalid"],
               ["--stress", "--device", "short"], ["--stress", "--device", "A" * 32],
               ["--stress", "--device", "0" * 33], ["--stress", "--device", "g" * 32],
               ["--stress", "--validation"], ["--stress", "--validation", "unknown"],
               ["--stress", "--profile"], ["--stress", "--profile", "unknown"],
               ["--stress", "--device", "auto", "--device", "auto"],
               ["--stress", "--validation", "off", "--validation", "off"],
               ["--stress", "--profile", profile, "--profile", profile],
               ["--stress", "--device", "auto", "--extra", "bad"],
               ["--stress", "--profile", profile, "--device", "auto", "--validation", "off", "--extra", "bad"],
               ["--unknown=quote\"slash\\\n\u6e2c\u8a66\U0001f988"]]
    invalid.extend(["--stress", "--profile", mismatch] for mismatch in stress.DURATIONS if mismatch != profile)
    for arguments in invalid:
        rows = run(arguments, 1)
        assert len(rows) == 1 and rows[0]["kind"] == "failure" and rows[0]["status"] == "FAIL", rows
        error = rows[0]["error"]
        assert error["code"] == "invalid_argument" and error["message"], error
        assert error["operation"] == "presentation-stress.arguments", error
    print(f"PASS: {count} CPU-only stress CLI checks; compiled profile {profile}; no GPU qualification")


if __name__ == "__main__":
    main()
