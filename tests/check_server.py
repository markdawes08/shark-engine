"""Exercise the real idle Server CLI, startup failures and bounded shutdown."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from process_runner import run_process

READY = "Shark Server ready (headless idle)\n"
STOPPED = "Shark Server stopped: "


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def check_cli(executable: Path, version: str, environment: dict[str, str]) -> None:
    result = run_process([str(executable), "--version"], timeout=5, env=environment)
    require(result.returncode == 0 and not result.timed_out and not result.cleanup_errors,
            f"Version invocation failed: {result}")
    require(result.stdout == f"Shark Server {version}\n" and not result.stderr,
            f"Unexpected version output: {result}")
    result = run_process([str(executable), "--help"], timeout=5, env=environment)
    require(result.returncode == 0 and not result.timed_out and not result.cleanup_errors,
            f"Help invocation failed: {result}")
    require(result.stdout.startswith("Usage: Server [--help | --version | --run-for-ms N]\n")
            and "networking and simulation are not implemented" in result.stdout
            and "1..600000" in result.stdout and READY not in result.stdout and not result.stderr,
            f"Unexpected help output: {result}")
    print("PASS: informational options exit successfully without starting Server")


def check_invalid_configuration(executable: Path, environment: dict[str, str]) -> None:
    invalid = [
        [""], ["--unknown"], ["--run-for-ms"], ["--run-for-ms=1"],
        ["--help", "--help"], ["--help", "--version"],
        ["--version", "--run-for-ms", "1"], ["--run-for-ms", "1", "--help"],
        ["--run-for-ms", "1", "--run-for-ms", "2"],
    ]
    invalid += [["--run-for-ms", text] for text in (
        "", "0", "600001", "1000000", "0000001", "18446744073709551616",
        "-1", "+1", " 1", "1 ", "1.0", "1e2", "0x10", "1ms", "9" * 1024,
    )]
    for arguments in invalid:
        result = run_process([str(executable), *arguments], timeout=5, env=environment)
        require(result.returncode == 2 and not result.timed_out and not result.cleanup_errors,
                f"Invalid arguments did not exit 2: {arguments!r}: {result}")
        require(not result.stdout and result.stderr.startswith("Shark Server: invalid configuration:")
                and "Usage: Server" in result.stderr,
                f"Invalid arguments started Server or omitted diagnostics: {arguments!r}: {result}")
    print(f"PASS: {len(invalid)} invalid configurations reject before readiness")


def check_bounded_runs(executable: Path, environment: dict[str, str]) -> None:
    # Repeat the actual process lifecycle. The 200 ms run also detects a parser or
    # lifecycle regression that immediately exits instead of honoring duration.
    for duration in ("1", "000001", "20", "200", "200", "200"):
        started = time.monotonic()
        result = run_process([str(executable), "--run-for-ms", duration], timeout=5,
                             env=environment)
        elapsed = time.monotonic() - started
        require(result.returncode == 0 and not result.timed_out and not result.cleanup_errors,
                f"Bounded lifetime failed: {duration}: {result}")
        require(result.stdout == READY + STOPPED + "run duration elapsed\n" and not result.stderr,
                f"Bounded lifetime output mismatch: {duration}: {result}")
        require(elapsed >= int(duration) / 1000 - 0.005 and elapsed < 5,
                f"Duration not honored within process deadline: {duration}: {elapsed:.3f}s")
    print("PASS: six no-display bounded lifetimes start and stop cleanly")


def cleanup_posix_process(process: subprocess.Popen) -> None:
    # These children own private sessions. Clean the entire group even if a
    # regression starts descendants and the initial process has already exited.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=1)


def check_signal_shutdown(executable: Path, environment: dict[str, str], signum: int,
                          arguments: list[str]) -> None:
    with tempfile.TemporaryDirectory(prefix="shark-server-signal-") as temporary:
        stdout_path = Path(temporary) / "stdout.txt"
        stderr_path = Path(temporary) / "stderr.txt"
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            process = subprocess.Popen([str(executable), *arguments], stdin=subprocess.DEVNULL,
                                       stdout=stdout, stderr=stderr, env=environment,
                                       start_new_session=True)
            try:
                ready_deadline = time.monotonic() + 5
                # Read separate file handles so polling cannot change the child's
                # output offset. Readiness is observed, never inferred from sleep.
                while stdout_path.read_text(encoding="utf-8") != READY:
                    require(process.poll() is None,
                            f"Server exited before readiness: {process.returncode}; "
                            f"{stderr_path.read_text(encoding='utf-8')}")
                    require(time.monotonic() < ready_deadline, "Server readiness deadline expired")
                    time.sleep(0.01)
                require(process.poll() is None, "Server did not retain its idle lifetime")
                started = time.monotonic()
                process.send_signal(signum)
                code = process.wait(timeout=2)
                elapsed = time.monotonic() - started
                name = signal.Signals(signum).name
                require(code == 0 and elapsed < 2,
                        f"{name} did not complete graceful shutdown: exit {code}, {elapsed:.3f}s")
                require(stdout_path.read_text(encoding="utf-8") == READY + STOPPED + name + "\n"
                        and not stderr_path.read_text(encoding="utf-8"),
                        f"{name} shutdown did not report the expected reason")
            finally:
                cleanup_posix_process(process)
    print(f"PASS: {signal.Signals(signum).name} stops a ready Server within two seconds")


def check_failed_readiness(executable: Path, environment: dict[str, str]) -> None:
    # A read-only stdout descriptor is a real startup I/O failure after handler
    # installation. It avoids adding a test-only failure switch to the product.
    with tempfile.TemporaryDirectory(prefix="shark-server-startup-") as temporary:
        output_path = Path(temporary) / "read-only.txt"
        output_path.write_bytes(b"")
        error_path = Path(temporary) / "stderr.txt"
        with output_path.open("rb") as invalid_stdout, error_path.open("wb") as stderr:
            process = subprocess.Popen([str(executable)], stdin=subprocess.DEVNULL,
                                       stdout=invalid_stdout, stderr=stderr, env=environment,
                                       start_new_session=True)
            try:
                code = process.wait(timeout=2)
                require(code == 1, f"Readiness failure must exit 1, got {code}")
                require(error_path.read_text(encoding="utf-8") ==
                        "Shark Server: startup failed: cannot report readiness\n",
                        "Startup I/O failure lacks its diagnostic")
                require(output_path.read_bytes() == b"", "Read-only output was unexpectedly written")
            finally:
                cleanup_posix_process(process)
    print("PASS: readiness I/O failure unwinds startup and exits 1")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--expected-version", required=True)
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    environment = dict(os.environ)
    for name in ("DISPLAY", "WAYLAND_DISPLAY", "MIR_SOCKET"):
        environment.pop(name, None)
    try:
        check_cli(executable, args.expected_version, environment)
        check_invalid_configuration(executable, environment)
        check_bounded_runs(executable, environment)
        if os.name == "posix":
            check_signal_shutdown(executable, environment, signal.SIGINT, [])
            check_signal_shutdown(executable, environment, signal.SIGTERM, [])
            check_signal_shutdown(executable, environment, signal.SIGTERM,
                                  ["--run-for-ms", "600000"])
            check_failed_readiness(executable, environment)
        else:
            print("NOT RUN: POSIX signal and read-only descriptor checks on this host; "
                  "Windows coverage is finite-run shutdown, not terminate() signal emulation")
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
