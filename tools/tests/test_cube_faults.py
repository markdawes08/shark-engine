"""Reject incomplete, stale, weakened or premature window resource fault evidence."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cube_faults as subject
from process_runner import ProcessResult

DEVICE, LOCK, DIGEST, BUNDLE = "a" * 32, "b" * 64, "c" * 64, "d" * 64


def trace(fault: str) -> tuple[list[dict], str]:
    fatal, captured = fault == "recording-error", fault == "capture-timeout"
    def resources(active=False, handles=0, submitted=0, recording=0):
        return dict(zip(subject.RESOURCE_FIELDS, (0, 0, 0, int(active), 2 * int(active), 0,
                                                  handles, submitted, recording)))
    def clean():
        return {"warnings": 0, "errors": 0, "dropped": 0, "messages": []}
    def stats(attempted=True, closed=False, completed=False):
        return {"submitted": int(attempted and captured), "completed": int(completed and captured),
                "presented": int(attempted and captured), "generation": int(attempted),
                "retired_generations": int(closed), "active": attempted and not closed,
                "closed": closed, "faulted": attempted and not captured}
    def error(operation):
        return {"code": "execution_failed", "operation": operation, "message": "injected"}
    operation = {"partial-creation": "graphics.test.window_draw_partial_creation",
                 "capture-timeout": "graphics.capture.wait",
                 "recording-error": "graphics.test.recording_error"}[fault]
    rows = [
        {"kind": "identity", "schema_version": 1, "fault": fault, "selection": DEVICE,
         "validation": "required", "device_accessed": True, "dependency_lock_sha256": LOCK,
         "source_sha256": DIGEST, "bundle_sha256": BUNDLE, "compiler": "fixture"},
        {"kind": "ready", "adapter": {"uuid": DEVICE, "name": "fixture", "driver_name": "fixture",
                                      "driver_info": "fixture", "driver_version": 1},
         "resources": resources(), "diagnostics": clean(), "stats": stats(attempted=False)},
        {"kind": "fault", "fired": True, "error": error(operation),
         "resources": resources(True, 15 if captured or fatal else 0, int(captured), int(fatal)),
         "diagnostics": clean(), "teardown_calls": 0, "stats": stats()}]
    if fatal:
        rows.extend([
            {"kind": "retained", "error": error("graphics.test.recording_error"),
             "resources": resources(True, 15, 0, 1), "diagnostics": clean(), "stats": stats(), "teardown_calls": 0},
            {"kind": "terminated", "expected": True, "teardown_calls": 0}])
        return rows, subject.FATAL_PREFIX + "injected" + subject.FATAL_SUFFIX
    if captured:
        rows.extend([
            {"kind": "drained", "resources": resources(True, 15, 1), "diagnostics": clean(),
             "stats": stats(completed=True), "teardown_calls": 0},
            {"kind": "recovered", "submitted": 2, "completed": 2, "created_resources": 15, "retired_resources": 0,
             "center_rgba8": [255, 99, 71, 255], "center_depth": 0.2687578548442049, "diagnostics": clean()}])
    closed = {"kind": "closed", "resources": resources(), "diagnostics": clean(),
              "stats": stats(closed=True, completed=True), "teardown_calls": 7}
    if not captured:
        closed["error"] = error(operation)
    rows.append(closed)
    return rows, ""


def inspect(rows, fault, stderr="", **kwargs):
    result = ProcessResult([], kwargs.get("returncode", 86 if fault == "recording-error" else 1),
                           "\n".join(json.dumps(row) for row in rows), stderr,
                           kwargs.get("timed_out", False), kwargs.get("cleanup_errors", ()))
    return subject.inspect_result(result, fault, DEVICE, LOCK, DIGEST, BUNDLE)[1]


class CubeFaultEvidenceTests(unittest.TestCase):
    def test_complete_protocols(self):
        for fault in subject.FAULTS:
            with self.subTest(fault=fault):
                rows, stderr = trace(fault)
                self.assertEqual(inspect(rows, fault, stderr), [])

    def test_stale_identity_schema_and_diagnostics(self):
        mutations = [
            lambda r: r[0].update(source_sha256="stale"),
            lambda r: r[0].update(bundle_sha256="stale"),
            lambda r: r[0].update(validation="off"),
            lambda r: r[0].update(device_accessed=False),
            lambda r: r[0].update(schema_version=True),
            lambda r: r[0].update(extra="ignored"),
            lambda r: r[1]["adapter"].update(uuid="e" * 32),
            lambda r: r[1]["adapter"].update(driver_version=True),
            lambda r: r[1]["stats"].update(generation=1),
            lambda r: r[2].update(fired=False),
            lambda r: r[2].update(teardown_calls=1),
            lambda r: r[2]["resources"].update(offscreen_resources=17),
            lambda r: r[2]["resources"].update(window_draw_resources=True),
            lambda r: r[2]["diagnostics"].update(warnings=1),
            lambda r: r[2]["error"].update(operation="other"),
            lambda r: r[2]["error"].update(message="x" * 1025),
            lambda r: r[2]["stats"].update(generation=0),
            lambda r: r[2]["stats"].update(closed=True),
            lambda r: r.pop(1)]
        for fault in subject.FAULTS:
            baseline, stderr = trace(fault)
            for index, mutation in enumerate(mutations):
                with self.subTest(fault=fault, mutation=index):
                    rows = copy.deepcopy(baseline)
                    mutation(rows)
                    self.assertTrue(inspect(rows, fault, stderr))

    def test_capture_completion_pixel_and_retirement_failures(self):
        baseline, stderr = trace("capture-timeout")
        mutations = [
            lambda r: r[2]["resources"].update(window_draw_resources=0),
            lambda r: r[2]["stats"].update(faulted=True),
            lambda r: r[3]["resources"].update(window_draw_resources=0),
            lambda r: r[3].update(teardown_calls=2),
            lambda r: r[3]["stats"].update(completed=0),
            lambda r: r[4].update(completed=1),
            lambda r: r[4].update(retired_resources=15),
            lambda r: r[4].update(created_resources=0),
            lambda r: r[4].update(center_depth=1),
            lambda r: r[4].update(center_rgba8=[255, 32, 16, 255]),
            lambda r: r[4].update(center_rgba8=[255, 99, 71, 0]),
            lambda r: r[4].update(center_depth=float("nan")),
            lambda r: r[5]["resources"].update(window_draw_resources=15),
            lambda r: r[5]["stats"].update(retired_generations=0),
            lambda r: r[5].update(teardown_calls=5)]
        for index, mutation in enumerate(mutations):
            with self.subTest(mutation=index):
                rows = copy.deepcopy(baseline)
                mutation(rows)
                self.assertTrue(inspect(rows, "capture-timeout", stderr))

    def test_partial_creation_and_fatal_recording_ownership(self):
        for fault in ("partial-creation", "recording-error"):
            baseline, stderr = trace(fault)
            if fault == "partial-creation":
                mutations = [lambda r: r[2]["resources"].update(window_draw_resources=15),
                             lambda r: r[3].update(teardown_calls=0),
                             lambda r: r[3]["stats"].update(faulted=False),
                             lambda r: r[3]["error"].update(operation="graphics.capture.wait")]
            else:
                mutations = [lambda r: r[3]["resources"].update(window_draw_resources=0),
                             lambda r: r[3]["resources"].update(retained_recordings=0),
                             lambda r: r[3].update(teardown_calls=7),
                             lambda r: r[4].update(expected=False),
                             lambda r: r[4].update(expected=1),
                             lambda r: r[4].update(teardown_calls=7)]
            for index, mutation in enumerate(mutations):
                with self.subTest(fault=fault, mutation=index):
                    rows = copy.deepcopy(baseline)
                    mutation(rows)
                    self.assertTrue(inspect(rows, fault, stderr))

    def test_process_deadline_exit_cleanup_and_stderr(self):
        for fault in subject.FAULTS:
            rows, stderr = trace(fault)
            for kwargs in ({"returncode": 0}, {"timed_out": True}, {"cleanup_errors": ("retained child",)}):
                with self.subTest(fault=fault, kwargs=kwargs):
                    self.assertTrue(inspect(rows, fault, stderr, **kwargs))
            self.assertTrue(inspect(rows, fault, "unexpected"))


if __name__ == "__main__":
    unittest.main()
