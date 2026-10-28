"""CPU controls for the Runtime readback verifier; these qualify no graphics device."""
from copy import deepcopy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import runtime_scene_smoke as smoke
from process_runner import ProcessResult

UUID = "c" * 32


def report(case):
    return {"event": "runtime-scene-complete", "adapter_uuid": UUID, "validation": "required",
            "adapter_name": "Synthetic CPU fixture", "driver_name": "No GPU", "driver_info": "CPU",
            "driver_version": 0, "width": smoke.WIDTH, "height": smoke.HEIGHT, "rendered_frames": 2,
            "draw_count": 2 if case == "fresh" else 1,
            "mesh_instances": 2 if case == "fresh" else 1, "hidden": 0, "culled": 0,
            "scene_instance": 1, "scene_revision": 4, "warnings": 0, "errors": 0, "dropped": 0,
            "presentation": {"submitted": 2, "completed": 2, "presented": 2, "generations": 1,
                             "retired_generations": 1, "in_flight": 0, "closed": True, "faulted": False},
            "capture": {"color": "color.rgba", "depth": "depth.f64", "color_space": "srgb",
                        "subpixel_precision_bits": 8}, "qualification": "NOT_RUN"}


def capture(root, case, value=None):
    reference = smoke.reference(case)
    value = report(case) if value is None else value
    text = json.dumps(value)
    (root / "report.json").write_text(text, encoding="utf-8")
    (root / "color.rgba").write_bytes(reference.color)
    (root / "depth.f64").write_bytes(struct.pack(f"<{len(reference.depth)}d", *reference.depth))
    return ProcessResult(["synthetic-no-GPU"], 0, text, "", False)


class RuntimeSceneSmokeTests(unittest.TestCase):
    def test_independent_geometry_accepts_exact_reference_and_rejects_changed_pixels(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for case in smoke.CASES:
                with self.subTest(case=case):
                    result = capture(root, case)
                    self.assertEqual(smoke.inspect_capture(case, root, UUID, result)[1]["status"], "PASS")
                    reference = smoke.reference(case)
                    pixel = next(index for index, (checked, depth) in
                                 enumerate(zip(reference.checked, reference.depth, strict=True))
                                 if checked and depth < 1)
                    color = bytearray(reference.color)
                    color[pixel * 4] = 0
                    (root / "color.rgba").write_bytes(color)
                    failed = smoke.inspect_capture(case, root, UUID, result)[1]
                    self.assertEqual(failed["status"], "FAIL")
                    self.assertEqual(failed["bad_color_pixels"], 1)
                    (root / "color.rgba").write_bytes(reference.color)
                    depths = list(reference.depth)
                    depths[pixel] += .01
                    (root / "depth.f64").write_bytes(struct.pack(f"<{len(depths)}d", *depths))
                    self.assertEqual(smoke.inspect_capture(case, root, UUID, result)[1]["bad_depth_pixels"], 1)

    def test_device_validation_completion_and_format_fail_closed(self):
        baseline = report("featureworld")
        changes = (("validation", "off"), ("adapter_uuid", "d" * 32), ("warnings", 1),
                   ("rendered_frames", 1), ("culled", 1), ("driver_info", ""))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for key, value in changes:
                changed = deepcopy(baseline)
                changed[key] = value
                result = capture(root, "featureworld", changed)
                with self.subTest(field=key), self.assertRaises(ValueError):
                    smoke.inspect_capture("featureworld", root, UUID, result)
            changed = deepcopy(baseline)
            changed["presentation"]["completed"] = 1
            result = capture(root, "featureworld", changed)
            with self.assertRaises(ValueError):
                smoke.inspect_capture("featureworld", root, UUID, result)
            result = capture(root, "featureworld")
            (root / "depth.f64").write_bytes(b"\0")
            with self.assertRaises(ValueError):
                smoke.inspect_capture("featureworld", root, UUID, result)

    def test_lifecycle_requires_every_phase_failed_candidate_and_retirement(self):
        baseline = report("featureworld")
        reference = smoke.reference("featureworld")
        depths = struct.pack(f"<{len(reference.depth)}d", *reference.depth)
        occupied = sum(value < 1 for value in reference.depth)
        row = {"event": "runtime-scene-lifecycle-complete", "status": "PASS", "adapter_uuid": UUID,
               "validation": "required", "checks": 70, "closed": True, "in_flight": 0,
               "submitted": 7, "completed": 7, "presented": 7, "generations": 3,
               "retired_generations": 3, "warnings": 0, "errors": 0, "dropped": 0,
               "replacement_rejection": {"fault": "window_draw_partial_creation", "fired": True,
                                           "old_frame_preserved": True}, "phases": []}
        row.update({key: baseline[key] for key in ("adapter_name", "driver_name", "driver_info", "driver_version")})
        for name in ("baseline", "replacement-rejected", "destroyed", "reloaded", "resized",
                     "restored", "retained-snapshot"):
            resized, empty = name == "resized", name == "destroyed"
            row["phases"].append({"phase": name, "width": 200 if resized else smoke.WIDTH,
                                  "height": 144 if resized else smoke.HEIGHT, "draws": 0 if empty else 1,
                                  "foreground_pixels": 0 if empty else occupied + int(resized)})

        def inspect(value):
            result = ProcessResult(["synthetic-no-GPU"], 0, json.dumps(value), "", False)
            return smoke.inspect_lifecycle(result, UUID, baseline, depths)

        self.assertEqual(inspect(row), row)
        for change in ("missing-phase", "live-owner", "destroyed-visible", "fault-not-fired"):
            value = deepcopy(row)
            if change == "missing-phase":
                value["phases"].pop()
            elif change == "live-owner":
                value["retired_generations"] = 2
            elif change == "destroyed-visible":
                value["phases"][2]["foreground_pixels"] = 1
            else:
                value["replacement_rejection"]["fired"] = False
            with self.subTest(change=change), self.assertRaises(ValueError):
                inspect(value)


if __name__ == "__main__":
    unittest.main()
