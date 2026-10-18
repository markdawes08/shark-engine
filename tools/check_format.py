"""Check or apply pinned clang-format to engine-owned C/C++ source files."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

from toolchain_report import ROOT, executable_details

SOURCE_DIRECTORIES = ("engine", "engine_tools", "editor", "runtime", "server", "cli", "tests", "probes")
SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fix", action="store_true")
    args = parser.parse_args()
    details = executable_details("clang-format", ["--version"])
    if details.get("exit_code") != 0 or "version 20.1.8" not in details.get("output", ""):
        print("Select locked clang-format 20.1.8 on PATH first.", file=sys.stderr)
        return 2
    files = sorted(path for directory in SOURCE_DIRECTORIES for path in (ROOT / directory).rglob("*")
                   if path.is_file() and path.suffix in SUFFIXES)
    if not files:
        print("No engine-owned source files found; formatting gate cannot pass.", file=sys.stderr)
        return 2
    if not (ROOT / ".clang-format").is_file():
        print("Missing repository .clang-format", file=sys.stderr)
        return 2
    for path in files:
        flags = ["-i"] if args.fix else ["--dry-run", "--Werror"]
        try:
            result = subprocess.run([details["path"], "--style=file", *flags, str(path)],
                                    cwd=ROOT, timeout=30, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            print(error, file=sys.stderr)
            return 2
        if result.returncode:
            return result.returncode
    print(f"Formatting {'applied to' if args.fix else 'passed for'} {len(files)} files.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
