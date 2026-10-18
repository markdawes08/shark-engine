"""Opt-in standard-user launcher integration checks; needs a Windows desktop.

Run from an elevated developer terminal with a same-user standard Explorer:
python tools/tests/check_windows_unelevated.py --output out/process-launch/check-1
This is deliberately separate from ordinary headless/CPU CTest jobs.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
SELF = Path(__file__).resolve()
sys.path.insert(0, str(ROOT / "tools"))
from windows_unelevated import UnavailableError, process_security, run_standard


def process_alive(pid: int) -> bool:
    import ctypes
    from ctypes import wintypes as w

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = (w.DWORD, w.BOOL, w.DWORD)
    kernel.OpenProcess.restype = w.HANDLE
    kernel.WaitForSingleObject.argtypes = (w.HANDLE, w.DWORD)
    kernel.WaitForSingleObject.restype = w.DWORD
    kernel.CloseHandle.argtypes = (w.HANDLE,)
    handle = kernel.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE, no modification.
    if not handle:
        error = ctypes.get_last_error()
        if error == 87:  # The exited process no longer exists.
            return False
        raise ctypes.WinError(error)
    try:
        state = kernel.WaitForSingleObject(handle, 1000)
        if state not in (0, 258):
            raise ctypes.WinError(ctypes.get_last_error())
        return state == 258
    finally:
        kernel.CloseHandle(handle)


def target(mode: str) -> int:
    if mode == "argv":
        print(json.dumps({"argv": sys.argv[3:], "security": process_security()}), flush=True)
        print("separate stderr marker", file=sys.stderr, flush=True)
        return 17
    if mode == "leaf":
        threading.Event().wait(60)
        return 0
    child = subprocess.Popen([sys.executable, str(SELF), "--target", "leaf"])
    value = {"target_pid": os.getpid(), "descendant_pid": child.pid, "security": process_security()}
    print(json.dumps(value), flush=True)
    if mode == "abrupt":
        path = Path(sys.argv[3])
        temporary = path.with_suffix(".tmp")
        temporary.write_text(json.dumps(value), encoding="utf-8")
        temporary.replace(path)
    if mode != "normal":
        threading.Event().wait(60)
    return 0


def check(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def exercise(output: Path, records: list[dict]) -> None:
    arguments = ["", "space argument", 'quote"argument', "backslash\\tail\\", "$literal`(value)"]
    command = [sys.executable, str(SELF), "--target", "argv", *arguments]
    result, security = run_standard(command, cwd=ROOT, timeout=10)
    actual = json.loads(result.stdout)
    check(result.returncode == 17 and result.stderr == "separate stderr marker\n",
          "Child nonzero exit or separate stderr was not preserved")
    check(actual["argv"] == arguments, "Child argv differs from the exact requested vector")
    check(actual["security"]["integrity_rid"] == 8192 and not actual["security"]["elevated"],
          "Target did not independently report standard medium integrity")
    check(not result.cleanup_errors, "Child cleanup failed")
    records.append({"case": "exact_argv_stdout_stderr_nonzero_exit", "status": "PASS",
                    "security": security, "result": asdict(result)})
    for mode, deadline in (("timeout", 2), ("normal", 10)):
        started = time.monotonic()
        result, security = run_standard([sys.executable, str(SELF), "--target", mode],
                                        cwd=ROOT, timeout=deadline)
        elapsed = time.monotonic() - started
        actual = json.loads(result.stdout)
        checked = [actual["target_pid"], actual["descendant_pid"],
                   security["bootstrap_pid"], security["worker_pid"]]
        survivors = [pid for pid in checked if process_alive(pid)]
        check(not survivors, f"Owned processes survived: {survivors}")
        check(result.returncode == (124 if mode == "timeout" else 0), "Unexpected fixture exit")
        check(result.timed_out == (mode == "timeout") and not result.cleanup_errors,
              "Fixture timeout or cleanup result was incorrect")
        check(elapsed < deadline + 5, "Command exceeded its bounded completion allowance")
        records.append({"case": mode + "_owned_descendant_cleanup", "status": "PASS",
                        "elapsed_seconds": elapsed, "checked_pids": checked, "survivors": survivors,
                        "security": security, "result": asdict(result)})

    pid_file = output / "abrupt-owned-pids.json"
    command = [sys.executable, str(ROOT / "tools/windows_unelevated.py"), "--timeout", "30", "--",
               sys.executable, str(SELF), "--target", "abrupt", str(pid_file)]
    owner = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 10
        while not pid_file.is_file() and time.monotonic() < deadline and owner.poll() is None:
            # Bounded readiness polling, not an assumed startup delay.
            threading.Event().wait(0.02)
        check(pid_file.is_file(), "Abrupt-exit fixture never became ready")
        actual = json.loads(pid_file.read_text(encoding="utf-8"))
        owner.kill()  # Only our own launcher; tests atomic job kill-on-close.
        stdout, stderr = owner.communicate(timeout=5)
        checked = [actual["target_pid"], actual["descendant_pid"]]
        survivors = [pid for pid in checked if process_alive(pid)]
        check(not survivors, f"Owned processes survived abrupt launcher exit: {survivors}")
        records.append({"case": "abrupt_launcher_exit_kill_on_close", "status": "PASS",
                        "owner_pid": owner.pid, "checked_pids": checked, "survivors": survivors,
                        "target_security": actual["security"], "stdout": stdout.decode(),
                        "stderr": stderr.decode()})
    finally:
        if owner.poll() is None:
            owner.kill()
            owner.wait(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="Fresh evidence directory")
    args = parser.parse_args()
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        print(f"Fresh evidence directory required: {error}", file=sys.stderr)
        return 2
    report = {"schema_version": 1, "status": "FAIL", "cases": [],
              "source_sha256": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in (SELF, ROOT / "tools/windows_unelevated.py",
                                             ROOT / "tools/process_runner.py")}}
    code = 1
    try:
        report["caller_security"] = process_security()
        if not report["caller_security"]["elevated"]:
            raise UnavailableError("These optional drop-token checks require an elevated caller and standard desktop")
        exercise(output, report["cases"])
        report["status"] = "PASS"
        code = 0
    except UnavailableError as error:
        report.update(status="NOT_RUN", reason=str(error))
        code = 3
    except (OSError, ValueError, KeyError, TimeoutError, subprocess.TimeoutExpired) as error:
        report["reason"] = str(error)
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "cases": len(report["cases"]),
                      "summary": str(output / "summary.json"), "reason": report.get("reason")}))
    return code


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--target":
        raise SystemExit(target(sys.argv[2]))
    raise SystemExit(main())
