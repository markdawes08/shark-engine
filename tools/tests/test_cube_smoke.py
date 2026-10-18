"""CPU-only window/offscreen protocol and fixed-color evidence tests; no GPU claims."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
from functools import cache
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cube_smoke as smoke
from process_runner import ProcessResult

LOCK, SOURCE, BUNDLE, UUID = "a" * 64, "b" * 64, "c" * 64, "d" * 32


@cache
def expected(target: str, extent: tuple[int, int]):
    return smoke.srgb_reference(extent) if target == "window" else smoke.reference.rasterize(0, extent)


def stats(count: int, generation: int, *, suspended: int = 0, closed: bool = False) -> dict:
    return {"submitted": count, "completed": count, "presented": count, "generation": generation,
            "retired_generations": generation if closed else generation - 1, "suspended_frames": suspended,
            "in_flight": 0, "frame_limit": 2, "active": not closed, "closed": closed, "faulted": False}


def resources(closed: bool = False) -> dict:
    row = dict.fromkeys(smoke.RESOURCE_FIELDS, 0)
    if not closed:
        row.update(live_swapchains=1, window_draw_resources=45, window_draw_submissions=1)
    return row


def records(mode: str = "smoke", validation: str = "required") -> list[dict]:
    rows = [{"kind": "identity", "schema_version": 1, "mode": mode,
             "selection": "" if mode == "describe" else UUID, "validation": validation,
             "dependency_lock_sha256": LOCK, "source_sha256": SOURCE, "compiler": "CPU fixture"}]
    if mode == "describe":
        return [*rows, {"kind": "description", "device_accessed": False, "cases": 3, "qualification": "NOT_RUN"}]
    adapter = {"uuid": UUID, "name": "CPU fixture", "driver_name": "No GPU", "driver_info": "No native libraries",
               "vendor_id": 1, "device_id": 2, "driver_version": 3, "api_version": 4, "queue_family": 0,
               "kind": "discrete", "features": dict.fromkeys(smoke.foundation.FEATURES, True),
               "formats": dict.fromkeys(smoke.foundation.FORMATS, True), "limits": smoke.foundation.LIMITS.copy(),
               "missing_requirements": []}
    rows.append({"kind": "adapter", "adapter": adapter, "bundle_sha256": BUNDLE})
    for index, (width, height) in enumerate(smoke.EXTENTS):
        row = {"kind": "case", "index": index, "stats": stats((index + 1) * 3, index + 1, suspended=int(index > 0)),
               "resources": resources()}
        for offset, target in enumerate(("window", "offscreen")):
            serial = index * 5 + 3 + offset
            row[target] = {"extent": {"width": width, "height": height}, "submitted": serial, "completed": serial,
                           "subpixel_precision_bits": 8, "color_encoding": "srgb" if target == "window" else "linear",
                           "created_resources": 15 if target == "window" else 17,
                           "retired_resources": 0 if target == "window" else 17,
                           "color_file": f"{target}-{index}.rgba8", "depth_file": f"{target}-{index}.d32f"}
        rows.append(row)
        if index == 0:
            rows.append({"kind": "minimized", "before": deepcopy(row["stats"]), "after": stats(3, 1, suspended=1)})
    rows.append({"kind": "complete", "status": "PASS" if validation == "required" else "NOT_RUN",
                 "stats": stats(9, 3, suspended=1, closed=True), "resources": resources(True),
                 "diagnostics": {"warnings": 0, "errors": 0, "dropped": 0, "messages": []}})
    return rows


def process(rows: list[dict]) -> ProcessResult:
    return ProcessResult(["cpu-fixture"], 0, "\n".join(json.dumps(row) for row in rows), "", False)


class CubeSmokeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name)
        for index, extent in enumerate(smoke.EXTENTS):
            for target in ("window", "offscreen"):
                reference = expected(target, extent)
                (self.output / f"{target}-{index}.rgba8").write_bytes(reference.color)
                (self.output / f"{target}-{index}.d32f").write_bytes(
                    struct.pack(f"<{len(reference.depth)}f", *reference.depth))

    def inspect(self, rows=None, *, mode="smoke", validation="required", child=None, pixels=False):
        # Schema mutations need not rerasterize the fixed reference for every
        # mutation. Separate tests exercise actual color/depth comparisons.
        with patch.object(smoke, "srgb_reference", side_effect=lambda extent: expected("window", extent)), \
                patch.object(smoke.reference, "rasterize", side_effect=lambda case, extent: expected("offscreen", extent)):
            if pixels:
                return smoke.inspect_result(child or process(rows or records(mode, validation)), mode,
                                            UUID, validation, LOCK, SOURCE, BUNDLE, self.output)
            with patch.object(smoke.reference, "compare_readback", return_value={"status": "PASS"}):
                return smoke.inspect_result(child or process(rows or records(mode, validation)), mode,
                                            UUID, validation, LOCK, SOURCE, BUNDLE, self.output)

    def test_complete_workload_compares_both_encodings_and_all_six_readbacks(self):
        _, errors, comparisons = self.inspect(pixels=True)
        self.assertEqual(errors, [])
        self.assertEqual(len(comparisons), 6)
        for row in comparisons:
            self.assertEqual(row["status"], "PASS")
            self.assertEqual(row["max_color_error"], 0)
            self.assertLessEqual(row["max_depth_error"], smoke.reference.DEPTH_TOLERANCE)
            self.assertGreaterEqual(row["occlusion_pixels"], smoke.reference.MIN_OCCLUSION_PIXELS)

    def test_description_is_device_free_strict_and_unqualified(self):
        self.assertFalse(self.inspect(mode="describe")[1])
        for field, value in (("device_accessed", True), ("device_accessed", 0), ("cases", 3.0),
                             ("cases", 2), ("qualification", "PASS")):
            rows = records("describe")
            rows[1][field] = value
            with self.subTest(field=field, value=value):
                self.assertTrue(self.inspect(rows, mode="describe")[1])

    def test_validation_off_cannot_claim_qualification(self):
        self.assertFalse(self.inspect(validation="off")[1])
        rows = records(validation="off")
        rows[-1]["status"] = "PASS"
        self.assertTrue(self.inspect(rows, validation="off")[1])

    def test_identity_device_bundle_and_capability_mismatches_reject(self):
        mutations = [lambda r: r[0].update(dependency_lock_sha256="0" * 64),
                     lambda r: r[0].update(source_sha256="0" * 64),
                     lambda r: r[0].update(schema_version=True), lambda r: r[0].update(selection="0" * 32),
                     lambda r: r[1].update(bundle_sha256="0" * 64),
                     lambda r: r[1]["adapter"].update(uuid="0" * 32),
                     lambda r: r[1]["adapter"]["features"].update(vulkan_1_3=False)]
        for index, mutation in enumerate(mutations):
            rows = records()
            mutation(rows)
            with self.subTest(index=index):
                self.assertTrue(self.inspect(rows)[1])

    def test_missing_extra_unordered_and_nonobject_rows_reject(self):
        rows = records()
        for changed in (rows[:-1], [*rows, rows[-1]], [rows[0], rows[2], rows[1], *rows[3:]],
                        [rows[0], [], *rows[2:]]):
            self.assertTrue(self.inspect(changed)[1])
        for index in range(len(rows)):
            changed = deepcopy(rows)
            changed[index]["unknown"] = True
            with self.subTest(index=index):
                self.assertTrue(self.inspect(changed)[1])

    def test_capture_identity_extents_serials_precision_encoding_and_retirement_reject(self):
        for field, value in (("extent", {"width": 160.0, "height": 128}), ("submitted", True), ("completed", 0),
                             ("subpixel_precision_bits", 7), ("color_encoding", "linear"),
                             ("created_resources", 0), ("retired_resources", 17), ("color_file", "../escape.rgba8"),
                             ("depth_file", "offscreen-0.d32f")):
            rows = records()
            rows[2]["window"][field] = value
            with self.subTest(field=field):
                self.assertTrue(self.inspect(rows)[1])
        for field, value in (("created_resources", 16), ("retired_resources", 16),
                             ("color_encoding", "srgb"), ("submitted", 3)):
            rows = records()
            rows[2]["offscreen"][field] = value
            with self.subTest(offscreen=field):
                self.assertTrue(self.inspect(rows)[1])

    def test_lifecycle_counter_types_bounds_and_retirement_reject(self):
        for field, value in (("submitted", 2), ("completed", 4), ("presented", True), ("frame_limit", 3),
                             ("generation", 0), ("retired_generations", 1), ("in_flight", 1),
                             ("active", 1), ("closed", True), ("faulted", True)):
            rows = records()
            rows[2]["stats"][field] = value
            with self.subTest(field=field):
                self.assertTrue(self.inspect(rows)[1])
        for field, value in (("completed", 8), ("in_flight", 1), ("retired_generations", 2), ("active", True)):
            rows = records()
            rows[-1]["stats"][field] = value
            with self.subTest(final=field):
                self.assertTrue(self.inspect(rows)[1])

    def test_resize_and_minimize_must_match_the_actual_case_sequence(self):
        for mutation in (lambda r: r[4]["stats"].update(generation=1, retired_generations=0),
                         lambda r: r[3]["before"].update(completed=2, in_flight=1),
                         lambda r: r[3]["after"].update(suspended_frames=0),
                         lambda r: r[3]["after"].update(completed=2, in_flight=1),
                         lambda r: r[-1]["stats"].update(suspended_frames=0)):
            rows = records()
            mutation(rows)
            self.assertTrue(self.inspect(rows)[1])

    def test_resource_counts_are_bounded_and_zero_after_close(self):
        for field in smoke.RESOURCE_FIELDS:
            rows = records()
            rows[2]["resources"][field] = 4097
            self.assertTrue(self.inspect(rows)[1])
            rows = records()
            rows[-1]["resources"][field] = 1
            self.assertTrue(self.inspect(rows)[1])

    def test_validation_process_failure_and_cleanup_are_never_qualified(self):
        for field in ("warnings", "errors", "dropped"):
            rows = records()
            rows[-1]["diagnostics"][field] = 1
            self.assertTrue(self.inspect(rows)[1])
        good = process(records())
        for changed in (replace(good, returncode=1), replace(good, timed_out=True),
                        replace(good, stderr="validation failure"), replace(good, cleanup_errors=("child survived",))):
            self.assertTrue(self.inspect(child=changed)[1])

    def test_duplicate_keys_nonfinite_numbers_and_oversized_protocol_reject(self):
        good = process(records())
        for text in ('{"kind":"identity","kind":"identity"}', '{"value":NaN}', '{"value":1e999}',
                     " " * (1024 * 1024 + 1), "null", "unexpected diagnostic"):
            self.assertTrue(self.inspect(child=replace(good, stdout=text))[1])

    def test_missing_wrong_sized_and_extra_capture_files_reject(self):
        path = self.output / "window-0.rgba8"
        original = path.read_bytes()
        for data in (original[:-1], original + b"\0"):
            path.write_bytes(data)
            self.assertTrue(self.inspect()[1])
        path.unlink()
        self.assertTrue(self.inspect()[1])
        path.write_bytes(original)
        (self.output / "extra.txt").write_text("unexpected")
        self.assertTrue(self.inspect()[1])

    def test_linked_capture_root_is_rejected_before_reading(self):
        original = Path.is_symlink

        def linked(path):
            return path == self.output or original(path)

        with patch.object(Path, "is_symlink", linked):
            self.assertTrue(self.inspect()[1])

    def test_unknown_inspection_mode_cannot_use_smoke_acceptance(self):
        self.assertTrue(self.inspect(mode="unknown")[1])

    def test_missing_srgb_conversion_wrong_colors_and_invalid_depth_reject(self):
        color_path = self.output / "window-0.rgba8"
        original = color_path.read_bytes()
        color_path.write_bytes(expected("offscreen", smoke.EXTENTS[0]).color)
        self.assertTrue(self.inspect(pixels=True)[1])
        color_path.write_bytes(bytes((0, 0, 0, 255)) * (len(original) // 4))
        self.assertTrue(self.inspect(pixels=True)[1])
        color_path.write_bytes(original)
        depth_path = self.output / "window-0.d32f"
        count = len(original) // 4
        for value in (0., 1., -0.01, 1.01, float("nan"), float("inf")):
            depth_path.write_bytes(struct.pack(f"<{count}f", *([value] * count)))
            self.assertTrue(self.inspect(pixels=True)[1])


class SrgbReferenceTests(unittest.TestCase):
    def test_known_transfer_values_keep_alpha_depth_and_coverage_unchanged(self):
        source = expected("offscreen", smoke.EXTENTS[0])
        pixels = bytes((0, 1, 10, 7, 64, 128, 255, 123))
        with patch.object(smoke.reference, "rasterize", return_value=replace(source, color=pixels)):
            encoded = smoke.srgb_reference(smoke.EXTENTS[0])
        self.assertEqual(encoded.color, bytes((0, 13, 56, 7, 137, 188, 255, 123)))
        self.assertEqual(encoded.depth, source.depth)
        self.assertEqual(encoded.checked, source.checked)
        self.assertEqual(encoded.occlusion, source.occlusion)


if __name__ == "__main__":
    unittest.main()
