"""CPU-only checks for the owned graphics lifecycle evidence gate."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import graphics_device_smoke as smoke
from process_runner import ProcessResult


LOCK = "a" * 64
SOURCE = "b" * 64
UUID = "c" * 32


def adapter() -> dict:
    return {"uuid": UUID, "name": "CPU evidence fixture", "driver_name": "No GPU",
            "driver_info": "No native libraries", "vendor_id": 1, "device_id": 2,
            "driver_version": 3, "api_version": 4, "queue_family": 0, "kind": "discrete",
            "features": dict.fromkeys(smoke.FEATURES, True),
            "formats": dict.fromkeys(smoke.FORMATS, True), "limits": smoke.LIMITS.copy(),
            "missing_requirements": []}


def diagnostics() -> dict:
    return {"warnings": 0, "errors": 0, "dropped": 0, "messages": []}


def records(mode: str = "smoke", validation: str = "required", device: str = "auto") -> list[dict]:
    identity = {"kind": "identity", "schema_version": 1, "mode": mode,
                "dependency_lock_sha256": LOCK, "source_sha256": SOURCE, "validation": validation,
                "selection": device, "assignment_05": "NOT_RUN", "compiler": "CPU fixture"}
    if mode == "describe":
        return [identity, {"kind": "description", "status": "PASS", "device_accessed": False,
                           "qualification": "NOT_RUN"}]
    if mode == "inventory":
        return [identity, {"kind": "inventory", "status": "PASS", "qualification": "NOT_RUN",
                           "adapters": [adapter()]}]
    cycles = []
    for index in range(1, smoke.CYCLES + 1):
        cycles.append({"kind": "cycle", "cycle": index,
                       "status": "PASS" if validation == "required" else "DIAGNOSTIC",
                       "second_owner_rejected": True, "idempotent_close": True,
                       "selection_matches": True, "second_owner_error": {"code": "conflict"},
                       "adapter": adapter(), "before_close": diagnostics(),
                       "after_close": diagnostics(), "after_second_close": diagnostics()})
    status = "PASS" if validation == "required" else "NOT_RUN"
    return [identity, *cycles, {"kind": "complete", "cycles": smoke.CYCLES,
                               "status": status, "qualification": status,
                               "execution_status": "PASS", "assignment_05": "NOT_RUN"}]


def result(rows: list[dict]) -> ProcessResult:
    return ProcessResult(["cpu-fixture"], 0, "\n".join(json.dumps(row) for row in rows), "", False)


class GraphicsDeviceEvidenceTests(unittest.TestCase):
    def inspect(self, rows=None, *, mode="smoke", validation="required", device="auto"):
        if rows is None:
            rows = records(mode, validation, device)
        return smoke.inspect_result(result(rows), mode, device, validation, LOCK, SOURCE)

    def test_complete_device_lifetimes_pass_only_the_checkpoint(self):
        rows, errors = self.inspect()
        self.assertFalse(errors)
        self.assertEqual(rows[-1]["qualification"], "PASS")
        self.assertEqual(rows[-1]["assignment_05"], "NOT_RUN")
        self.assertEqual(len(rows), 18)

    def test_description_and_inventory_do_not_qualify_a_device(self):
        for mode in ("describe", "inventory"):
            with self.subTest(mode=mode):
                rows, errors = self.inspect(mode=mode)
                self.assertFalse(errors)
                self.assertEqual(rows[-1]["qualification"], "NOT_RUN")
                rows[-1]["qualification"] = "PASS"
                self.assertTrue(self.inspect(rows, mode=mode)[1])

    def test_validation_off_cannot_claim_qualification(self):
        rows, errors = self.inspect(validation="off")
        self.assertFalse(errors)
        self.assertEqual(rows[-1]["qualification"], "NOT_RUN")
        for field in ("qualification", "status"):
            changed = deepcopy(rows)
            changed[-1][field] = "PASS"
            self.assertTrue(self.inspect(changed, validation="off")[1])
        rows[1]["status"] = "PASS"
        self.assertTrue(self.inspect(rows, validation="off")[1])

    def test_process_timeout_nonzero_and_failed_cleanup_override_valid_output(self):
        complete = result(records())
        for change in ({"timed_out": True}, {"returncode": 1}, {"returncode": 124},
                       {"cleanup_errors": ("Fixture child cleanup failed",)}):
            with self.subTest(change=change):
                self.assertTrue(smoke.inspect_result(replace(complete, **change), "smoke", "auto",
                                                     "required", LOCK, SOURCE)[1])

    def test_stale_or_wrong_identity_is_rejected(self):
        for field, value in (("dependency_lock_sha256", "d" * 64), ("source_sha256", "e" * 64),
                             ("mode", "inventory"), ("schema_version", True),
                             ("validation", "off"), ("selection", UUID),
                             ("assignment_05", "PASS"), ("compiler", None)):
            with self.subTest(field=field):
                rows = records()
                rows[0][field] = value
                self.assertTrue(self.inspect(rows)[1])

    def test_all_ordered_cycles_and_final_completion_are_required(self):
        base = records()
        incomplete = [base[:-1], base[:1] + base[2:], base + [base[-1]]]
        for rows in incomplete:
            self.assertTrue(self.inspect(rows)[1])
        for change in (0, 2, 17, "1", True):
            rows = records()
            rows[1]["cycle"] = change
            with self.subTest(cycle=change):
                self.assertTrue(self.inspect(rows)[1])
        for field, value in (("cycles", 15), ("status", "FAIL"), ("qualification", "NOT_RUN"),
                             ("execution_status", "FAIL"), ("assignment_05", "PASS")):
            rows = records()
            rows[-1][field] = value
            with self.subTest(field=field):
                self.assertTrue(self.inspect(rows)[1])

    def test_owner_rejection_close_and_exact_selection_are_required(self):
        for field in ("second_owner_rejected", "idempotent_close", "selection_matches"):
            rows = records()
            rows[1][field] = False
            self.assertTrue(self.inspect(rows)[1])
        rows = records()
        rows[1]["second_owner_error"]["code"] = "execution_failed"
        self.assertTrue(self.inspect(rows)[1])
        rows = records(device=UUID)
        self.assertFalse(self.inspect(rows, device=UUID)[1])
        rows[1]["adapter"]["uuid"] = "d" * 32
        self.assertTrue(self.inspect(rows, device=UUID)[1])
        self.assertTrue(self.inspect(rows)[1])

    def test_capability_features_formats_and_limit_failures_cannot_qualify(self):
        for group in ("features", "formats"):
            for name in adapter()[group]:
                rows = records()
                rows[1]["adapter"][group][name] = False
                with self.subTest(group=group, name=name):
                    self.assertTrue(self.inspect(rows)[1])
        for name, minimum in smoke.LIMITS.items():
            rows = records()
            rows[1]["adapter"]["limits"][name] = minimum - 1
            with self.subTest(limit=name):
                self.assertTrue(self.inspect(rows)[1])
        rows = records()
        rows[1]["adapter"]["missing_requirements"] = ["Fixture missing capability"]
        self.assertTrue(self.inspect(rows)[1])
        rows[1]["adapter"] = adapter()
        rows[1]["adapter"]["kind"] = "software"
        self.assertTrue(self.inspect(rows)[1])

    def test_inventory_metadata_and_numeric_types_are_validated(self):
        for group in ("features", "formats", "limits"):
            rows = records()
            rows[1]["adapter"][group] = {}
            self.assertTrue(self.inspect(rows)[1])
        for field, value in (("uuid", "invalid"), ("vendor_id", True), ("device_id", -1),
                             ("queue_family", 2**32), ("name", None), ("kind", "invalid"),
                             ("driver_name", "x" * 257)):
            rows = records()
            rows[1]["adapter"][field] = value
            with self.subTest(field=field):
                self.assertTrue(self.inspect(rows)[1])
        for adapters in ([], [adapter()] * 65, [adapter(), adapter()]):
            rows = records(mode="inventory")
            rows[-1]["adapters"] = adapters
            self.assertTrue(self.inspect(rows, mode="inventory")[1])

    def test_every_diagnostic_snapshot_must_remain_clean_and_consistent(self):
        for snapshot in ("before_close", "after_close", "after_second_close"):
            for field in ("warnings", "errors", "dropped"):
                for value in (1, -1, True):
                    rows = records()
                    rows[1][snapshot][field] = value
                    with self.subTest(snapshot=snapshot, field=field, value=value):
                        self.assertTrue(self.inspect(rows)[1])
        rows = records(validation="off")
        rows[1]["after_second_close"]["warnings"] = 1
        self.assertTrue(self.inspect(rows, validation="off")[1])

    def test_malformed_empty_duplicate_and_unbounded_json_is_rejected(self):
        for output in ("", "null", "{", '{"kind":"identity","kind":"other"}',
                       '{"value":NaN}', "x" * (8 * 1024 * 1024 + 1)):
            with self.subTest(prefix=output[:32]):
                process = replace(result(records()), stdout=output)
                self.assertTrue(smoke.inspect_result(process, "smoke", "auto", "required", LOCK, SOURCE)[1])

    def test_digest_is_sorted_and_binds_each_source_path_and_hash(self):
        inputs = {"b.cpp": "b" * 64, "a.hpp": "a" * 64}
        expected = hashlib.sha256(("a.hpp\n" + "a" * 64 + "\nb.cpp\n" + "b" * 64 + "\n").encode()).hexdigest()
        self.assertEqual(smoke.source_digest(inputs), expected)
        self.assertEqual(smoke.source_digest(dict(reversed(list(inputs.items())))), expected)
        self.assertNotEqual(smoke.source_digest({**inputs, "a.hpp": "d" * 64}), expected)
        self.assertNotEqual(smoke.source_digest({"renamed.hpp": "a" * 64, "b.cpp": "b" * 64}), expected)


if __name__ == "__main__":
    unittest.main()
