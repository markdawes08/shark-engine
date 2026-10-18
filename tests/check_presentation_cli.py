"""Exercise presentation CLI parsing without initializing a display or GPU."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import presentation_smoke as smoke
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
    environment["VK_DRIVER_FILES"] = str(ROOT / "does-not-exist-05b-icd.json")
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
        assert rows[0]["mode"] == mode, rows
        rows[0]["mode"] = "describe"
        process = smoke.ProcessResult([str(executable)], 0, "\n".join(json.dumps(row) for row in rows), "", False)
        _, errors = smoke.inspect_result(process, "describe", "auto", "required", lock, sources)
        assert not errors, errors
        assert rows[1]["usage"], rows

    invalid = [["--unknown"], ["--inventory"], ["--device", "auto"], ["--describe", "--smoke"],
               ["--help", "--validation", "required"], ["--describe", "--device", "auto"],
               ["--smoke", "--device"], ["--smoke", "--device", "short"], ["--smoke", "--device", "A" * 32],
               ["--smoke", "--device", "0" * 33], ["--smoke", "--device", "g" * 32],
               ["--smoke", "--validation"], ["--smoke", "--validation", "unknown"],
               ["--smoke", "--device", "auto", "--device", "auto"],
               ["--smoke", "--validation", "off", "--validation", "off"],
               ["--smoke", "--validation", "off", "--device", "auto", "--extra", "bad"],
               ["--smoke", "--device", "auto", "--extra", "bad"],
               ["--unknown=quote\"slash\\\n\u6e2c\u8a66\U0001f988"]]
    for arguments in invalid:
        rows = run(arguments, 1)
        assert len(rows) == 1 and rows[0]["kind"] == "failure" and rows[0]["status"] == "FAIL", rows
        error = rows[0]["error"]
        assert error["code"] == "invalid_argument" and error["message"], error
        assert error["operation"] == "presentation-smoke.arguments", error
    print(f"PASS: {count} CPU-only presentation CLI checks; no GPU qualification")


if __name__ == "__main__":
    main()
