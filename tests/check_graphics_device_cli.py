"""Exercise graphics device CLI parsing without initializing a display or GPU."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import graphics_device_smoke as smoke
from graphics_probe import finite_float, reject_constant, sha256, unique_object
from process_runner import run_process
from toolchain_report import ROOT


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    environment = dict(os.environ)
    environment.pop("DISPLAY", None)
    environment.pop("WAYLAND_DISPLAY", None)
    # A malformed driver path makes accidental Vulkan setup fail explicitly.
    environment["VK_DRIVER_FILES"] = str(ROOT / "does-not-exist-05a-icd.json")
    lock = sha256(ROOT / "dependencies.lock.json")
    sources = smoke.source_digest(smoke.source_inputs())
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

    for arguments, mode in (([], "describe"), (["--help"], "help"), (["--describe"], "describe")):
        rows = run(arguments, 0)
        assert len(rows) == 2, rows
        identity, description = rows
        assert identity["kind"] == "identity" and identity["mode"] == mode, identity
        assert identity["schema_version"] == 1 and type(identity["schema_version"]) is int, identity
        assert identity["dependency_lock_sha256"] == lock, identity
        assert identity["source_sha256"] == sources, identity
        assert identity["assignment_05"] == "NOT_RUN" and identity["validation"] == "required", identity
        assert description["kind"] == "description" and description["status"] == "PASS", description
        assert description["device_accessed"] is False, description
        assert description["qualification"] == "NOT_RUN" and description["usage"], description

    invalid = [["--unknown"], ["--device", "auto"], ["--describe", "--smoke"],
               ["--help", "--validation", "required"], ["--describe", "--device", "auto"],
               ["--inventory", "--device", "auto"], ["--inventory", "--validation"],
               ["--inventory", "--validation", "unknown"], ["--smoke", "--device"],
               ["--smoke", "--device", "short"], ["--smoke", "--device", "A" * 32],
               ["--smoke", "--device", "0" * 33], ["--smoke", "--device", "g" * 32],
               ["--smoke", "--validation", "unknown"],
               ["--smoke", "--device", "auto", "--device", "auto"],
               ["--smoke", "--validation", "off", "--validation", "off"],
               ["--smoke", "--validation", "off", "--device", "auto", "--extra", "bad"],
               ["--smoke", "--device", "auto", "--extra", "bad"],
               ["--unknown=quote\"slash\\\n\u6e2c\u8a66\U0001f988"]]
    for arguments in invalid:
        rows = run(arguments, 1)
        assert len(rows) == 1 and rows[0]["kind"] == "failure", rows
        assert rows[0]["status"] == "FAIL", rows
        error = rows[0]["error"]
        assert error["code"] == "invalid_argument" and error["message"], error
        assert error["operation"] == "graphics-device-smoke.arguments", error
    print(f"PASS: {count} CPU-only graphics-device CLI checks; no GPU qualification")


if __name__ == "__main__":
    main()
