"""Reject malformed/incomplete fault evidence; synthetic rows do not qualify GPUs."""
from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import presentation_faults as faults
from process_runner import ProcessResult

UUID = "a" * 32
LOCK = "b" * 64
SOURCE = "c" * 64


def clean() -> dict:
    return {"warnings": 0, "errors": 0, "dropped": 0, "messages": []}


def stats(**overrides: object) -> dict:
    return {"submitted": 1, "completed": 1, "presented": 1, "generation": 1,
            "retired_generations": 0, "active": True, "closed": False, "faulted": False, **overrides}


def native_rows(fault: str = "recording-error") -> list[dict]:
    fatal = fault.startswith("recording-")
    operation = "graphics.presentation.frame" if fault == "recording-exception" else "graphics.test." + fault.replace("-", "_")
    error = {"code": "execution_failed", "operation": operation, "message": "Synthetic injected fault"}
    resources = dict.fromkeys(faults.RESOURCE_FIELDS, 0)
    resources.update(live_swapchains=1, image_wrappers=3, image_views=3,
                     command_lists=2, acquire_semaphores=2, acquire_fences=2,
                     retained_recordings=int(fatal))
    rows = [
        {"kind": "identity", "schema_version": 1, "mode": "native", "fault": fault,
         "device_accessed": True, "selection": UUID, "validation": "required",
         "dependency_lock_sha256": LOCK, "source_sha256": SOURCE, "compiler": "Synthetic"},
        {"kind": "healthy", "stats": stats(), "diagnostics": clean(),
         "adapter": {"uuid": UUID, "name": "Fixture", "driver_name": "Synthetic", "driver_info": "No GPU accessed", "driver_version": 1}},
        {"kind": "fault", "fired": True, "primary": deepcopy(error), "repeated": deepcopy(error),
         "stats": stats(faulted=True), "resources": deepcopy(resources), "diagnostics": clean()},
        {"kind": "retained" if fatal else "closed", "error": error,
         "stats": stats(faulted=True, closed=not fatal, active=fatal, retired_generations=0 if fatal else 1),
         "resources": resources if fatal else dict.fromkeys(faults.RESOURCE_FIELDS, 0),
         "teardown_calls": 0 if fatal else 7, "diagnostics": clean()},
    ]
    if fatal:
        rows.append({"kind": "terminated", "expected": True, "teardown_calls": 0})
    return rows


def process(rows: list[dict], fatal: bool = True) -> ProcessResult:
    final = rows[-2] if fatal else rows[-1]
    return ProcessResult(args=["synthetic"], returncode=86 if fatal else 1,
                         stdout="".join(json.dumps(row) + "\n" for row in rows),
                         stderr=faults.FATAL_PREFIX + final["error"]["message"] + faults.FATAL_SUFFIX if fatal else "",
                         timed_out=False)


class FaultEvidence(unittest.TestCase):
    def check(self, rows: list[dict], fault: str = "recording-error", *, accepted: bool = False) -> None:
        _, errors = faults.inspect_result(process(rows, fault.startswith("recording-")), "native", fault, UUID, LOCK, SOURCE)
        self.assertEqual(bool(errors), not accepted, errors)

    def test_all_native_policy_modes(self) -> None:
        for fault in faults.NATIVE_FAULTS:
            with self.subTest(fault=fault):
                self.check(native_rows(fault), fault, accepted=True)

    def test_all_cpu_fatal_modes(self) -> None:
        for fault, operation in (("waiting", "graphics.presentation.drain_timeout"), ("error", "graphics.test.cpu_drain"), ("exception", "graphics.presentation.close")):
            error = {"code": "execution_failed", "operation": operation, "message": "Synthetic drain failure"}
            rows = [{"kind": "identity", "schema_version": 1, "mode": "cpu", "fault": fault, "device_accessed": False},
                    {"kind": "retained", "error": error, "stats": stats(), "teardown_calls": 0},
                    {"kind": "terminated", "expected": True, "teardown_calls": 0}]
            _, errors = faults.inspect_result(process(rows), "cpu", fault)
            self.assertEqual(errors, [])

    def test_success_exit_does_not_mask_failure(self) -> None:
        for code in (0, 1, 87, 88, 124):
            _, errors = faults.inspect_result(replace(process(native_rows()), returncode=code), "native", "recording-error", UUID, LOCK, SOURCE)
            self.assertTrue(errors)

    def test_timeout_cleanup_and_unexpected_stderr(self) -> None:
        original = process(native_rows())
        for result in (replace(original, timed_out=True), replace(original, cleanup_errors=("Child escaped",)), replace(original, stderr=original.stderr + "driver warning\n"), replace(original, stderr="")):
            _, errors = faults.inspect_result(result, "native", "recording-error", UUID, LOCK, SOURCE)
            self.assertTrue(errors)

    def test_missing_or_extra_rows(self) -> None:
        original = process(native_rows())
        lines = original.stdout.splitlines()
        for stdout in ("\n".join(lines[:-1]), "\n".join(lines[1:]), original.stdout + lines[-1] + "\n"):
            _, errors = faults.inspect_result(replace(original, stdout=stdout), "native", "recording-error", UUID, LOCK, SOURCE)
            self.assertTrue(errors)

    def test_identity_and_validation_mismatch(self) -> None:
        for key, value in (("schema_version", True), ("source_sha256", "x" * 64), ("dependency_lock_sha256", "x" * 64), ("selection", "x" * 32), ("validation", "off"), ("device_accessed", False), ("fault", "surface-error")):
            rows = native_rows()
            rows[0][key] = value
            self.check(rows)

    def test_actual_validation_noise_is_not_an_injected_error(self) -> None:
        for index in (1, 2, 3):
            for field in ("warnings", "errors", "dropped"):
                rows = native_rows()
                rows[index]["diagnostics"][field] = 1
                self.check(rows)

    def test_real_driver_identity_required(self) -> None:
        for key, value in (("uuid", "x" * 32), ("driver_version", True), ("driver_name", "")):
            rows = native_rows()
            rows[1]["adapter"][key] = value
            self.check(rows)

    def test_primary_error_must_survive_repeat_and_cleanup(self) -> None:
        for index, key in ((2, "repeated"), (3, "error")):
            rows = native_rows()
            rows[index][key]["operation"] = "replacement"
            self.check(rows)

    def test_no_work_after_fault(self) -> None:
        for index in (2, 3):
            for field in ("submitted", "presented"):
                rows = native_rows()
                rows[index]["stats"][field] += 1
                self.check(rows)

    def test_retained_real_recording_and_no_teardown(self) -> None:
        for index in (2, 3):
            rows = native_rows()
            rows[index]["resources"]["retained_recordings"] = 0
            self.check(rows)
        for index in (3, 4):
            rows = native_rows()
            rows[index]["teardown_calls"] = 1
            self.check(rows)

    def test_termination_must_be_expected(self) -> None:
        rows = native_rows()
        rows[-1]["expected"] = False
        self.check(rows)

    def test_cleanup_must_release_every_observed_resource(self) -> None:
        for field in faults.RESOURCE_FIELDS:
            rows = native_rows("device-error")
            rows[-1]["resources"][field] = 1
            self.check(rows, "device-error")

    def test_boolean_counter_and_resource_bound_rejected(self) -> None:
        rows = native_rows()
        rows[2]["stats"]["submitted"] = True
        self.check(rows)
        rows = native_rows()
        rows[2]["resources"]["image_views"] = 9
        self.check(rows)

    def test_duplicate_keys_nonfinite_and_oversized_output(self) -> None:
        original = process(native_rows())
        for stdout in ('{"kind":"identity","kind":"identity"}\n', '{"kind":NaN}\n', "x" * (1024 * 1024 + 1)):
            _, errors = faults.inspect_result(replace(original, stdout=stdout), "native", "recording-error", UUID, LOCK, SOURCE)
            self.assertTrue(errors)


if __name__ == "__main__":
    unittest.main()
