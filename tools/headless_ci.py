"""Build and qualify M01 Server/Core inside the locked Ubuntu 24.04 environment.

Local Docker entry: bash tools/ci/headless/run.sh [out/ci/headless]
An isolated chroot prepared by tools/ci/headless/setup.sh can run this directly.
This command never installs packages or changes the host system.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import sys
import tarfile
import time

from entt_dependency import prepare as prepare_entt
from process_runner import run_process
from toolchain_report import PINNED_TOOLS, write_report

ROOT = Path(__file__).resolve().parents[1]
INPUTS = ROOT / "tools/ci/headless/inputs.json"
PROVENANCE = Path("/opt/shark-headless/provenance")
PRESETS = ("server-only", "server-debug", "server-asan-ubsan")
# Fail both the known presentation families and any drift from the exact reviewed
# package inventory. libxxhash is a compression/hash dependency, not X11.
FORBIDDEN_PACKAGE = re.compile(
    r"^(?:x11-|xserver|xorg|wayland|vulkan|mesa|glslang|shaderc|spirv|"
    r"alsa-|pulseaudio|pipewire|jackd|assimp|lib(?:x11|xau|xcb|xdmcp|xext|"
    r"xi[0-9-]|xrandr|xrender|xfixes|xcursor|wayland|vulkan|glfw|sdl|"
    r"gl[0-9x-]|egl|gles|opengl|drm|gbm|asound|pulse|pipewire|jack|openal|"
    r"portaudio|sndfile|vorbis|ogg|flac|mpg123|assimp|png|jpeg|webp|tiff|"
    r"freeimage|freetype|shaderc|spirv))", re.IGNORECASE)


def package_errors(actual: str, expected: str) -> list[str]:
    actual_lines = sorted(line for line in actual.splitlines() if line.strip())
    expected_lines = sorted(line for line in expected.splitlines() if line.strip())
    errors = []
    if actual_lines != expected_lines:
        added = sorted(set(actual_lines) - set(expected_lines))
        removed = sorted(set(expected_lines) - set(actual_lines))
        errors.append(f"Installed package lock mismatch: added/changed={added}; missing/changed={removed}")
    for line in actual_lines:
        name = line.split("\t")[0].split(":")[0]
        if FORBIDDEN_PACKAGE.match(name):
            errors.append(f"Presentation/import/shader package is installed: {name}")
    return errors


def phase(name: str, command: list[str], output: Path, timeout: int) -> dict:
    print(f"[{name}] {' '.join(command)}", flush=True)
    started = time.monotonic()
    try:
        result = run_process(command, cwd=ROOT, timeout=timeout)
        code = result.returncode
        log = result.stdout + result.stderr
        if result.timed_out:
            log += f"\nTIMEOUT after {timeout} seconds\n"
        if result.cleanup_errors:
            log += "\n" + "\n".join(result.cleanup_errors) + "\n"
            code = code or 125
    except OSError as error:
        code, log = 127, str(error) + "\n"
    (output / f"{name}.log").write_text(log, encoding="utf-8")
    print(log, end="" if log.endswith("\n") else "\n", flush=True)
    return {"name": name, "command": command, "exit_code": code,
            "elapsed_seconds": round(time.monotonic() - started, 3),
            "status": "PASS" if code == 0 else "FAIL"}


def environment_errors(output: Path) -> list[str]:
    errors = []
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        return ["The headless gate requires Linux x86_64 inside the pinned Ubuntu image/chroot"]
    os_release = platform.freedesktop_os_release()
    if os_release.get("ID") != "ubuntu" or os_release.get("VERSION_ID") != "24.04":
        errors.append(f"Ubuntu 24.04 required, found {os_release.get('PRETTY_NAME')}")
    if not PROVENANCE.is_dir():
        return errors + ["Missing locked image provenance; run tools/ci/headless/setup.sh only inside its disposable image/chroot"]
    if json.loads((PROVENANCE / "inputs.json").read_text()) != json.loads(INPUTS.read_text()):
        errors.append("Image input lock differs from the source lock; rebuild the image")
    result = run_process(["dpkg-query", "-W", "-f=${binary:Package}\t${Version}\t${Architecture}\n"],
                         timeout=30)
    if result.returncode or result.cleanup_errors:
        errors.append("Could not audit installed system packages")
    else:
        (output / "installed-packages.tsv").write_text(result.stdout, encoding="utf-8")
        errors.extend(package_errors(result.stdout, (ROOT / "tools/ci/headless/packages.tsv").read_text()))
    shutil.copytree(PROVENANCE, output / "provenance", dirs_exist_ok=True)
    with tarfile.open(output / "provenance/python-licenses.tar.gz", "w:gz") as archive:
        for path in sorted(Path(sys.prefix).rglob("*")):
            if path.is_file() and any(word in path.name.lower() for word in ("license", "copyright", "notice")):
                archive.add(path, arcname=path.relative_to(sys.prefix))
    report = write_report(output / "toolchain.json")
    report["qualification_profile"] = "M01 minimal Ubuntu 24.04 image; Python 3.12.3 snapshot exception"
    cpu_models = sorted(set(re.findall(r"^model name\s*:\s*(.+)$", Path("/proc/cpuinfo").read_text(), re.M)))
    memory = dict(re.findall(r"^(MemTotal|MemAvailable):\s*(.+)$", Path("/proc/meminfo").read_text(), re.M))
    report["hardware"] = {"cpu_models": cpu_models, "logical_cpus": os.cpu_count(),
                          "visible_memory": memory,
                          "scope": "CPU/memory visible to container or WSL; shared host kernel, not bare-metal qualification"}
    worktree = run_process(["git", "status", "--porcelain", "--untracked-files=all"], cwd=ROOT, timeout=30)
    report["source_worktree"] = {"exit_code": worktree.returncode, "status": worktree.stdout,
                                 "scope": "Uncommitted local checks describe the worktree, not only git_revision"}
    (output / "toolchain.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if report["python"]["version"] != "3.12.3":
        errors.append("Locked headless profile requires Ubuntu snapshot Python 3.12.3")
    for name, version in PINNED_TOOLS.items():
        details = report["tools"][name]
        if (report["packages"].get(name) != version or details.get("exit_code") != 0
                or details.get("cleanup_errors") or not re.search(
                    rf"(?<![\d.]){re.escape(version)}(?!\d|\.\d)", details.get("output", ""))):
            errors.append(f"Locked {name} {version} must be selected on PATH")
    if report["packages"].get("PyYAML") != "6.0.3":
        errors.append("Locked PyYAML 6.0.3 required")
    for path in ("/dev/dri", "/dev/snd", "/tmp/.X11-unix"):
        if Path(path).exists():
            errors.append(f"Headless environment must not expose {path}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "out/ci/headless")
    parser.add_argument("--build-root", type=Path, default=ROOT / "out/ci/headless/build")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=1200)
    parser.add_argument("--inject-test-failure", action="store_true")
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout < 1:
        parser.error("--jobs and --timeout must be positive")
    output, build_root = args.output.resolve(), args.build_root.resolve()
    reserved = ("summary.json", "toolchain.json", "installed-packages.tsv", "provenance",
                "clean-runtime.json", *PRESETS)
    if any((output / name).exists() for name in reserved):
        parser.error("--output contains prior evidence; choose a fresh output directory")
    output.mkdir(parents=True, exist_ok=True)
    summary = {"schema_version": 1, "status": "FAIL", "phases": [],
               "started_utc": datetime.now(timezone.utc).isoformat(),
               "injected_failure": args.inject_test_failure,
               "input_lock_sha256": hashlib.sha256(INPUTS.read_bytes()).hexdigest(),
               "limits": "Headless lifecycle/configuration only; no graphics, networking, simulation or MMO capacity qualification"}
    code = 1
    try:
        # The read-only source bind may belong to another UID. Trust only this
        # explicit directory, through process-local config, never global '*'.
        os.environ.update(GIT_CONFIG_COUNT="1", GIT_CONFIG_KEY_0="safe.directory",
                          GIT_CONFIG_VALUE_0=str(ROOT), PYTHONDONTWRITEBYTECODE="1",
                          ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
                          UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
        for name in ("DISPLAY", "WAYLAND_DISPLAY", "MIR_SOCKET", "PULSE_SERVER"):
            os.environ.pop(name, None)
        errors = environment_errors(output)
        if errors:
            summary["errors"] = errors
            print("\n".join(errors), file=sys.stderr)
            return 1
        entt_archive = prepare_entt(ROOT, build_root / "dependency-cache")
        for preset in PRESETS:
            build = build_root / preset
            evidence = output / preset
            evidence.mkdir(parents=True, exist_ok=True)
            if (build / "CMakeCache.txt").exists():
                raise RuntimeError(f"Use a fresh --build-root; existing cache: {build}")
            query = build / ".cmake/api/v1/query"
            query.mkdir(parents=True, exist_ok=True)
            for name in ("codemodel-v2", "cache-v2"):
                (query / name).touch()
            compiler = "clang++-18" if preset == "server-asan-ubsan" else "g++-13"
            os.environ["CXX"] = compiler
            os.environ["CC"] = "clang-18" if preset == "server-asan-ubsan" else "gcc-13"
            commands = [
                ("configure", ["cmake", "--preset", preset, "-B", str(build),
                               f"-DCMAKE_CXX_COMPILER={compiler}",
                               f"-DPython3_EXECUTABLE={sys.executable}",
                               f"-DSHARK_ENTT_ARCHIVE={entt_archive}",
                               "-DSHARK_INJECT_TEST_FAILURE=" + (
                                   "ON" if args.inject_test_failure and preset != "server-only" else "OFF")]),
                ("build", ["cmake", "--build", str(build), "--parallel", str(args.jobs)]),
                ("boundary", ["cmake", "--build", str(build), "--target", "shark_check_server_boundary"]),
            ]
            if preset == "server-only":
                package = evidence / "package"
                if package.exists():
                    raise RuntimeError(f"Use a fresh --output; existing package: {package}")
                commands.extend([
                    ("install", ["cmake", "--install", str(build), "--prefix", str(package), "--component", "Server"]),
                    ("closure", [sys.executable, str(ROOT / "tools/check_server_closure.py"),
                                 "--build-dir", str(build), "--package-dir", str(package),
                                 "--output", str(evidence / "closure.json")]),
                    ("package-smoke", [str(package / "bin/Server"), "--run-for-ms", "20"]),
                ])
            else:
                commands.append(("test", ["ctest", "--test-dir", str(build), "--output-on-failure",
                                           "--no-tests=error", "--output-junit", str(evidence / "ctest.xml")]))
            for name, command in commands:
                record = phase(name, command, evidence, args.timeout)
                record["preset"] = preset
                summary["phases"].append(record)
                code = record["exit_code"]
                if name == "configure":
                    write_report(evidence / "toolchain.json", build)
                if code:
                    return code if code > 0 else 1
        summary["status"] = "PASS"
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        summary["errors"] = [str(error)]
        print(error, file=sys.stderr)
        code = 1
        return 1
    finally:
        summary["exit_code"] = code
        summary["finished_utc"] = datetime.now(timezone.utc).isoformat()
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
