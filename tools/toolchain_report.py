"""Collect toolchain evidence without dumping environment variables or credentials."""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import re
import shutil
import sys

from process_runner import run_process

ROOT = Path(__file__).resolve().parents[1]
PINNED_PYTHON = "3.13.5"
PINNED_TOOLS = {"cmake": "3.31.10", "ninja": "1.13.2", "clang-format": "20.1.8"}
ENVIRONMENT_KEYS = (
    "ImageOS", "ImageVersion", "RUNNER_OS", "RUNNER_ARCH", "GITHUB_SHA",
    "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_WORKFLOW", "CC", "CXX",
    "VCToolsVersion", "WindowsSDKVersion", "VisualStudioVersion", "DEVELOPER_DIR",
)


def command_output(command: list[str]) -> dict:
    try:
        result = run_process(command, cwd=ROOT, timeout=30)
        return {"command": command, "exit_code": result.returncode,
                "output": (result.stdout + result.stderr).strip(),
                "timed_out": result.timed_out, "cleanup_errors": result.cleanup_errors}
    except OSError as error:
        return {"command": command, "error": str(error)}


def executable_details(name: str, arguments: list[str]) -> dict:
    executable = shutil.which(name)
    if executable is None:
        return {"available": False}
    path = Path(executable).resolve()
    details = {"available": True, "path": str(path),
               "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    details.update(command_output([str(path), *arguments]))
    return details


def collect(build_dir: Path | None = None) -> dict:
    report = {
        "schema_version": 1,
        "platform": {"system": platform.system(), "release": platform.release(),
                     "version": platform.version(), "machine": platform.machine()},
        "python": {"version": platform.python_version(), "executable": sys.executable},
        "environment": {key: os.environ[key] for key in ENVIRONMENT_KEYS if key in os.environ},
        "git_revision": command_output(["git", "rev-parse", "HEAD"]),
        "tools": {name: executable_details(name, ["--version"]) for name in PINNED_TOOLS},
        "packages": {},
    }
    for name in (*PINNED_TOOLS, "PyYAML"):
        try:
            report["packages"][name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            report["packages"][name] = None
    if platform.system() == "Darwin":
        report["xcode"] = command_output(["xcodebuild", "-version"])
        report["sdk"] = command_output(["xcrun", "--show-sdk-version"])
    if build_dir is not None:
        report["build_directory"] = str(build_dir)
        compilers = []
        for definition in sorted(build_dir.glob("CMakeFiles/*/CMakeCXXCompiler.cmake")):
            text = definition.read_text(encoding="utf-8", errors="replace")
            values = dict(re.findall(r'set\((CMAKE_CXX_[A-Z_]+) "([^"\n]*)"\)', text))
            keep = {key: value for key, value in values.items() if key in {
                "CMAKE_CXX_COMPILER", "CMAKE_CXX_COMPILER_ID", "CMAKE_CXX_COMPILER_VERSION",
                "CMAKE_CXX_COMPILER_ARCHITECTURE_ID", "CMAKE_CXX_COMPILER_FRONTEND_VARIANT",
                "CMAKE_CXX_PLATFORM_ID", "CMAKE_CXX_LIBRARY_ARCHITECTURE",
            }}
            compiler = keep.get("CMAKE_CXX_COMPILER")
            if compiler:
                keep["binary"] = executable_details(compiler, ["/Bv"] if keep.get(
                    "CMAKE_CXX_COMPILER_ID") == "MSVC" else ["--version"])
            compilers.append(keep)
        report["configured_compilers"] = compilers
        cache = build_dir / "CMakeCache.txt"
        if cache.is_file():
            # Compiler/deployment inputs only; never copy the whole cache into metadata.
            keys = {"CMAKE_GENERATOR", "CMAKE_BUILD_TYPE", "CMAKE_OSX_DEPLOYMENT_TARGET",
                    "CMAKE_OSX_ARCHITECTURES", "CMAKE_OSX_SYSROOT", "CMAKE_CXX_FLAGS",
                    "CMAKE_CXX_FLAGS_DEBUG", "CMAKE_CXX_FLAGS_RELEASE", "CMAKE_MAKE_PROGRAM"}
            report["cmake_inputs"] = dict((match[0], match[1]) for match in re.findall(
                r"^([^/#:=\n]+):[^=\n]+=(.*)$", cache.read_text(encoding="utf-8"), re.M)
                if match[0] in keys)
    return report


def pinned_errors(report: dict) -> list[str]:
    errors = []
    if report["python"]["version"] != PINNED_PYTHON:
        errors.append(f"Python {PINNED_PYTHON} required; found {report['python']['version']}")
    for name, version in PINNED_TOOLS.items():
        details = report["tools"][name]
        if (report["packages"].get(name) != version or not details.get("available")
                or details.get("exit_code") != 0
                or details.get("cleanup_errors")
                or not re.search(rf"(?<![\d.]){re.escape(version)}(?!\d|\.\d)", details.get("output", ""))):
            errors.append(f"{name} {version} must be installed and selected on PATH")
    return errors


def write_report(output: Path, build_dir: Path | None = None) -> dict:
    report = collect(build_dir)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "out/toolchain.json")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--check-pins", action="store_true")
    args = parser.parse_args()
    report = write_report(args.output, args.build_dir)
    errors = pinned_errors(report) if args.check_pins else []
    for error in errors:
        print(error, file=sys.stderr)
    print(f"Toolchain evidence: {args.output}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
