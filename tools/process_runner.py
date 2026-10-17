"""Run a noninteractive command with a deadline and owned process-tree cleanup.

Output goes to temporary files, not pipes: a descendant retaining an output handle
cannot keep communicate() or a pipe-reader thread blocked after the deadline.
Background descendants are not supported and are terminated when the command ends.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
from typing import Mapping, Sequence


@dataclass(frozen=True)
class ProcessResult:
    args: list[str]
    returncode: int
    stdout: str
    stderr: str
    timed_out: bool
    cleanup_errors: tuple[str, ...] = ()


class _WindowsJob:
    """A private job prevents descendants surviving an early launcher exit."""

    def __init__(self) -> None:
        import ctypes
        from ctypes import wintypes

        self.ctypes = ctypes
        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        self.api.CreateJobObjectW.argtypes = (wintypes.LPVOID, wintypes.LPCWSTR)
        self.api.CreateJobObjectW.restype = wintypes.HANDLE
        self.api.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        self.api.OpenProcess.restype = wintypes.HANDLE
        self.api.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
        self.api.AssignProcessToJobObject.restype = wintypes.BOOL
        self.api.TerminateJobObject.argtypes = (wintypes.HANDLE, wintypes.UINT)
        self.api.TerminateJobObject.restype = wintypes.BOOL
        self.api.CloseHandle.argtypes = (wintypes.HANDLE,)
        self.api.CloseHandle.restype = wintypes.BOOL
        self.handle = self.api.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())

    def assign(self, pid: int) -> None:
        # PROCESS_SET_QUOTA | PROCESS_TERMINATE; operate on our launched PID only.
        handle = self.api.OpenProcess(0x0100 | 0x0001, False, pid)
        if not handle:
            raise self.ctypes.WinError(self.ctypes.get_last_error())
        try:
            if not self.api.AssignProcessToJobObject(self.handle, handle):
                raise self.ctypes.WinError(self.ctypes.get_last_error())
        finally:
            self.api.CloseHandle(handle)

    def terminate(self) -> None:
        if not self.api.TerminateJobObject(self.handle, 124):
            raise self.ctypes.WinError(self.ctypes.get_last_error())

    def close(self) -> None:
        self.api.CloseHandle(self.handle)


def _kill_windows_tree(pid: int, timeout: float) -> list[str]:
    errors = []
    taskkill = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/taskkill.exe"
    try:
        killer = subprocess.Popen([str(taskkill), "/PID", str(pid), "/T", "/F"],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        try:
            killer.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            killer.kill()
            try:
                killer.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                errors.append("taskkill did not exit after termination")
    except OSError as error:
        errors.append(f"taskkill failed: {error}")
    return errors


def run_process(command: Sequence[str], *, cwd: str | Path | None = None,
                timeout: float, env: Mapping[str, str] | None = None,
                termination_timeout: float = 1.0) -> ProcessResult:
    """Capture output and return 124 on timeout, otherwise preserve the exit code.

    Cleanup adds at most three termination_timeout waits on Windows, one on Unix.
    Process creation/OS filesystem calls themselves are outside Python's timeout
    guarantees. Unix commands must not deliberately escape their private session.
    """
    if timeout <= 0 or termination_timeout <= 0:
        raise ValueError("timeout and termination_timeout must be positive")
    args = list(command)
    if not args:
        raise ValueError("command must not be empty")
    windows = os.name == "nt"
    job = _WindowsJob() if windows else None
    process = None
    timed_out = False
    cleanup_errors: list[str] = []
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        try:
            if windows:
                # The launcher waits for one input byte, so it cannot create a child
                # before assignment to the job. This avoids the Popen/Assign race.
                launch = [sys.executable, str(Path(__file__).resolve()), "--worker", json.dumps(args)]
                process = subprocess.Popen(launch, cwd=cwd, env=env, stdin=subprocess.PIPE,
                                           stdout=stdout, stderr=stderr,
                                           creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
                job.assign(process.pid)
                process.stdin.write(b"1")
                process.stdin.close()
            else:
                process = subprocess.Popen(args, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                           stdout=stdout, stderr=stderr, start_new_session=True)
            try:
                process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
        finally:
            if process is not None:
                if windows:
                    if timed_out:
                        cleanup_errors.extend(_kill_windows_tree(process.pid, termination_timeout))
                    try:
                        job.terminate()
                    except OSError as error:
                        cleanup_errors.append(f"Job termination failed: {error}")
                    # Also handles an assignment error: an empty job cannot kill
                    # the gated launcher that failed to join it.
                    if process.poll() is None:
                        process.kill()
                    if process.stdin is not None and not process.stdin.closed:
                        process.stdin.close()
                else:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    except OSError as error:
                        cleanup_errors.append(f"Process-group termination failed: {error}")
                        if process.poll() is None:
                            process.kill()
                try:
                    process.wait(timeout=termination_timeout)
                except subprocess.TimeoutExpired:
                    cleanup_errors.append("Launched process did not exit within cleanup deadline")
            if job is not None:
                job.close()
        stdout.seek(0)
        stderr.seek(0)
        def read_snapshot(stream) -> str:
            text = stream.read(os.fstat(stream.fileno()).st_size).decode("utf-8", errors="replace")
            return text.replace("\r\n", "\n").replace("\r", "\n")

        captured_stdout = read_snapshot(stdout)
        captured_stderr = read_snapshot(stderr)
    return ProcessResult(args, 124 if timed_out else process.returncode,
                         captured_stdout, captured_stderr, timed_out, tuple(cleanup_errors))


def _worker() -> int:
    # Internal Windows launcher: no shell interpolation or inherited stdin access.
    if sys.stdin.buffer.read(1) != b"1":
        return 127
    try:
        return subprocess.call(json.loads(sys.argv[2]), stdin=subprocess.DEVNULL)
    except OSError as error:
        print(error, file=sys.stderr, flush=True)
        return 127


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--worker":
        raise SystemExit("process_runner is an imported helper, not a command-line interface")
    raise SystemExit(_worker())
