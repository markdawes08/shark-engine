"""Run the explicit Windows GLFW smoke with bounded process cleanup and evidence.

This briefly opens owned native windows. It does not render, change display
settings, send system-wide input or qualify physical keyboard/mouse hardware.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import sys

from process_runner import run_process

CASES = {
    "startup-owner-and-title", "resize-and-dpi", "native-input-callbacks",
    "focus-release-and-regain", "minimize-and-restore", "bounded-callback-diagnostics",
    "native-close-and-idempotent-cleanup", "repeated-native-lifetimes",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    executable = arguments.executable.resolve()
    output = arguments.output.resolve()
    if sys.platform != "win32" or not executable.is_file():
        parser.error("Requires Windows and the built WindowSmoke executable")
    if output.exists():
        parser.error("Output directory must be fresh; retain prior evidence")
    output.mkdir(parents=True)
    summary = {
        "schema_version": 1, "status": "FAIL",
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(), "command": [str(executable)],
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "scope": "Current Windows PC native GLFW lifecycle and targeted synthetic Win32 callbacks; no rendering or physical input claim",
    }
    try:
        result = run_process([str(executable)], cwd=executable.parent, timeout=60)
        (output / "stdout.jsonl").write_text(result.stdout, encoding="utf-8")
        (output / "stderr.log").write_text(result.stderr, encoding="utf-8")
        summary.update(exit_code=result.returncode, timed_out=result.timed_out,
                       cleanup_errors=list(result.cleanup_errors))
        records = [json.loads(line) for line in result.stdout.splitlines()]
        summary["records"] = records
        case_rows = [row for row in records if "case" in row]
        passed = (result.returncode == 0 and not result.timed_out and not result.cleanup_errors
                  and len(case_rows) == len(CASES) and {row["case"] for row in case_rows} == CASES
                  and all(row.get("status") == "PASS" for row in records)
                  and records[-1].get("cases") == len(CASES))
        summary["status"] = "PASS" if passed else "FAIL"
    except Exception as error:
        summary["error"] = str(error)
    finally:
        summary["finished_utc"] = datetime.now(timezone.utc).isoformat()
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
