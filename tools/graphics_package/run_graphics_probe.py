"""Run a verified Shark Linux graphics probe package with bounded evidence.

Use Python 3.12+ in isolated mode: python3 -I -B run_graphics_probe.py --help.
No SDK, compiler, third-party Python package or source checkout is required.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import importlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import platform
import re
import selectors
import signal
import subprocess
import sys
import time

MAX_FILE = 128 * 1024 * 1024
MAX_TOTAL = 512 * 1024 * 1024
MAX_OUTPUT = 8 * 1024 * 1024
HELPERS = {"_support/graphics_probe.py", "_support/process_runner.py", "_support/toolchain_report.py"}
SHADERS = {"fixture_vs.spv", "fixture_ps.spv", "compute_cs.spv", "sample_ps.spv"}


def unique_object(pairs: list) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_number(value: str) -> None:
    raise ValueError(f"Nonfinite JSON number: {value}")


def finite_float(value: str) -> float:
    number = float(value)
    if not math.isfinite(number):
        reject_number(value)
    return number


def no_links(path: Path) -> None:
    if any(item.is_symlink() for item in (path, *path.parents)):
        raise ValueError(f"Symbolic links are not valid package/evidence paths: {path}")


def file_hash(path: Path) -> str:
    no_links(path)
    if not path.is_file() or path.stat().st_size > MAX_FILE:
        raise ValueError(f"Missing or oversized package file: {path}")
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def load_json(path: Path) -> dict:
    no_links(path)
    if not path.is_file() or path.stat().st_size > MAX_OUTPUT:
        raise ValueError(f"Missing or oversized JSON: {path}")
    result = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object,
                        parse_constant=reject_number, parse_float=finite_float)
    if not isinstance(result, dict):
        raise ValueError("Expected a JSON object")
    return result


def relative_name(name: object) -> str:
    if not isinstance(name, str):
        raise ValueError("Package path must be a string")
    path = PurePosixPath(name)
    if (not name or "\\" in name or ":" in name or "\0" in name or path.is_absolute()
            or path.as_posix() != name or any(part in (".", "..") for part in path.parts)):
        raise ValueError(f"Unsafe package path: {name}")
    return name


def verify_package(root: Path) -> dict:
    no_links(root)
    manifest = load_json(root / "package-manifest.json")
    if (type(manifest.get("schema_version")) is not int or manifest["schema_version"] != 1
            or manifest.get("assignment") != "02d" or manifest.get("platform") != "linux-x86_64"
            or manifest.get("configuration") != "Release" or manifest.get("status") != "PASS"
            or manifest.get("qualification") != "NOT_RUN"):
        raise ValueError("Unsupported or nonpassing Linux package manifest")
    for key in ("dependency_lock_sha256", "probe_source_sha256"):
        if not isinstance(manifest.get(key), str) or not re.fullmatch(r"[0-9a-f]{64}", manifest[key]):
            raise ValueError("Package identity requires SHA-256 values")
    probe = manifest.get("probe")
    if (not isinstance(probe, dict) or type(probe.get("schema_version")) is not int or probe["schema_version"] != 2
            or probe.get("executable") != "bin/GraphicsProbe"
            or any(probe.get(key) != manifest[key] for key in ("dependency_lock_sha256", "probe_source_sha256"))):
        raise ValueError("Package probe identity is inconsistent")
    validation = manifest.get("validation")
    if (not isinstance(validation, dict)
            or validation.get("manifest") != "validation/VkLayer_khronos_validation.json"
            or validation.get("library") != "validation/libVkLayer_khronos_validation.so"):
        raise ValueError("Package validation selection is invalid")
    files = manifest.get("files")
    if not isinstance(files, dict) or not 1 <= len(files) <= 256:
        raise ValueError("Invalid package file inventory")
    required = {"bin/GraphicsProbe", "lib/libvulkan.so.1", "run_graphics_probe.py", *HELPERS,
                "validation/VkLayer_khronos_validation.json", "validation/libVkLayer_khronos_validation.so",
                "validation/vk_layer_settings.txt", "provenance/dependencies.lock.json",
                "provenance/shader-manifest.json", "provenance/validation-manifest.json", "provenance/dependency-closure.json",
                "provenance/source.json", "provenance/toolchain.json", "provenance/probe-description.json",
                "README.txt", *("shaders/" + name for name in SHADERS)}
    if not required <= set(files) or not any(name.startswith("licenses/") for name in files):
        raise ValueError("Package is missing required binaries, helpers, shaders, provenance or licenses")
    total = 0
    for name, record in files.items():
        relative_name(name)
        if (not isinstance(record, dict) or type(record.get("bytes")) is not int
                or not 0 <= record["bytes"] <= MAX_FILE or not isinstance(record.get("sha256"), str)
                or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"])):
            raise ValueError("Invalid package file identity")
        total += record["bytes"]
        if total > MAX_TOTAL:
            raise ValueError("Package byte budget exceeded")
        path = root / name
        if file_hash(path) != record["sha256"] or path.stat().st_size != record["bytes"]:
            raise ValueError(f"Package file changed: {name}")
    actual = set()
    count = 0
    for path in root.rglob("*"):
        count += 1
        if count > 512:
            raise ValueError("Package tree entry budget exceeded")
        no_links(path)
        if path.is_file():
            actual.add(path.relative_to(root).as_posix())
        elif not path.is_dir():
            raise ValueError("Package contains a nonregular entry")
    if actual != set(files) | {"package-manifest.json"}:
        raise ValueError("Unlisted or missing package files")
    implicit = root / "validation/implicit"
    if not implicit.is_dir() or any(implicit.iterdir()):
        raise ValueError("Implicit layer directory must exist and remain empty")
    layer = load_json(root / validation["manifest"]).get("layer")
    if (not isinstance(layer, dict) or layer.get("name") != "VK_LAYER_KHRONOS_validation"
            or layer.get("library_path") != "./libVkLayer_khronos_validation.so"
            or layer.get("api_version") != validation.get("api_version")):
        raise ValueError("Package layer declaration is inconsistent")
    if file_hash(root / "provenance/dependencies.lock.json") != manifest["dependency_lock_sha256"]:
        raise ValueError("Packaged lock disagrees with native identity")
    return manifest


@dataclass
class ProcessResult:
    args: list[str]
    returncode: int
    stdout: str
    stderr: str
    timed_out: bool
    cleanup_errors: tuple[str, ...] = ()


class LauncherInterrupted(Exception):
    """Distinct from InterruptedError, which selectors may retry internally."""


def run_bounded(command: list[str], *, cwd: Path, env: dict, timeout: float,
                maximum: int = MAX_OUTPUT) -> ProcessResult:
    """Drain bounded pipes; retire the owned process group on every exit path.

    Children must not deliberately escape their new session. Unlike a container,
    a process group is not a hostile-code sandbox. SIGKILL of this launcher is
    outside the guarantee; normal exits, deadlines and SIGINT/SIGTERM are handled.
    """
    if not 0 < timeout <= 1200 or not 0 < maximum <= MAX_OUTPUT:
        raise ValueError("Invalid process budget")
    process = None
    buffers = {"stdout": bytearray(), "stderr": bytearray()}
    errors = []
    timed_out = False
    deadline = time.monotonic() + timeout
    with selectors.DefaultSelector() as selector:
        try:
            process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       close_fds=True, start_new_session=True)
            for name in buffers:
                stream = getattr(process, name)
                os.set_blocking(stream.fileno(), False)
                selector.register(stream, selectors.EVENT_READ, name)
            while selector.get_map():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    timed_out = True
                    break
                if process.poll() is not None:
                    # A surviving descendant must not hold pipe EOF hostage.
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    deadline = min(deadline, time.monotonic() + 1)
                for key, _ in selector.select(min(remaining, 0.05)):
                    data = os.read(key.fd, 65536)
                    if not data:
                        selector.unregister(key.fileobj)
                        continue
                    target = buffers[key.data]
                    space = maximum - len(target)
                    target.extend(data[:space])
                    if len(data) > space:
                        errors.append(f"{key.data} exceeded its byte budget")
                        break
                if errors:
                    break
            if process.poll() is None and not errors and not timed_out:
                try:
                    process.wait(timeout=max(0.001, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    timed_out = True
        except (KeyboardInterrupt, LauncherInterrupted):
            errors.append("Launcher interrupted; owned process group terminated")
        finally:
            if process is not None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                except OSError as error:
                    errors.append(f"Process group cleanup failed: {error}")
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    errors.append("Owned leader did not terminate within cleanup budget")
                for name in buffers:
                    stream = getattr(process, name)
                    if stream:
                        stream.close()
    code = 124 if timed_out else (1 if errors else process.returncode)
    return ProcessResult(command, code, buffers["stdout"].decode("utf-8", errors="replace"),
                         buffers["stderr"].decode("utf-8", errors="replace"), timed_out, tuple(errors))


def child_environment(root: Path, output: Path) -> dict:
    # Preserve only the desktop connection, ordinary account and locale data.
    # Never inherit loader, driver/layer selection, Python or SDK overrides.
    keys = ("HOME", "USER", "LOGNAME", "DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XAUTHORITY",
            "DBUS_SESSION_BUS_ADDRESS", "LANG", "LC_ALL")
    env = {key: os.environ[key] for key in keys if key in os.environ}
    env.update(PATH="/usr/bin:/bin", LD_LIBRARY_PATH=str(root / "lib"),
               VK_LAYER_PATH=str(root / "validation"), VK_IMPLICIT_LAYER_PATH=str(root / "validation/implicit"),
               VK_LAYER_SETTINGS_PATH=str(output / "vk_layer_settings.txt"))
    return env


def bounded_int(minimum: int, maximum: int):
    def parse(value: str) -> int:
        number = int(value)
        if not minimum <= number <= maximum:
            raise argparse.ArgumentTypeError(f"Expected {minimum}..{maximum}")
        return number
    return parse


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("describe", "inventory", "smoke", "cases"), default="describe")
    parser.add_argument("--device")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--lifecycle-cycles", type=bounded_int(0, 100), default=2)
    parser.add_argument("--device-cycles", type=bounded_int(1, 20), default=1)
    parser.add_argument("--present-seconds", type=bounded_int(0, 600), default=0)
    parser.add_argument("--timeout", type=bounded_int(1, 1200), default=120)
    args = parser.parse_args()
    root = Path(__file__).absolute().parent
    output = args.output.absolute()
    report = {"schema_version": 1, "assignment": "02d", "status": "FAIL", "qualification": "NOT_RUN",
              "platform": "linux-x86_64", "started_utc": datetime.now(timezone.utc).isoformat(), "mode": args.mode}
    created = False
    code = 1
    try:
        no_links(root)
        root = root.resolve()
        if any(character in str(root) for character in (":", ";", "$")):
            raise ValueError("Package path cannot contain loader delimiters ':', ';' or '$'")
        no_links(output)
        output = output.resolve()
        if output.exists() or output.is_relative_to(root):
            raise ValueError("Evidence output must be fresh and outside the package")
        if args.mode in ("smoke", "cases") and (not args.device or not re.fullmatch(r"[0-9a-f]{32}", args.device)):
            raise ValueError("Smoke/cases require an explicit 32-character lowercase device UUID")
        if args.mode not in ("smoke", "cases") and args.device:
            raise ValueError("Device selection is only valid for smoke/cases")
        if args.mode != "cases" and (args.lifecycle_cycles, args.device_cycles, args.present_seconds) != (2, 1, 0):
            raise ValueError("Lifecycle workload flags are only valid for cases")
        manifest = verify_package(root)
        package_hash = file_hash(root / "package-manifest.json")
        output.mkdir(parents=True)
        created = True
        report.update(package_manifest_sha256=package_hash, dependency_lock_sha256=manifest["dependency_lock_sha256"],
                      probe_source_sha256=manifest["probe_source_sha256"], timeout_seconds=args.timeout,
                      host={"system": platform.system(), "release": platform.release(), "machine": platform.machine(),
                            "python": platform.python_version(), "uid": os.getuid() if hasattr(os, "getuid") else None})
        if platform.system() != "Linux" or platform.machine() not in ("x86_64", "AMD64") or sys.version_info < (3, 12):
            report.update(status="NOT_RUN", reason="Native Linux x86-64 and Python 3.12+ are required")
            code = 3
        elif args.mode != "describe" and (os.geteuid() == 0 or os.geteuid() != os.getuid() or os.getegid() != os.getgid()):
            report.update(status="NOT_RUN", reason="GPU operations require an ordinary unprivileged desktop account")
            code = 3
        else:
            # Imports happen only after every package file, including these exact
            # helpers, has passed hash/path validation. Disable bytecode writes.
            sys.dont_write_bytecode = True
            sys.path.insert(0, str(root / "_support"))
            contract = importlib.import_module("graphics_probe")
            artifacts = output / "artifacts"
            artifacts.mkdir()
            (output / "vk_layer_settings.txt").write_text(
                "# Probe enables core and synchronization validation explicitly; no local overrides.\n", encoding="utf-8")
            command = [str(root / "bin/GraphicsProbe"), "--" + args.mode]
            if args.mode in ("smoke", "cases"):
                command += ["--device", args.device, "--validation", "required"]
            if args.mode == "cases":
                command += ["--shader-dir", str(root / "shaders"), "--artifacts-dir", str(artifacts),
                            "--lifecycle-cycles", str(args.lifecycle_cycles), "--device-cycles", str(args.device_cycles),
                            "--present-seconds", str(args.present_seconds)]
            def interrupt(_signum, _frame):
                # Subsequent cancellation signals must not interrupt teardown.
                for item in (signal.SIGINT, signal.SIGTERM):
                    signal.signal(item, signal.SIG_IGN)
                raise LauncherInterrupted("Launcher signal")
            previous = {item: signal.signal(item, interrupt) for item in (signal.SIGINT, signal.SIGTERM)}
            try:
                result = run_bounded(command, cwd=output, env=child_environment(root, output), timeout=args.timeout)
            finally:
                for item, handler in previous.items():
                    signal.signal(item, handler)
            (output / "stdout.json").write_text(result.stdout, encoding="utf-8")
            (output / "stderr.log").write_text(result.stderr, encoding="utf-8")
            native, errors = contract.inspect_result(result, args.mode, manifest["dependency_lock_sha256"],
                manifest["probe_source_sha256"], args.device, "required", artifacts_dir=artifacts,
                lifecycle_cycles=args.lifecycle_cycles, device_cycles=args.device_cycles, present_seconds=args.present_seconds)
            if verify_package(root) != manifest or file_hash(root / "package-manifest.json") != package_hash:
                errors.append("Package changed during execution")
            code = 124 if result.timed_out else (1 if errors else result.returncode)
            report.update(status="PASS" if code == 0 else "NOT_RUN" if code == 3 else "FAIL",
                          command=command, exit_code=code, process_exit_code=result.returncode,
                          timed_out=result.timed_out, cleanup_errors=list(result.cleanup_errors), errors=errors, report=native)
    except (OSError, ValueError, KeyError, TypeError, UnicodeError, RecursionError, ImportError) as error:
        report.update(status="FAIL", reason=str(error))
        code = 1
    report.update(exit_code=code, finished_utc=datetime.now(timezone.utc).isoformat())
    text = json.dumps(report, indent=2, sort_keys=True, allow_nan=False) + "\n"
    if created:
        (output / "summary.json").write_text(text, encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("status", "qualification", "exit_code")} | {"summary": str(output / "summary.json") if created else None,
          "reason": report.get("reason")}, allow_nan=False))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
