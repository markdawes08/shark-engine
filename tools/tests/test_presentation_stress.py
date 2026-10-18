"""Mutation tests for native stress evidence; synthetic records never qualify a GPU."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import presentation_stress as stress
from process_runner import ProcessResult
from test_presentation_smoke import adapter, diagnostics, extent, window, LOCK, SOURCE, UUID


def initial_state() -> dict:
    return {"synchronization": "host_completion", "extent": extent((0, 0)), "image_count": 0,
            "frame_limit": 2, "in_flight": 0, "peak_in_flight": 0, "submitted": 0, "completed": 0,
            "presented": 0, "generation": 0, "retired_generations": 0, "suspended_frames": 0,
            "waiting_frames": 0, "active": False, "faulted": False, "closed": False}


def resource_snapshot(state: dict) -> dict:
    values = dict.fromkeys(stress.RESOURCE_FIELDS, 0)
    if not state["closed"]:
        values.update(live_swapchains=1, image_wrappers=3, image_views=3, command_lists=2,
                      acquire_semaphores=2, acquire_fences=2)
        if state["synchronization"] == "present_fence":
            values.update(present_semaphores=3, present_fences=3)
    return values


def records(*, profile="debug", strategy="present_fence", mode="stress",
            validation="required", device="auto", minimized_zero=False) -> list[dict]:
    rows = [{"kind": "identity", "schema_version": 1, "mode": mode, "profile": profile,
             "diagnostic": False,
             "dependency_lock_sha256": LOCK, "source_sha256": SOURCE, "validation": validation,
             "selection": device, "assignment_05": "NOT_RUN", "compiler": "CPU fixture"},
            stress.workload(profile)]
    if mode == "describe":
        return [*rows, {"kind": "description", "status": "PASS", "device_accessed": False,
                       "qualification": "NOT_RUN", "native_graphics_available": True,
                       "usage": "CPU description"}]
    size = (640, 400)
    state = initial_state()
    active_baseline = {"private_bytes": 100 * 1024 * 1024, "handles": 100,
                       "gdi_objects": 15, "user_objects": 26}
    closed_baseline = {"private_bytes": 50 * 1024 * 1024, "handles": 50,
                       "gdi_objects": 10, "user_objects": 4}
    active_values = []
    closed_values = []

    def append(kind: str, **values) -> None:
        rows.append(deepcopy({"kind": kind, **values}))

    def frames(count: int, recreate=False) -> None:
        if recreate:
            state.update(synchronization=strategy, frame_limit=2 if strategy == "present_fence" else 1,
                         image_count=3, extent=extent(size), active=True,
                         retired_generations=state["generation"], generation=state["generation"] + 1)
        state["submitted"] += count
        state["presented"] += count
        state.update(completed=state["submitted"], in_flight=0, peak_in_flight=1)

    def sample(phase: str, index: int, elapsed: int) -> None:
        # Vary observations so final-peak aggregation must inspect every sample.
        value = dict(active_baseline)
        if phase != "warmup":
            value["private_bytes"] += (index % 7) * 1024
            value["handles"] += index % 3
        active_values.append(value)
        append("sample", phase=phase, index=index, elapsed_ms=elapsed, window=window(size),
               stats=state, resources=resource_snapshot(state), process=value,
               baseline=active_baseline, within_budget=True)

    def close_fields() -> dict:
        state.update(closed=True, active=False, retired_generations=state["generation"])
        return deepcopy({"closed": state, "second_closed": state, "idempotent_close": True,
                         "before_close": diagnostics(), "after_close": diagnostics(),
                         "after_second_close": diagnostics(), "resources": resource_snapshot(state)})

    append("stress_begin", adapter=adapter(), window=window(size), stats=state)
    frames(24, True)
    before = deepcopy(state)
    state["suspended_frames"] += 1
    append("warmup_minimize", index=1, status="suspended", window=window(size, True),
           before=before, stats=state)
    before = deepcopy(state)
    frames(8)
    append("warmup_restore", index=1, window=window(size), before=before, stats=state)
    sample("warmup", 0, 0)
    duration = stress.DURATIONS[profile] * 1000
    schedule = [(duration * index // 101, "resize", index) for index in range(1, 101)]
    schedule.extend((milliseconds, "periodic", milliseconds // 5000) for milliseconds in range(5000, duration + 1, 5000))
    for elapsed, kind, index in sorted(schedule):
        frames(1)
        if kind == "periodic":
            sample("periodic", index, elapsed)
            continue
        size = stress.smoke.SIZES[(index - 1) % 4]
        before = deepcopy(state)
        frames(16, True)
        append("resize", index=index, requested=extent(size), window=window(size), before=before, stats=state)
        sample("resize", index, elapsed)
        if index % 5 == 0:
            before = deepcopy(state)
            state["suspended_frames"] += 1
            append("minimize", index=index // 5, status="suspended",
                   window=window((0, 0) if minimized_zero else size, True), before=before, stats=state)
            frames(8)
            append("restore", index=index // 5, window=window(size), stats=state)
            sample("restore", index // 5, elapsed)
    sustained = state["presented"] - 32
    periodic = duration // 5000
    sample("final", 0, duration)
    append("sustained_end", elapsed_ms=duration, presented_frames=sustained,
           resize_transitions=100, minimize_restore_cycles=20, periodic_samples=periodic, stats=state)
    append("active_close", **close_fields())
    for cycle in range(1, 21):
        size, state = (640, 400), initial_state()
        append("lifetime_begin", cycle=cycle, adapter=adapter(), window=window(size), stats=state)
        frames(16, True)
        value = dict(closed_baseline)
        if cycle > 5:
            value["private_bytes"] += cycle % 5 * 2048
            value["handles"] += cycle % 4
        if cycle >= 5:
            closed_values.append(value)
        append("lifetime_end", cycle=cycle, **close_fields(), process=value,
               baseline=closed_baseline if cycle >= 5 else dict.fromkeys(closed_baseline, 0),
               measured=cycle >= 5, within_budget=True)
    qualification = "PASS" if validation == "required" else "NOT_RUN"
    append("complete", status=qualification, qualification=qualification, execution_status="PASS",
           diagnostic=False, active_budget_failures=0, closed_budget_failures=0,
           sustained_duration_ms=duration, sustained_presented_frames=sustained,
           resize_transitions=100, minimize_restore_cycles=20, lifetimes=20,
           lifetime_presented_frames=320, periodic_samples=periodic,
           active_baseline=active_baseline, closed_baseline=closed_baseline,
           active_peak={key: max(value[key] for value in active_values) for key in active_baseline},
           closed_peak={key: max(value[key] for value in closed_values) for key in closed_baseline},
           assignment_05="NOT_RUN")
    return rows


def result(rows: list[dict]) -> ProcessResult:
    return ProcessResult(["cpu-fixture"], 0, "\n".join(json.dumps(row) for row in rows), "", False)


def first(rows: list[dict], kind: str, **fields) -> dict:
    return next(row for row in rows if row["kind"] == kind and all(row.get(key) == value for key, value in fields.items()))


def invent_sustained_frame(rows: list[dict], start: int) -> None:
    """Insert coherent extra work, keeping later counters/aggregates consistent."""
    for row in rows[start:]:
        if row["kind"] == "lifetime_begin":
            break
        for value in row.values():
            if isinstance(value, dict) and {"submitted", "completed", "presented"} <= value.keys():
                for field in ("submitted", "completed", "presented"):
                    value[field] += 1
        if row["kind"] == "sustained_end":
            row["presented_frames"] += 1
    rows[-1]["sustained_presented_frames"] += 1


class StressEvidenceTests(unittest.TestCase):
    def inspect(self, rows=None, *, profile="debug", mode="stress", validation="required", device="auto", **options):
        if rows is None:
            rows = records(profile=profile, mode=mode, validation=validation, device=device, **options)
        return stress.inspect_result(result(rows), mode, device, validation, LOCK, SOURCE, profile)

    def test_complete_profiles_and_native_strategies_accept(self):
        for profile in stress.DURATIONS:
            for strategy in ("present_fence", "host_completion"):
                with self.subTest(profile=profile, strategy=strategy):
                    rows, errors = self.inspect(profile=profile, strategy=strategy, minimized_zero=True)
                    self.assertFalse(errors)
                    self.assertEqual(rows[-1]["lifetimes"], 20)
                    self.assertEqual(rows[-1]["assignment_05"], "NOT_RUN")

    def test_description_remains_device_free_and_binds_compiled_policy(self):
        for profile in stress.DURATIONS:
            rows, errors = self.inspect(mode="describe", profile=profile)
            self.assertFalse(errors)
            for field, value in (("device_accessed", True), ("qualification", "PASS"), ("native_graphics_available", False)):
                changed = deepcopy(rows)
                changed[2][field] = value
                self.assertTrue(self.inspect(changed, mode="describe", profile=profile)[1])

    def test_validation_off_cannot_qualify(self):
        rows, errors = self.inspect(validation="off")
        self.assertFalse(errors)
        self.assertEqual(rows[-1]["qualification"], "NOT_RUN")
        for field in ("qualification", "status"):
            changed = deepcopy(rows)
            changed[-1][field] = "PASS"
            self.assertTrue(self.inspect(changed, validation="off")[1])

    def test_diagnostic_growth_investigation_cannot_qualify_even_with_clean_budgets(self):
        rows = records()
        rows[0]["diagnostic"] = True
        self.assertTrue(self.inspect(rows)[1])
        del rows[0]["diagnostic"]
        self.assertTrue(self.inspect(rows)[1])
        for field, value in (("diagnostic", True), ("active_budget_failures", 1),
                             ("closed_budget_failures", 1), ("active_budget_failures", False),
                             ("closed_budget_failures", -1)):
            rows = records()
            rows[-1][field] = value
            self.assertTrue(self.inspect(rows)[1])

    def test_process_timeout_failure_cleanup_and_stderr_reject_complete_output(self):
        value = result(records())
        for change in ({"returncode": 1}, {"timed_out": True}, {"stderr": "warning"},
                       {"cleanup_errors": ("owned child still alive",)}):
            with self.subTest(change=change):
                self.assertTrue(stress.inspect_result(replace(value, **change), "stress", "auto", "required", LOCK, SOURCE, "debug")[1])

    def test_identity_source_lock_profile_and_schema_are_bound(self):
        for field, value in (("source_sha256", "e" * 64), ("dependency_lock_sha256", "f" * 64),
                             ("profile", "asan"), ("mode", "describe"), ("schema_version", True),
                             ("selection", UUID), ("validation", "off"), ("compiler", ""),
                             ("assignment_05", "PASS")):
            rows = records()
            rows[0][field] = value
            with self.subTest(field=field):
                self.assertTrue(self.inspect(rows)[1])

    def test_workload_allowances_cannot_be_changed(self):
        for key, value in stress.workload("debug").items():
            rows = records()
            rows[1][key] = value + 1 if type(value) is int else "changed"
            with self.subTest(key=key):
                self.assertTrue(self.inspect(rows)[1])
        rows = records()
        rows[1]["handle_allowance"] = True
        self.assertTrue(self.inspect(rows)[1])

    def test_missing_reordered_and_extra_transitions_or_lifetimes_fail(self):
        for kind in ("stress_begin", "warmup_minimize", "warmup_restore", "resize", "minimize", "restore", "sustained_end", "active_close",
                     "lifetime_begin", "lifetime_end", "complete"):
            rows = records()
            rows.remove(first(rows, kind))
            with self.subTest(kind=kind):
                self.assertTrue(self.inspect(rows)[1])
        rows = records()
        rows += [deepcopy(rows[-1])]
        self.assertTrue(self.inspect(rows)[1])
        rows = records()
        first(rows, "lifetime_begin", cycle=2)["cycle"] = 3
        self.assertTrue(self.inspect(rows)[1])

    def test_requested_uuid_capabilities_and_same_adapter_each_lifetime(self):
        self.assertFalse(self.inspect(device=UUID)[1])
        for kind in ("stress_begin", "lifetime_begin"):
            rows = records(device=UUID)
            first(rows, kind)["adapter"]["uuid"] = "d" * 32
            self.assertTrue(self.inspect(rows, device=UUID)[1])
        for field in ("features", "formats", "limits"):
            rows = records()
            first(rows, "stress_begin")["adapter"][field] = {}
            self.assertTrue(self.inspect(rows)[1])

    def test_warmup_and_drained_samples_require_real_completed_frames(self):
        for field, value in (("presented", 31), ("completed", 31), ("in_flight", 1), ("active", False),
                             ("faulted", True), ("generation", 0), ("retired_generations", 1),
                             ("submitted", True), ("completed", 2 ** 64), ("frame_limit", 3)):
            rows = records()
            first(rows, "sample", phase="warmup")["stats"][field] = value
            with self.subTest(field=field):
                self.assertTrue(self.inspect(rows)[1])

    def test_fixed_warmup_suspends_once_and_restores_eight_of_32_frames(self):
        for kind, field, value in (("warmup_minimize", "status", "waiting"),
                                   ("warmup_minimize", "index", 2), ("warmup_restore", "index", True)):
            rows = records()
            first(rows, kind)[field] = value
            self.assertTrue(self.inspect(rows)[1])
        for kind in ("warmup_minimize", "warmup_restore"):
            rows = records()
            first(rows, kind)["window"]["iconified"] = kind != "warmup_minimize"
            self.assertTrue(self.inspect(rows)[1])
        rows = records()
        first(rows, "warmup_minimize")["stats"]["submitted"] += 1
        self.assertTrue(self.inspect(rows)[1])
        rows = records()
        first(rows, "warmup_restore")["stats"]["presented"] = 31
        self.assertTrue(self.inspect(rows)[1])
        rows = records()
        rows.insert(rows.index(first(rows, "sample", phase="warmup")), deepcopy(first(rows, "warmup_restore")))
        self.assertTrue(self.inspect(rows)[1])

    def test_resources_are_exact_and_closed_owners_have_none(self):
        for field in stress.RESOURCE_FIELDS:
            rows = records()
            first(rows, "sample", phase="warmup")["resources"][field] += 1
            with self.subTest(field=field):
                self.assertTrue(self.inspect(rows)[1])
            for kind in ("active_close", "lifetime_end"):
                rows = records()
                first(rows, kind)["resources"][field] = 1
                self.assertTrue(self.inspect(rows)[1])

    def test_final_sample_is_mandatory_immediately_before_sustained_end(self):
        rows = records()
        rows.remove(first(rows, "sample", phase="final"))
        self.assertTrue(self.inspect(rows)[1])
        for field, value in (("index", 1), ("elapsed_ms", 59999), ("within_budget", False)):
            rows = records()
            first(rows, "sample", phase="final")[field] = value
            self.assertTrue(self.inspect(rows)[1])
        rows = records()
        final_index = rows.index(first(rows, "sample", phase="final"))
        rows.insert(final_index + 1, deepcopy(first(rows, "resize")))
        self.assertTrue(self.inspect(rows)[1])

    def test_budget_boundaries_are_inclusive_and_compiled_asan_allowance_is_distinct(self):
        for profile in stress.DURATIONS:
            for field, allowance in (("private_bytes", stress.MEMORY_LIMITS[profile]), ("handles", 8),
                                     ("gdi_objects", 0), ("user_objects", 0)):
                rows = records(profile=profile)
                row = first(rows, "sample", phase="resize")
                row["process"][field] = row["baseline"][field] + allowance
                rows[-1]["active_peak"][field] = row["process"][field]
                with self.subTest(profile=profile, field=field):
                    self.assertFalse(self.inspect(rows, profile=profile)[1])
                row["process"][field] += 1
                rows[-1]["active_peak"][field] += 1
                self.assertTrue(self.inspect(rows, profile=profile)[1])

    def test_active_and_closed_measurements_enforce_budgets(self):
        for kind, selection in (("sample", {"phase": "resize"}), ("lifetime_end", {"cycle": 6})):
            for field, allowance in (("private_bytes", stress.MEMORY_LIMITS["debug"]), ("handles", 8),
                                     ("gdi_objects", 0), ("user_objects", 0)):
                rows = records()
                row = first(rows, kind, **selection)
                row["process"][field] = row["baseline"][field] + allowance + 1
                with self.subTest(kind=kind, field=field):
                    self.assertTrue(self.inspect(rows)[1])
            rows = records()
            first(rows, kind, **selection)["within_budget"] = False
            self.assertTrue(self.inspect(rows)[1])

    def test_measurement_types_and_baselines_cannot_hide_growth(self):
        for kind, selection in (("sample", {"phase": "resize"}), ("lifetime_end", {"cycle": 6})):
            for field, value in (("private_bytes", True), ("handles", 0), ("handles", -1),
                                 ("private_bytes", 2 ** 64), ("gdi_objects", True), ("user_objects", -1)):
                rows = records()
                first(rows, kind, **selection)["process"][field] = value
                self.assertTrue(self.inspect(rows)[1])
            rows = records()
            first(rows, kind, **selection)["baseline"]["handles"] += 1
            self.assertTrue(self.inspect(rows)[1])
        rows = records()
        first(rows, "lifetime_end", cycle=4)["measured"] = True
        self.assertTrue(self.inspect(rows)[1])

    def test_gui_measurements_allow_legitimate_zero_but_require_both_counters(self):
        rows = records()
        for row in rows:
            for value in row.values():
                if isinstance(value, dict) and {"private_bytes", "handles"} <= value.keys():
                    value["gdi_objects"] = 0
                    value["user_objects"] = 0
        self.assertFalse(self.inspect(rows)[1])
        for field in ("gdi_objects", "user_objects"):
            changed = deepcopy(rows)
            del first(changed, "sample", phase="resize")["process"][field]
            self.assertTrue(self.inspect(changed)[1])

    def test_duration_and_periodic_progress_have_lower_and_upper_bounds(self):
        for value in (59999, 70001, True, -1):
            rows = records()
            first(rows, "sustained_end")["elapsed_ms"] = value
            self.assertTrue(self.inspect(rows)[1])
        for value in (4999, 10001, -1):
            rows = records()
            first(rows, "sample", phase="periodic")["elapsed_ms"] = value
            self.assertTrue(self.inspect(rows)[1])
        rows = records()
        periodic = first(rows, "sample", phase="periodic")
        periodic["stats"].update(presented=32, submitted=32, completed=32)
        self.assertTrue(self.inspect(rows)[1])

    def test_resize_generation_extent_and_exact_frame_counts(self):
        for change in ("dimensions", "generation", "frames", "order"):
            rows = records()
            row = first(rows, "resize")
            if change == "dimensions":
                row["requested"] = extent((123, 456))
            elif change == "generation":
                row["stats"]["generation"] = row["before"]["generation"]
            elif change == "frames":
                row["stats"]["presented"] -= 1
            else:
                row["index"] = True
            self.assertTrue(self.inspect(rows)[1])

    def test_minimize_suspends_and_restore_really_presents(self):
        for kind, field, value in (("minimize", "status", "waiting"), ("restore", "index", 2)):
            rows = records()
            first(rows, kind)[field] = value
            self.assertTrue(self.inspect(rows)[1])
        for kind in ("minimize", "restore"):
            rows = records()
            first(rows, kind)["window"]["iconified"] = kind != "minimize"
            self.assertTrue(self.inspect(rows)[1])
        rows = records()
        first(rows, "minimize")["stats"]["submitted"] += 1
        self.assertTrue(self.inspect(rows)[1])
        rows = records()
        first(rows, "restore")["stats"]["presented"] -= 1
        self.assertTrue(self.inspect(rows)[1])

    def test_validation_must_remain_clean_through_repeated_close(self):
        for kind in ("active_close", "lifetime_end"):
            for stage in ("before_close", "after_close", "after_second_close"):
                for field, value in (("warnings", 1), ("errors", 1), ("dropped", 1), ("messages", ["warning"])):
                    rows = records()
                    first(rows, kind)[stage][field] = value
                    self.assertTrue(self.inspect(rows)[1])
            rows = records()
            first(rows, kind)["idempotent_close"] = False
            self.assertTrue(self.inspect(rows)[1])
            rows = records()
            first(rows, kind)["second_closed"]["presented"] += 1
            self.assertTrue(self.inspect(rows)[1])

    def test_close_requires_all_submitted_uses_retired(self):
        for kind in ("active_close", "lifetime_end"):
            for field in ("completed", "retired_generations"):
                rows = records()
                row = first(rows, kind)
                row["closed"][field] -= 1
                row["second_closed"][field] -= 1
                self.assertTrue(self.inspect(rows)[1])

    def test_active_close_cannot_invent_work_or_generations(self):
        for fields in (("submitted", "completed"), ("generation", "retired_generations"),
                       ("suspended_frames",), ("waiting_frames",)):
            rows = records()
            row = first(rows, "active_close")
            for name in ("closed", "second_closed"):
                for field in fields:
                    row[name][field] += 1
            with self.subTest(fields=fields):
                errors = self.inspect(rows)[1]
                self.assertTrue(errors)
                self.assertIn("Close performed undeclared GPU work", errors[0])

    def test_sustained_end_cannot_invent_work_after_final_sample(self):
        rows = records()
        invent_sustained_frame(rows, rows.index(first(rows, "sustained_end")))
        errors = self.inspect(rows)[1]
        self.assertTrue(errors)
        self.assertIn("Sustained counters changed after the final drained resource sample", errors[0])

    def test_immediate_resize_restore_sample_cannot_invent_an_extra_frame(self):
        for phase in ("resize", "restore"):
            rows = records()
            invent_sustained_frame(rows, rows.index(first(rows, "sample", phase=phase)))
            with self.subTest(phase=phase):
                errors = self.inspect(rows)[1]
                self.assertTrue(errors)
                self.assertIn("Immediate transition sample performed undeclared GPU work", errors[0])

    def test_resource_maxima_and_final_aggregates_match_every_sample(self):
        for field in ("active_peak", "closed_peak", "active_baseline", "closed_baseline"):
            for resource in stress.PROCESS_FIELDS:
                rows = records()
                rows[-1][field][resource] += 1
                self.assertTrue(self.inspect(rows)[1])
        for field in ("sustained_duration_ms", "sustained_presented_frames", "resize_transitions",
                      "minimize_restore_cycles", "lifetimes", "lifetime_presented_frames", "periodic_samples"):
            rows = records()
            rows[-1][field] += 1
            self.assertTrue(self.inspect(rows)[1])

    def test_malformed_duplicate_nonfinite_and_nonobject_json_fail(self):
        complete = result(records())
        for text in ("", "not-json", "null", "[]", '{"kind":"identity","kind":"identity"}',
                     complete.stdout.replace('"handles": 100', '"handles": NaN', 1)):
            self.assertTrue(stress.inspect_result(replace(complete, stdout=text), "stress", "auto", "required", LOCK, SOURCE, "debug")[1])


if __name__ == "__main__":
    unittest.main()
