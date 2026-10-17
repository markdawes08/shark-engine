"""Build a fresh source-only snapshot without staging, committing or copying local inputs."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import stat
import subprocess
import sys

from process_runner import run_process

ROOT = Path(__file__).resolve().parents[1]


def source_file(root: Path, relative: str) -> Path:
    path = Path(relative)
    if path.is_absolute() or ".." in path.parts:
        raise ValueError(f"Unsafe source path: {relative}")
    candidate = root
    for part in path.parts:
        candidate = candidate / part
        attributes = candidate.lstat()
        if candidate.is_symlink() or getattr(attributes, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0):
            raise ValueError(f"Linked source path is not allowed: {relative}")
    resolved = candidate.resolve()
    if not resolved.is_relative_to(root.resolve()) or not resolved.is_file():
        raise ValueError(f"Unsafe/non-file source path: {relative}")
    return resolved


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", required=True, choices=("windows-debug", "linux-debug", "macos-debug"))
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    destination = ROOT / "out" / ("clean-source-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ"))
    destination.mkdir(parents=True, exist_ok=False)
    evidence = {"kind": "source-only snapshot, not a committed checkout", "source": str(ROOT),
                "snapshot": str(destination), "preset": args.preset, "status": "FAIL", "phases": []}
    try:
        result = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
                                cwd=ROOT, capture_output=True, check=True, timeout=30)
        paths = sorted(set(name for name in result.stdout.decode("utf-8").split("\0") if name))
        if "AI_Game_Engine_Agent_Roadmap.txt" in paths:
            raise ValueError("The local roadmap must be ignored and absent from the snapshot")
        for relative in paths:
            source = source_file(ROOT, relative)
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        evidence["source_file_count"] = len(paths)
        # Repository/ignore checks require a Git root. This creates no index
        # entries or commits, even in the disposable snapshot.
        commands = [
            ["git", "init", "-b", "main"],
            ["cmake", "--preset", args.preset, f"-DPython3_EXECUTABLE={sys.executable}"],
            ["cmake", "--build", "--preset", args.preset, "--parallel", str(args.jobs)],
            ["ctest", "--preset", args.preset, "--output-on-failure", "--no-tests=error"],
        ]
        for index, command in enumerate(commands):
            result = run_process(command, cwd=destination, timeout=1200)
            log = result.stdout + result.stderr
            if result.cleanup_errors:
                log += "\nCleanup errors: " + "; ".join(result.cleanup_errors)
            (destination / f"phase-{index}.log").write_text(log, encoding="utf-8")
            evidence["phases"].append({"command": command, "exit_code": result.returncode})
            if result.returncode or result.cleanup_errors:
                print(log, file=sys.stderr)
                return result.returncode if result.returncode > 0 else 1
        evidence["status"] = "PASS"
        print(f"PASS: source-only configure/build/tests without the local roadmap: {destination}")
        print("No files were staged or committed; this is not hosted clean-checkout evidence")
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        evidence["error"] = str(error)
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        (destination / "snapshot-result.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
