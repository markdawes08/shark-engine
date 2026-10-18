"""Real child-process regression tests for deadlines, cleanup and exit evidence."""

from pathlib import Path
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from process_runner import run_process


class ProcessRunnerTests(unittest.TestCase):
    def assert_stopped(self, pid: int) -> None:
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes
            api = ctypes.WinDLL("kernel32", use_last_error=True)
            api.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
            api.OpenProcess.restype = wintypes.HANDLE
            api.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
            api.WaitForSingleObject.restype = wintypes.DWORD
            api.CloseHandle.argtypes = (wintypes.HANDLE,)
            handle = api.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE only
            if not handle:
                self.assertEqual(ctypes.get_last_error(), 87, "Could not inspect fixture process")
                return  # ERROR_INVALID_PARAMETER: the PID no longer exists.
            try:
                self.assertEqual(api.WaitForSingleObject(handle, 1500), 0,
                                 f"Fixture process {pid} survived cleanup")
            finally:
                api.CloseHandle(handle)
        else:
            # SIGKILL delivery does not synchronously retire an orphaned child.
            # Observe its actual exit within the runner's one-second termination
            # budget, rather than interpreting one immediate kernel state sample
            # (including an uninterruptible I/O wait) as the final outcome.
            deadline = time.monotonic() + 1.0
            last_state = "not inspected"
            while True:
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    return
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    self.fail(f"Fixture process {pid} survived cleanup: {last_state}")
                # Use an independent observer, not the runner under test. A
                # disappearing PID can make ps return 1 with an empty result.
                state = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)],
                                       capture_output=True, text=True, timeout=remaining,
                                       check=False)
                self.assertIn(state.returncode, (0, 1),
                              f"Could not inspect fixture process: {state.stderr}")
                last_state = state.stdout.strip()
                # Orphaned grandchildren may await init's reap as zombies;
                # these have terminated and retain no open output handles.
                if not last_state or last_state.startswith("Z"):
                    return
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    self.fail(f"Fixture process {pid} survived cleanup: {last_state}")
                time.sleep(min(0.01, remaining))  # Bounded exit-condition polling.

    def cleanup_fixture(self, pid: int) -> None:
        """Even a broken runner must not leave the regression fixture running."""
        if os.name == "nt":
            taskkill = Path(os.environ["SystemRoot"]) / "System32/taskkill.exe"
            process = subprocess.Popen([str(taskkill), "/PID", str(pid), "/T", "/F"],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        else:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    def test_preserves_nonzero_status_and_both_output_streams(self):
        result = run_process([sys.executable, "-c",
                              "import sys; print('output'); print('error', file=sys.stderr); sys.exit(7)"],
                             timeout=5)
        self.assertEqual(result.returncode, 7)
        self.assertEqual(result.stdout.strip(), "output")
        self.assertEqual(result.stderr.strip(), "error")
        self.assertFalse(result.timed_out)
        self.assertFalse(result.cleanup_errors)

    def test_output_larger_than_pipe_buffers_is_preserved(self):
        result = run_process([sys.executable, "-c",
                              "import sys; sys.stdout.write('x'*1048576); sys.stderr.write('y'*1048576)"],
                             timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "x" * 1048576)
        self.assertEqual(result.stderr, "y" * 1048576)

    def check_descendant_cleanup(self, early_exit: bool):
        with tempfile.TemporaryDirectory(prefix="shark-process-test-") as directory:
            pids_path = Path(directory) / "pids.json"
            # Both processes inherit the output handles; the child deliberately
            # blocks far beyond the deadline. Readiness is the recorded PID pair.
            fixture = (
                "import json, os, pathlib, subprocess, sys, threading; "
                "child=subprocess.Popen([sys.executable,'-c','import threading; threading.Event().wait(30)']); "
                "pathlib.Path(sys.argv[1]).write_text(json.dumps([os.getpid(), child.pid])); "
                "print('fixture-ready', flush=True); "
                + ("sys.exit(7)" if early_exit else "threading.Event().wait(30)")
            )
            started = time.monotonic()
            pids = []
            try:
                result = run_process([sys.executable, "-c", fixture, str(pids_path)], timeout=1)
                elapsed = time.monotonic() - started
                self.assertTrue(pids_path.is_file(), "Fixture did not launch before the deadline")
                pids = json.loads(pids_path.read_text())
                self.assertLess(elapsed, 5, "Deadline waited for the 30-second descendant")
                self.assertIn("fixture-ready", result.stdout)
                self.assertEqual(result.returncode, 7 if early_exit else 124)
                self.assertEqual(result.timed_out, not early_exit)
                self.assertFalse(result.cleanup_errors)
                for pid in pids:
                    self.assert_stopped(pid)
            finally:
                if not pids and pids_path.is_file():
                    pids = json.loads(pids_path.read_text())
                for pid in pids:
                    self.cleanup_fixture(pid)

    def test_timeout_terminates_parent_and_inheriting_descendant(self):
        self.check_descendant_cleanup(early_exit=False)

    def test_early_parent_exit_does_not_leave_a_descendant(self):
        self.check_descendant_cleanup(early_exit=True)

    def test_invalid_deadline_rejected_before_launch(self):
        with self.assertRaises(ValueError):
            run_process([sys.executable, "-c", "raise SystemExit(0)"], timeout=0)


if __name__ == "__main__":
    unittest.main()
