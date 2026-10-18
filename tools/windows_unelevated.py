"""Run a bounded development command with the same user's standard desktop token.

Example: python tools/windows_unelevated.py --timeout 90 -- python tools/graphics_probe.py ...
This never prompts, uses credentials, changes a token/ACL/registry, or falls back
to an elevated child. It is needed because the Vulkan loader intentionally
ignores development layer search paths in elevated processes.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import asdict
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import uuid

from process_runner import ProcessResult, _WindowsJob, run_process

ROOT = Path(__file__).resolve().parents[1]
MAX_RESULT_BYTES = 64 * 1024 * 1024


class UnavailableError(RuntimeError):
    """A genuine standard-user process could not be established."""


def _api():
    if os.name != "nt":
        raise UnavailableError("This UAC helper is Windows-only; run directly on this host")
    import ctypes
    from ctypes import wintypes as w

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    security = ctypes.WinDLL("advapi32", use_last_error=True)
    kernel.GetCurrentProcess.restype = w.HANDLE
    kernel.CloseHandle.argtypes = (w.HANDLE,)
    kernel.CloseHandle.restype = w.BOOL
    security.OpenProcessToken.argtypes = (w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE))
    security.OpenProcessToken.restype = w.BOOL
    security.GetTokenInformation.argtypes = (w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD, ctypes.POINTER(w.DWORD))
    security.GetTokenInformation.restype = w.BOOL
    security.GetSidSubAuthorityCount.argtypes = (w.LPVOID,)
    security.GetSidSubAuthorityCount.restype = ctypes.POINTER(ctypes.c_ubyte)
    security.GetSidSubAuthority.argtypes = (w.LPVOID, w.DWORD)
    security.GetSidSubAuthority.restype = ctypes.POINTER(w.DWORD)
    security.EqualSid.argtypes = (w.LPVOID, w.LPVOID)
    security.EqualSid.restype = w.BOOL
    security.DuplicateTokenEx.argtypes = (w.HANDLE, w.DWORD, w.LPVOID, ctypes.c_int, ctypes.c_int,
                                         ctypes.POINTER(w.HANDLE))
    security.DuplicateTokenEx.restype = w.BOOL
    return ctypes, w, kernel, security


def _current_token(api):
    ctypes, w, kernel, security = api
    token = w.HANDLE()
    if not security.OpenProcessToken(kernel.GetCurrentProcess(), 0x0008, ctypes.byref(token)):  # TOKEN_QUERY
        raise ctypes.WinError(ctypes.get_last_error())
    return token


def _token_buffer(api, token, information_class: int):
    ctypes, w, _, security = api
    needed = w.DWORD()
    security.GetTokenInformation(token, information_class, None, 0, ctypes.byref(needed))
    if not needed.value or needed.value > 1024 * 1024:
        raise ctypes.WinError(ctypes.get_last_error())
    buffer = ctypes.create_string_buffer(needed.value)
    if not security.GetTokenInformation(token, information_class, buffer, needed, ctypes.byref(needed)):
        raise ctypes.WinError(ctypes.get_last_error())
    return buffer


def _token_security(api, token) -> dict:
    ctypes, w, _, security = api
    integer = lambda kind: ctypes.cast(_token_buffer(api, token, kind), ctypes.POINTER(w.DWORD)).contents.value
    integrity = _token_buffer(api, token, 25)  # TOKEN_MANDATORY_LABEL begins with SID_AND_ATTRIBUTES.
    sid = ctypes.cast(integrity, ctypes.POINTER(w.LPVOID)).contents.value
    count = security.GetSidSubAuthorityCount(sid).contents.value
    rid = security.GetSidSubAuthority(sid, count - 1).contents.value
    return {"elevated": bool(integer(20)), "elevation_type": integer(18),
            "integrity_rid": rid,
            "integrity_level": "low" if rid < 0x2000 else "medium" if rid < 0x3000 else "high",
            "token_type": integer(8)}


def process_security() -> dict:
    """Describe this process's actual Windows token without returning identity/secrets."""
    api = _api()
    token = _current_token(api)
    try:
        return _token_security(api, token)
    finally:
        api[2].CloseHandle(token)


def _assert_standard(security: dict) -> None:
    if (security["elevated"] or security["integrity_rid"] < 0x2000
            or security["integrity_rid"] >= 0x3000 or security["token_type"] != 1):
        raise UnavailableError("A non-elevated medium-integrity primary token is required")


@contextmanager
def _request_directory(root: Path):
    # Python 3.13 TemporaryDirectory requests mode 0700 on Windows. From an
    # elevated process that grants only Administrators/owner access and denies
    # the same user's limited token. Inherit the existing workspace permissions
    # instead; requests contain argv, cwd and deadline, never credentials.
    root = root.resolve()
    directory = root / ("uac-" + uuid.uuid4().hex)
    directory.mkdir()
    try:
        yield directory
    finally:
        if directory.resolve().parent != root:
            raise ValueError("Request directory escaped its owned root")
        shutil.rmtree(directory)


def _launch_linked_worker(request: Path, result: Path, *, cwd: Path, timeout: float) -> dict:
    api = _api()
    ctypes, w, kernel, security = api

    class StartupInfo(ctypes.Structure):
        _fields_ = [("cb", w.DWORD), ("lpReserved", w.LPWSTR), ("lpDesktop", w.LPWSTR),
                    ("lpTitle", w.LPWSTR), ("dwX", w.DWORD), ("dwY", w.DWORD),
                    ("dwXSize", w.DWORD), ("dwYSize", w.DWORD), ("dwXCountChars", w.DWORD),
                    ("dwYCountChars", w.DWORD), ("dwFillAttribute", w.DWORD),
                    ("dwFlags", w.DWORD), ("wShowWindow", w.WORD), ("cbReserved2", w.WORD),
                    ("lpReserved2", ctypes.POINTER(ctypes.c_ubyte)), ("hStdInput", w.HANDLE),
                    ("hStdOutput", w.HANDLE), ("hStdError", w.HANDLE)]

    class ProcessInfo(ctypes.Structure):
        _fields_ = [("hProcess", w.HANDLE), ("hThread", w.HANDLE),
                    ("dwProcessId", w.DWORD), ("dwThreadId", w.DWORD)]

    class StartupInfoEx(ctypes.Structure):
        _fields_ = [("StartupInfo", StartupInfo), ("lpAttributeList", w.LPVOID)]

    class BasicLimits(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong),
                    ("LimitFlags", w.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", w.DWORD),
                    ("Affinity", ctypes.c_size_t), ("PriorityClass", w.DWORD), ("SchedulingClass", w.DWORD)]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", BasicLimits), ("IoInfo", ctypes.c_ulonglong * 6),
                    ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]

    kernel.SetInformationJobObject.argtypes = (w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD)
    kernel.SetInformationJobObject.restype = w.BOOL
    kernel.InitializeProcThreadAttributeList.argtypes = (w.LPVOID, w.DWORD, w.DWORD, ctypes.POINTER(ctypes.c_size_t))
    kernel.InitializeProcThreadAttributeList.restype = w.BOOL
    kernel.UpdateProcThreadAttribute.argtypes = (w.LPVOID, w.DWORD, ctypes.c_size_t, w.LPVOID,
                                                ctypes.c_size_t, w.LPVOID, w.LPVOID)
    kernel.UpdateProcThreadAttribute.restype = w.BOOL
    kernel.DeleteProcThreadAttributeList.argtypes = (w.LPVOID,)
    kernel.CreateProcessW.argtypes = (w.LPCWSTR, w.LPWSTR, w.LPVOID, w.LPVOID, w.BOOL,
                                      w.DWORD, w.LPVOID, w.LPCWSTR, w.LPVOID,
                                      ctypes.POINTER(ProcessInfo))
    kernel.CreateProcessW.restype = w.BOOL
    kernel.ResumeThread.argtypes = (w.HANDLE,)
    kernel.ResumeThread.restype = w.DWORD
    kernel.WaitForSingleObject.argtypes = (w.HANDLE, w.DWORD)
    kernel.WaitForSingleObject.restype = w.DWORD
    kernel.GetExitCodeProcess.argtypes = (w.HANDLE, ctypes.POINTER(w.DWORD))
    kernel.GetExitCodeProcess.restype = w.BOOL
    kernel.TerminateProcess.argtypes = (w.HANDLE, w.UINT)
    kernel.TerminateProcess.restype = w.BOOL

    token = _current_token(api)
    linked = None
    linked_original = None
    shell_process = None
    attributes = None
    info = ProcessInfo()
    job = _WindowsJob()
    try:
        limits = ExtendedLimits()
        limits.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not kernel.SetInformationJobObject(job.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            raise ctypes.WinError(ctypes.get_last_error())
        caller = _token_security(api, token)
        try:
            # TokenLinkedToken can be identification-only and cannot launch a
            # process. Use the actual standard desktop primary token instead,
            # and require the exact same user/session before any launch.
            user = ctypes.WinDLL("user32", use_last_error=True)
            user.GetShellWindow.restype = w.HWND
            user.GetWindowThreadProcessId.argtypes = (w.HWND, ctypes.POINTER(w.DWORD))
            user.GetWindowThreadProcessId.restype = w.DWORD
            shell_pid = w.DWORD()
            window = user.GetShellWindow()
            if not window or not user.GetWindowThreadProcessId(window, ctypes.byref(shell_pid)):
                raise UnavailableError("No standard desktop shell; run directly from a standard terminal")
            kernel.OpenProcess.argtypes = (w.DWORD, w.BOOL, w.DWORD)
            kernel.OpenProcess.restype = w.HANDLE
            shell_process = kernel.OpenProcess(0x1080, False, shell_pid)  # QUERY_LIMITED_INFORMATION | CREATE_PROCESS
            if not shell_process:
                raise ctypes.WinError(ctypes.get_last_error())
            desktop_token = w.HANDLE()
            if not security.OpenProcessToken(shell_process, 0x000A, ctypes.byref(desktop_token)):
                raise ctypes.WinError(ctypes.get_last_error())
            linked_original = desktop_token.value
            primary = w.HANDLE()
            if not security.DuplicateTokenEx(linked_original, 0x000B, None, 2, 1, ctypes.byref(primary)):
                raise ctypes.WinError(ctypes.get_last_error())
            linked = primary.value
        except OSError as error:
            raise UnavailableError(f"Standard desktop token unavailable: {error}") from error
        expected = _token_security(api, linked)
        _assert_standard(expected)
        if expected["elevation_type"] != 3:
            raise UnavailableError("The desktop token is not a UAC limited token")
        caller_user = _token_buffer(api, token, 1)
        linked_user = _token_buffer(api, linked, 1)
        caller_sid = ctypes.cast(caller_user, ctypes.POINTER(w.LPVOID)).contents.value
        linked_sid = ctypes.cast(linked_user, ctypes.POINTER(w.LPVOID)).contents.value
        if not security.EqualSid(caller_sid, linked_sid):
            raise UnavailableError("Desktop token belongs to a different user")
        caller_session = ctypes.cast(_token_buffer(api, token, 12), ctypes.POINTER(w.DWORD)).contents.value
        child_session = ctypes.cast(_token_buffer(api, linked, 12), ctypes.POINTER(w.DWORD)).contents.value
        if caller_session != child_session:
            raise UnavailableError("Desktop token belongs to a different session")

        command = [sys.executable, str(Path(__file__).resolve()), "--bootstrap", str(request), str(result)]
        command_line = subprocess.list2cmdline(command)
        if len(command_line) >= 1024:
            raise ValueError("UAC worker command exceeds the documented Windows API limit")
        environment = ctypes.create_unicode_buffer("\0".join(
            f"{name}={value}" for name, value in sorted(os.environ.items(), key=lambda item: item[0].casefold())) + "\0\0")
        needed = ctypes.c_size_t()
        kernel.InitializeProcThreadAttributeList(None, 2, 0, ctypes.byref(needed))
        attributes_buffer = ctypes.create_string_buffer(needed.value)
        if not kernel.InitializeProcThreadAttributeList(attributes_buffer, 2, 0, ctypes.byref(needed)):
            raise ctypes.WinError(ctypes.get_last_error())
        attributes = attributes_buffer
        parent_handle = w.HANDLE(shell_process)
        job_handles = (w.HANDLE * 1)(job.handle)
        if not kernel.UpdateProcThreadAttribute(attributes, 0, 0x00020000, ctypes.byref(parent_handle),
                                                ctypes.sizeof(parent_handle), None, None):
            raise ctypes.WinError(ctypes.get_last_error())
        # Associate the owned kill-on-close job atomically with process creation.
        # Even an abrupt launcher exit cannot leave a suspended or running child.
        if not kernel.UpdateProcThreadAttribute(attributes, 0, 0x0002000D, job_handles,
                                                ctypes.sizeof(job_handles), None, None):
            raise ctypes.WinError(ctypes.get_last_error())
        startup = StartupInfoEx()
        startup.StartupInfo.cb = ctypes.sizeof(startup)
        startup.StartupInfo.dwFlags = 1  # STARTF_USESHOWWINDOW
        startup.StartupInfo.wShowWindow = 0  # SW_HIDE; never create an interactive helper window.
        startup.lpAttributeList = ctypes.cast(attributes, w.LPVOID)
        # CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT.
        # Explicit application path and Windows argv quoting; no command shell.
        if not kernel.CreateProcessW(
                sys.executable, ctypes.create_unicode_buffer(command_line), None, None, False,
                0x00000004 | 0x00000400 | 0x08000000 | 0x00080000, environment, str(cwd),
                ctypes.byref(startup), ctypes.byref(info)):
            raise UnavailableError(f"Cannot start the standard-user process: {ctypes.WinError(ctypes.get_last_error())}")
        # The child cannot execute or spawn descendants before job assignment.
        if kernel.ResumeThread(info.hThread) == 0xFFFFFFFF:
            raise ctypes.WinError(ctypes.get_last_error())
        state = kernel.WaitForSingleObject(info.hProcess, int((timeout + 10) * 1000))
        if state == 0x102:
            raise TimeoutError("Standard-user worker exceeded its outer deadline")
        if state != 0:
            raise ctypes.WinError(ctypes.get_last_error())
        exit_code = w.DWORD()
        if not kernel.GetExitCodeProcess(info.hProcess, ctypes.byref(exit_code)):
            raise ctypes.WinError(ctypes.get_last_error())
        if exit_code.value != 0:
            diagnostic = result.with_suffix(".log")
            detail = diagnostic.read_text(encoding="utf-8")[:4096] if diagnostic.is_file() else "no worker diagnostic"
            raise UnavailableError(f"Standard-user worker failed (exit {exit_code.value}): {detail}")
        return {"caller": caller, "requested_child": expected, "same_user": True,
                "same_session": True, "session_id": child_session, "desktop_pid": shell_pid.value,
                "method": "CreateProcessW with same-user standard desktop parent and atomic owned job",
                "bootstrap_pid": info.dwProcessId}
    finally:
        try:
            job.terminate()
        finally:
            job.close()
            if info.hProcess:
                # Covers an assignment failure while the process is suspended.
                kernel.TerminateProcess(info.hProcess, 124)
                kernel.WaitForSingleObject(info.hProcess, 1000)
                kernel.CloseHandle(info.hProcess)
            if info.hThread:
                kernel.CloseHandle(info.hThread)
            if linked:
                kernel.CloseHandle(linked)
            if linked_original:
                kernel.CloseHandle(linked_original)
            if shell_process:
                kernel.CloseHandle(shell_process)
            if attributes is not None:
                kernel.DeleteProcThreadAttributeList(attributes)
            kernel.CloseHandle(token)


def run_standard(command: list[str], *, cwd: Path, timeout: float) -> tuple[ProcessResult, dict]:
    if not command or not 0 < timeout <= 3600:
        raise ValueError("A command and timeout in (0, 3600] seconds are required")
    caller = process_security()
    if not caller["elevated"] and 0x2000 <= caller["integrity_rid"] < 0x3000:
        _assert_standard(caller)
        result = run_process(command, cwd=cwd, timeout=timeout)
        return result, {"caller": caller, "child": caller, "same_user": True, "method": "already standard user"}
    temporary_root = ROOT / "out/process-launch"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with _request_directory(temporary_root) as directory:
        request = directory / "request.json"
        result_path = directory / "result.json"
        request.write_text(json.dumps({"args": command, "cwd": str(cwd), "timeout": timeout}), encoding="utf-8")
        provenance = _launch_linked_worker(request, result_path, cwd=cwd, timeout=timeout)
        if not result_path.is_file() or result_path.stat().st_size > MAX_RESULT_BYTES:
            raise ValueError("Missing or oversized standard-user command result")
        record = json.loads(result_path.read_text(encoding="utf-8"))
        _assert_standard(record["security"])
        if record["security"] != provenance["requested_child"]:
            raise ValueError("Actual worker security differs from the requested linked token")
        provenance["child"] = record["security"]
        provenance["worker_pid"] = record["worker_pid"]
        value = record["result"]
        if value["args"] != command:
            raise ValueError("Worker command evidence differs from requested argv")
        value["cleanup_errors"] = tuple(value["cleanup_errors"])
        return ProcessResult(**value), provenance


def _worker(request: Path, result: Path) -> int:
    security = process_security()
    _assert_standard(security)
    if request.stat().st_size > 1024 * 1024:
        raise ValueError("Worker request is too large")
    value = json.loads(request.read_text(encoding="utf-8"))
    if not value["args"] or not 0 < value["timeout"] <= 3600:
        raise ValueError("Invalid worker command/deadline")
    completed = run_process(value["args"], cwd=value["cwd"], timeout=value["timeout"])
    result.write_text(json.dumps({"security": security, "worker_pid": os.getpid(),
                                 "result": asdict(completed)}), encoding="utf-8")
    return 0


def _bootstrap(request: Path, result: Path) -> int:
    # A process created with an explicit standard desktop parent may retain an
    # elevated creator's default process/token DACL. A normal child creation
    # establishes the standard token's ordinary defaults before token queries
    # or command execution. Both generations already belong to the outer job.
    with result.with_suffix(".log").open("w", encoding="utf-8") as errors:
        return subprocess.call([sys.executable, str(Path(__file__).resolve()), "--worker", str(request), str(result)],
                               stdin=subprocess.DEVNULL, stdout=errors, stderr=errors)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--evidence", type=Path, help="New JSON file for actual token and process evidence")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    report = {"schema_version": 1, "status": "FAIL", "command": command}
    if args.evidence and args.evidence.exists():
        print("Fresh token evidence file required; existing file preserved", file=sys.stderr)
        return 2
    try:
        result, security = run_standard(command, cwd=Path.cwd(), timeout=args.timeout)
        report.update(status="PASS" if result.returncode == 0 and not result.cleanup_errors else "FAIL",
                      security=security, result=asdict(result))
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        code = result.returncode if not result.cleanup_errors else 1
    except UnavailableError as error:
        report.update(status="NOT_RUN", reason=str(error))
        print(f"NOT_RUN: {error}", file=sys.stderr)
        code = 3
    except TimeoutError as error:
        report.update(reason=str(error), timed_out=True)
        print(str(error), file=sys.stderr)
        code = 124
    except (OSError, ValueError, KeyError) as error:
        report["reason"] = str(error)
        print(str(error), file=sys.stderr)
        code = 1
    if args.evidence:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        with args.evidence.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
    return code


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--bootstrap":
        raise SystemExit(_bootstrap(Path(sys.argv[2]), Path(sys.argv[3])))
    if len(sys.argv) == 4 and sys.argv[1] == "--worker":
        raise SystemExit(_worker(Path(sys.argv[2]), Path(sys.argv[3])))
    raise SystemExit(main())
