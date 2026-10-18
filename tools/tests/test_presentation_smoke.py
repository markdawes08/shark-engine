"""CPU evidence rejection tests; synthetic records do not qualify a real GPU."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import presentation_smoke as smoke
from process_runner import ProcessResult

LOCK = "a" * 64
SOURCE = "b" * 64
UUID = "c" * 32


def diagnostics() -> dict:
    return {"warnings": 0, "errors": 0, "dropped": 0, "messages": []}


def adapter() -> dict:
    return {"uuid": UUID, "name": "CPU evidence fixture", "driver_name": "No GPU",
            "driver_info": "No native libraries", "vendor_id": 1, "device_id": 2,
            "driver_version": 3, "api_version": 4, "queue_family": 0, "kind": "discrete",
            "features": dict.fromkeys(smoke.foundation.FEATURES, True),
            "formats": dict.fromkeys(smoke.foundation.FORMATS, True),
            "limits": smoke.foundation.LIMITS.copy(), "missing_requirements": []}


def extent(size: tuple[int, int]) -> dict:
    return {"width": size[0], "height": size[1]}


def window(size: tuple[int, int], iconified: bool = False) -> dict:
    return {"content": extent(size), "framebuffer": extent(size), "visible": True,
            "iconified": iconified, "close_requested": False}


def records(*, mode: str = "smoke", validation: str = "required", device: str = "auto",
            strategy: str = "present_fence", retry_first: bool = False,
            minimized_zero: bool = False) -> list[dict]:
    rows = [{"kind": "identity", "schema_version": 1, "mode": mode,
             "dependency_lock_sha256": LOCK, "source_sha256": SOURCE, "validation": validation,
             "selection": device, "assignment_05": "NOT_RUN", "compiler": "CPU fixture"}]
    if mode == "describe":
        return [*rows, {"kind": "description", "status": "PASS", "device_accessed": False,
                       "qualification": "NOT_RUN", "cycles": smoke.CYCLES, "frames_per_cycle": smoke.FRAMES,
                       "resizes_per_cycle": 4, "minimizes_per_cycle": 2, "attempt_limit": smoke.ATTEMPTS}]
    for cycle in range(1, smoke.CYCLES + 1):
        state = {"synchronization": "host_completion", "extent": extent((0, 0)), "image_count": 0,
                 "frame_limit": 2, "in_flight": 0, "peak_in_flight": 0, "submitted": 0, "completed": 0,
                 "presented": 0, "generation": 0, "retired_generations": 0, "suspended_frames": 0,
                 "waiting_frames": 0, "active": False, "faulted": False, "closed": False}
        size = (640, 400)

        def append(kind: str, **values) -> None:
            rows.append(deepcopy({"kind": kind, "cycle": cycle, **values}))

        append("cycle_begin", adapter=adapter(), window=window(size), stats=state)

        def frames(stage: str, count: int, recreate: bool = False) -> None:
            for frame in range(1, count + 1):
                recreated = recreate and frame == 1
                if recreated:
                    state.update(synchronization=strategy, frame_limit=2 if strategy == "present_fence" else 1,
                                 image_count=3, extent=extent(size), active=True, completed=state["submitted"],
                                 in_flight=0, retired_generations=state["generation"], generation=state["generation"] + 1)
                attempt = 1
                if retry_first and cycle == 1 and stage == "initial" and frame == 1:
                    state["waiting_frames"] += 1
                    append("frame", stage=stage, frame=frame, attempt=attempt, status="waiting",
                           recreated=recreated, window=window(size), stats=state)
                    recreated = False
                    attempt += 1
                state["submitted"] += 1
                state["presented"] += 1
                state["completed"] = state["submitted"] - (1 if strategy == "present_fence" else 0)
                state["in_flight"] = state["submitted"] - state["completed"]
                state["peak_in_flight"] = max(state["peak_in_flight"], state["in_flight"])
                append("frame", stage=stage, frame=frame, attempt=attempt, status="presented",
                       recreated=recreated, window=window(size), stats=state)

        frames("initial", 16, True)
        append("owner_conflict", second_owner_rejected=True,
               error={"code": "conflict", "operation": "graphics.window", "message": "Owner exists"}, stats=state)
        for index, size in enumerate(smoke.SIZES, 1):
            append("resize", index=index, requested=extent(size), window=window(size), stats=state)
            frames(f"resize{index}", 16, True)
            if index in (2, 4):
                before = deepcopy(state)
                state["suspended_frames"] += 1
                append("minimize", index=index // 2, status="suspended",
                       window=window((0, 0) if minimized_zero else size, True),
                       before=before, stats=state)
                append("restore", index=index // 2, window=window(size), stats=state)
                frames(f"restore{index // 2}", 8)
        before_drain = deepcopy(state)
        state.update(completed=state["submitted"], in_flight=0)
        drained = deepcopy(state)
        state.update(closed=True, active=False, retired_generations=state["generation"])
        append("cycle_end", status="PASS" if validation == "required" else "DIAGNOSTIC",
               presented_frames=smoke.FRAMES, idempotent_close=True, before_drain=before_drain,
               drained=drained, closed=state, second_closed=state, before_close=diagnostics(),
               after_close=diagnostics(), after_second_close=diagnostics())
    status = "PASS" if validation == "required" else "NOT_RUN"
    rows.append({"kind": "complete", "status": status, "qualification": status, "execution_status": "PASS",
                 "cycles": smoke.CYCLES, "presented_frames": smoke.CYCLES * smoke.FRAMES,
                 "resize_transitions": smoke.CYCLES * 4, "minimize_restore_cycles": smoke.CYCLES * 2,
                 "assignment_05": "NOT_RUN"})
    return rows


def result(rows: list[dict]) -> ProcessResult:
    return ProcessResult(["cpu-fixture"], 0, "\n".join(json.dumps(row) for row in rows), "", False)


class PresentationEvidenceTests(unittest.TestCase):
    def inspect(self, rows=None, *, mode="smoke", validation="required", device="auto", **options):
        if rows is None:
            rows = records(mode=mode, validation=validation, device=device, **options)
        return smoke.inspect_result(result(rows), mode, device, validation, LOCK, SOURCE)

    def test_complete_workload_accepts_both_synchronization_strategies(self):
        for strategy in ("present_fence", "host_completion"):
            with self.subTest(strategy=strategy):
                rows, errors = self.inspect(strategy=strategy)
                self.assertFalse(errors)
                self.assertEqual(rows[-1]["presented_frames"], 384)
                self.assertEqual(rows[-1]["assignment_05"], "NOT_RUN")
                self.assertEqual(len([row for row in rows if row["kind"] == "frame"]), 384)

    def test_bounded_retry_requires_real_eventual_presentation(self):
        rows, errors = self.inspect(retry_first=True)
        self.assertFalse(errors)
        self.assertEqual(rows[2]["status"], "waiting")
        self.assertEqual(rows[3]["attempt"], 2)
        rows[3]["attempt"] = 1
        self.assertTrue(self.inspect(rows)[1])

    def test_description_is_device_free_and_retains_fixed_workload(self):
        rows, errors = self.inspect(mode="describe")
        self.assertFalse(errors)
        for field, value in (("device_accessed", True), ("qualification", "PASS"), ("cycles", True),
                             ("frames_per_cycle", 32), ("resizes_per_cycle", 3), ("attempt_limit", 1000)):
            changed = deepcopy(rows)
            changed[1][field] = value
            with self.subTest(field=field):
                self.assertTrue(self.inspect(changed, mode="describe")[1])

    def test_validation_off_retains_not_run_qualification(self):
        rows, errors = self.inspect(validation="off")
        self.assertFalse(errors)
        self.assertEqual(rows[-1]["qualification"], "NOT_RUN")
        for field in ("status", "qualification"):
            changed = deepcopy(rows)
            changed[-1][field] = "PASS"
            self.assertTrue(self.inspect(changed, validation="off")[1])

    def test_timeout_process_failure_stderr_and_cleanup_override_complete_output(self):
        complete = result(records())
        for change in ({"timed_out": True}, {"returncode": 1}, {"returncode": 124},
                       {"cleanup_errors": ("Owned child still running",)}, {"stderr": "validation warning"}):
            with self.subTest(change=change):
                self.assertTrue(smoke.inspect_result(replace(complete, **change), "smoke", "auto", "required", LOCK, SOURCE)[1])

    def test_current_command_source_lock_and_schema_are_required(self):
        for field, value in (("dependency_lock_sha256", "d" * 64), ("source_sha256", "e" * 64),
                             ("mode", "describe"), ("schema_version", True), ("validation", "off"),
                             ("selection", UUID), ("assignment_05", "PASS"), ("compiler", "")):
            rows = records()
            rows[0][field] = value
            with self.subTest(field=field):
                self.assertTrue(self.inspect(rows)[1])

    def test_each_ordered_frame_cycle_and_final_completion_is_required(self):
        base = records()
        for changed in (base[:-1], base[:2] + base[3:], base[:1] + base[2:], base + [base[-1]],
                        base[:2] + [base[3], base[2]] + base[4:]):
            self.assertTrue(self.inspect(changed)[1])
        for field, value in (("cycle", True), ("frame", 2), ("attempt", 0), ("attempt", 17),
                             ("stage", "restore1"), ("status", "suspended"), ("recreated", 1)):
            rows = records()
            rows[2][field] = value
            with self.subTest(field=field, value=value):
                self.assertTrue(self.inspect(rows)[1])

    def test_fixed_total_counts_and_assignment_qualification_cannot_be_inflated(self):
        for field, value in (("cycles", 3), ("presented_frames", 383), ("resize_transitions", True),
                             ("minimize_restore_cycles", 9), ("assignment_05", "PASS"),
                             ("execution_status", "FAIL"), ("qualification", "NOT_RUN")):
            rows = records()
            rows[-1][field] = value
            with self.subTest(field=field):
                self.assertTrue(self.inspect(rows)[1])

    def test_exact_device_capabilities_and_repeated_selection_are_verified(self):
        self.assertFalse(self.inspect(device=UUID)[1])
        for field, value in (("uuid", "d" * 32), ("kind", "software"), ("name", "x" * 257)):
            rows = records(device=UUID)
            rows[1]["adapter"][field] = value
            self.assertTrue(self.inspect(rows, device=UUID)[1])
        rows = records()
        next(row for row in rows if row["kind"] == "cycle_begin" and row["cycle"] == 2)["adapter"]["uuid"] = "d" * 32
        self.assertTrue(self.inspect(rows)[1])
        for field in ("features", "formats", "limits"):
            rows = records()
            rows[1]["adapter"][field] = {}
            self.assertTrue(self.inspect(rows)[1])

    def test_counter_types_overflow_faults_and_frame_bounds_are_rejected(self):
        for field, value in (("submitted", True), ("submitted", -1), ("completed", 2**64),
                             ("completed", 10), ("presented", 10), ("in_flight", 3),
                             ("peak_in_flight", 3), ("frame_limit", 3), ("synchronization", "unknown"),
                             ("active", 1), ("faulted", True), ("image_count", 0), ("image_count", 9),
                             ("generation", 0), ("retired_generations", 1)):
            rows = records()
            rows[2]["stats"][field] = value
            with self.subTest(field=field, value=value):
                self.assertTrue(self.inspect(rows)[1])

    def test_frame_progress_and_actual_framebuffer_cannot_be_omitted(self):
        for field, value in (("submitted", 4), ("presented", 0), ("generation", 2),
                             ("suspended_frames", 1), ("closed", True)):
            rows = records()
            rows[2]["stats"][field] = value
            self.assertTrue(self.inspect(rows)[1])
        for change in ({"close_requested": True}, {"iconified": True}, {"visible": False},
                       {"framebuffer": {"width": 123, "height": 456}}):
            rows = records()
            rows[2]["window"].update(change)
            self.assertTrue(self.inspect(rows)[1])

    def test_resize_and_minimize_are_observed_not_just_counted(self):
        for kind, field, value in (("resize", "requested", extent((10, 10))), ("resize", "index", 2),
                                   ("minimize", "status", "waiting"), ("restore", "index", True)):
            rows = records()
            next(row for row in rows if row["kind"] == kind)[field] = value
            self.assertTrue(self.inspect(rows)[1])
        for kind in ("minimize", "restore"):
            rows = records()
            row = next(row for row in rows if row["kind"] == kind)
            row["window"]["iconified"] = kind != "minimize"
            self.assertTrue(self.inspect(rows)[1])
        rows = records()
        row = next(row for row in rows if row["kind"] == "minimize")
        row["stats"]["submitted"] += 1
        self.assertTrue(self.inspect(rows)[1])

    def test_dpi_scaled_startup_requires_normalization_and_minimize_accepts_zero_extent(self):
        # Real 150%-DPI Win32 preflight began at 960x600 despite requesting
        # 640x400, then reported both extents as zero while iconified. Preserve
        # rejection of an unnormalized initial workload, accept the native zero
        # observation, and still require the exact requested extent on restore.
        rows = records(minimized_zero=True)
        rows[1]["window"] = window((960, 600))
        self.assertTrue(self.inspect(rows)[1])
        rows = records(minimized_zero=True)
        self.assertFalse(self.inspect(rows)[1])
        for size in ((1, 0), (640, 400), (960, 600)):
            changed = deepcopy(rows)
            next(row for row in changed if row["kind"] == "minimize")["window"]["content"] = extent(size)
            self.assertTrue(self.inspect(changed)[1])
        next(row for row in rows if row["kind"] == "restore")["window"] = window((960, 600))
        self.assertTrue(self.inspect(rows)[1])

    def test_second_owner_cannot_replace_or_damage_active_presentation(self):
        for field, value in (("second_owner_rejected", False), ("error", {"code": "execution_failed"})):
            rows = records()
            next(row for row in rows if row["kind"] == "owner_conflict")[field] = value
            self.assertTrue(self.inspect(rows)[1])
        rows = records()
        next(row for row in rows if row["kind"] == "owner_conflict")["stats"]["submitted"] = 17
        self.assertTrue(self.inspect(rows)[1])

    def test_drain_and_close_must_complete_work_and_retire_every_generation(self):
        for snapshot, field, value in (("drained", "in_flight", 1), ("drained", "completed", 95),
                                       ("closed", "closed", False), ("closed", "retired_generations", 4),
                                       ("closed", "submitted", 97), ("second_closed", "generation", 6)):
            rows = records()
            next(row for row in rows if row["kind"] == "cycle_end")[snapshot][field] = value
            with self.subTest(snapshot=snapshot, field=field):
                self.assertTrue(self.inspect(rows)[1])

    def test_all_validation_snapshots_must_be_clean_and_idempotent(self):
        for snapshot in ("before_close", "after_close", "after_second_close"):
            for field, value in (("warnings", 1), ("errors", 1), ("dropped", 1), ("messages", ["warning"])):
                rows = records()
                next(row for row in rows if row["kind"] == "cycle_end")[snapshot][field] = value
                with self.subTest(snapshot=snapshot, field=field):
                    self.assertTrue(self.inspect(rows)[1])

    def test_malformed_oversized_duplicate_and_nonfinite_json_is_rejected(self):
        for output in ("", "null", "{", '{"kind":"identity","kind":"other"}',
                       '{"value":NaN}', "x" * (8 * 1024 * 1024 + 1)):
            process = replace(result(records()), stdout=output)
            self.assertTrue(smoke.inspect_result(process, "smoke", "auto", "required", LOCK, SOURCE)[1])

    def test_source_inputs_replace_only_native_fixture_identity(self):
        inputs = smoke.source_inputs()
        previous = smoke.foundation.source_inputs()
        self.assertNotIn("tests/graphics_device_smoke.cpp", inputs)
        self.assertIn("tests/presentation_smoke.cpp", inputs)
        previous.pop("tests/graphics_device_smoke.cpp")
        inputs.pop("tests/presentation_smoke.cpp")
        self.assertEqual(inputs, previous)


if __name__ == "__main__":
    unittest.main()
