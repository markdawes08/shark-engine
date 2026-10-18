"""Negative protocol checks for the native offscreen ownership evidence parser."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import offscreen_faults as subject
from process_runner import ProcessResult

DEVICE = "a" * 32
LOCK = "b" * 64
DIGEST = "c" * 64


def trace(fault: str) -> tuple[list[dict], str]:
    def resources(handles: int = 0, submitted: int = 0, recording: int = 0) -> dict:
        return dict(zip(subject.RESOURCE_FIELDS, (handles, submitted, recording, 0, 0, 0)))
    def clean() -> dict:
        return {"warnings": 0, "errors": 0, "dropped": 0, "messages": []}
    fatal = fault == "recording-error"
    operation = {"partial-creation": "graphics.test.offscreen_partial_creation",
                 "submission-timeout": "graphics.offscreen.wait_timeout",
                 "recording-error": "graphics.test.offscreen_recording_error"}[fault]
    rows = [
        {"kind": "identity", "schema_version": 1, "fault": fault, "selection": DEVICE,
         "validation": "required", "device_accessed": True, "dependency_lock_sha256": LOCK,
         "source_sha256": DIGEST, "compiler": "fixture"},
        {"kind": "ready", "adapter": {"uuid": DEVICE, "name": "fixture", "driver_name": "fixture",
                                      "driver_info": "fixture", "driver_version": 1},
         "resources": resources(), "diagnostics": clean()},
        {"kind": "fault", "fired": True, "error": {"code": "execution_failed", "operation": operation, "message": "injected"},
         "resources": resources(0 if fault == "partial-creation" else 17,
                                int(fault == "submission-timeout"), int(fatal)),
         "diagnostics": clean(), "teardown_calls": 0}]
    if fatal:
        rows.extend([
            {"kind": "retained", "error": {"code": "execution_failed", "operation": "graphics.offscreen.drain", "message": "retained"},
             "resources": resources(17, 0, 1), "diagnostics": clean(), "teardown_calls": 0},
            {"kind": "terminated", "expected": True, "teardown_calls": 0}])
        return rows, subject.FATAL_PREFIX + "retained" + subject.FATAL_SUFFIX
    rows.extend([
        {"kind": "retired", "resources": resources(), "diagnostics": clean(), "teardown_calls": 0},
        {"kind": "recovered", "submitted": 2, "completed": 2, "created_resources": 17, "retired_resources": 17,
         "center_rgba8": [64, 128, 192, 255], "center_depth": 0.5, "diagnostics": clean()},
        {"kind": "closed", "resources": resources(), "diagnostics": clean(), "teardown_calls": 5}])
    return rows, ""


def inspect(rows: list[dict], fault: str, stderr: str = "", **kwargs: object) -> list[str]:
    result = ProcessResult([], int(kwargs.get("returncode", 86 if fault == "recording-error" else 1)),
                           "\n".join(json.dumps(row) for row in rows), stderr,
                           bool(kwargs.get("timed_out", False)), tuple(kwargs.get("cleanup_errors", ())))
    return subject.inspect_result(result, fault, DEVICE, LOCK, DIGEST)[1]


class OffscreenFaultEvidenceTests(unittest.TestCase):
    def test_complete_protocols(self) -> None:
        for fault in subject.FAULTS:
            with self.subTest(fault=fault):
                rows, stderr = trace(fault)
                self.assertEqual(inspect(rows, fault, stderr), [])

    def test_wrong_identity_diagnostics_and_ownership(self) -> None:
        for fault in subject.FAULTS:
            baseline, stderr = trace(fault)
            mutations = [
                lambda rows: rows[0].update(source_sha256="stale"),
                lambda rows: rows[0].update(validation="off"),
                lambda rows: rows[1]["adapter"].update(uuid="d" * 32),
                lambda rows: rows[2].update(fired=False),
                lambda rows: rows[2].update(teardown_calls=1),
                lambda rows: rows[2]["resources"].update(live_swapchains=1),
                lambda rows: rows[2]["resources"].update(offscreen_resources=True),
                lambda rows: rows[2]["diagnostics"].update(warnings=1),
                lambda rows: rows[2]["error"].update(operation="other"),
                lambda rows: rows.pop(1)]
            for index, mutate in enumerate(mutations):
                with self.subTest(fault=fault, mutation=index):
                    rows = copy.deepcopy(baseline)
                    mutate(rows)
                    self.assertTrue(inspect(rows, fault, stderr))

    def test_timeout_cleanup_and_exit_failures(self) -> None:
        for fault in subject.FAULTS:
            rows, stderr = trace(fault)
            for kwargs in ({"timed_out": True}, {"cleanup_errors": ("retained child",)}, {"returncode": 0}):
                with self.subTest(fault=fault, kwargs=kwargs):
                    self.assertTrue(inspect(rows, fault, stderr, **kwargs))

    def test_final_retirement_and_fatal_ownership(self) -> None:
        for fault in subject.FAULTS:
            baseline, stderr = trace(fault)
            if fault == "recording-error":
                mutations = [lambda rows: rows[3]["resources"].update(offscreen_resources=0),
                             lambda rows: rows[4].update(expected=False),
                             lambda rows: rows[4].update(teardown_calls=5)]
            else:
                mutations = [lambda rows: rows[3]["resources"].update(offscreen_resources=17),
                             lambda rows: rows[4].update(completed=1),
                             lambda rows: rows[4].update(retired_resources=16),
                             lambda rows: rows[4].update(center_depth=1),
                             lambda rows: rows[5].update(teardown_calls=0)]
            for index, mutate in enumerate(mutations):
                with self.subTest(fault=fault, mutation=index):
                    rows = copy.deepcopy(baseline)
                    mutate(rows)
                    self.assertTrue(inspect(rows, fault, stderr))
            self.assertTrue(inspect(baseline, fault, "unexpected"))


if __name__ == "__main__":
    unittest.main()
