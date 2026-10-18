"""Evidence acceptance tests; these need no graphics libraries or device."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from graphics_probe import inspect_result
from process_runner import ProcessResult


class ProbeEvidenceTests(unittest.TestCase):
    def result(self, *, exit_code=0, status="PASS", **changes):
        report = {"schema_version": 1, "mode": "smoke", "qualification": "NOT_RUN",
                  "dependency_lock_sha256": "a" * 64, "status": status,
                  "probe_source_sha256": "b" * 64, "exit_code": exit_code,
                  "validation_warnings": 0, "validation_errors": 0,
                  "selected_device_uuid": "c" * 32,
                  "vulkan_validation_enabled": True, "nvrhi_validation_enabled": True,
                  "devices": [{"uuid": "c" * 32, "reported_profile_supported": True}],
                  "stages": [{"name": name, "status": "PASS"} for name in
                             ("loader", "instance", "surface", "inventory", "device",
                              "nvrhi_device_and_validation", "swapchain", "teardown")],
                  "swapchain": {"image_count": 2, "extent": [64, 64], "present_mode": "FIFO", "presented_frames": 0},
                  "cases": [{"id": f"P02-{number:02d}", "status": "NOT_RUN"} for number in range(1, 9)]}
        if exit_code:
            report["error"] = {"code": "device_unavailable", "message": "Test fixture device absent"}
        report.update(changes)
        return ProcessResult(["probe"], exit_code, json.dumps(report), "", False)

    def inspect(self, result):
        return inspect_result(result, "smoke", "a" * 64, "b" * 64, "c" * 32)

    def test_success_is_setup_only(self):
        report, errors = self.inspect(self.result())
        self.assertFalse(errors)
        self.assertEqual(report["qualification"], "NOT_RUN")

    def test_missing_device_is_unrun_not_success(self):
        report, errors = self.inspect(self.result(exit_code=3, status="NOT_RUN"))
        self.assertFalse(errors)
        self.assertEqual(report["status"], "NOT_RUN")

    def test_nonzero_cannot_pass(self):
        for code in (1, 2, 3, 124, -9):
            with self.subTest(code=code):
                self.assertTrue(self.inspect(self.result(exit_code=code))[1])

    def test_false_qualification_and_stale_lock_rejected(self):
        for change in ({"qualification": "PASS"}, {"dependency_lock_sha256": "b" * 64},
                       {"probe_source_sha256": "d" * 64},
                       {"mode": "describe"}, {"schema_version": True}, {"schema_version": 2},
                       {"cases": []}, {"cases": [{"id": "P02-01", "status": "PASS"}] * 8},
                       {"cases": [{"id": [], "status": "NOT_RUN"}] * 8}):
            with self.subTest(change=change):
                self.assertTrue(self.inspect(self.result(**change))[1])

    def test_empty_or_contradictory_smoke_evidence_rejected(self):
        for change in ({"devices": []}, {"devices": [None]}, {"stages": []},
                       {"selected_device_uuid": "d" * 32}, {"vulkan_validation_enabled": False},
                       {"nvrhi_validation_enabled": False}, {"validation_errors": 1},
                       {"validation_warnings": False}, {"swapchain": None},
                       {"swapchain": {"image_count": True}}, {"exit_code": 1}):
            with self.subTest(change=change):
                self.assertTrue(self.inspect(self.result(**change))[1])

    def test_unavailable_needs_actionable_error(self):
        self.assertTrue(self.inspect(self.result(exit_code=3, status="NOT_RUN", error={}))[1])

    def test_timeout_and_failed_cleanup_cannot_pass(self):
        result = self.result()
        for timed_out, cleanup in ((True, ()), (False, ("cleanup failed",))):
            observed = ProcessResult(result.args, 0, result.stdout, "", timed_out, cleanup)
            self.assertTrue(self.inspect(observed)[1])

    def test_malformed_and_ambiguous_reports_rejected(self):
        for data in ("[]", "null", "diagnostic before JSON\n{}", '{"status":1,"status":2}',
                     '{"value":NaN}', "{}{}", ""):
            with self.subTest(data=data):
                self.assertTrue(self.inspect(ProcessResult(["probe"], 0, data, "", False))[1])


if __name__ == "__main__":
    unittest.main()
