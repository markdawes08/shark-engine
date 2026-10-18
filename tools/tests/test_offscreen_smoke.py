"""CPU-only oracle and rejection tests; these synthetic readbacks qualify no GPU."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import offscreen_smoke as smoke
from process_runner import ProcessResult

LOCK, SOURCE, UUID = "a" * 64, "b" * 64, "c" * 32


def adapter() -> dict:
    return {"uuid": UUID, "name": "CPU evidence fixture", "driver_name": "No GPU",
            "driver_info": "No native libraries", "vendor_id": 1, "device_id": 2,
            "driver_version": 3, "api_version": 4, "queue_family": 0, "kind": "discrete",
            "features": dict.fromkeys(smoke.foundation.FEATURES, True),
            "formats": dict.fromkeys(smoke.foundation.FORMATS, True),
            "limits": smoke.foundation.LIMITS.copy(), "missing_requirements": []}


def records(mode="render", validation="required") -> list[dict]:
    rows = [{"kind": "identity", "schema_version": 1, "mode": mode,
             "dependency_lock_sha256": LOCK, "source_sha256": SOURCE, "compiler": "CPU fixture",
             "validation": validation, "selection": UUID if mode == "render" else "",
             "assignment_06": "NOT_RUN"}]
    if mode == "describe":
        return [*rows, {"kind": "description", "status": "PASS", "qualification": "NOT_RUN",
                       "device_accessed": False, "cases": 2, "native_graphics_available": False,
                       "scope": "CPU protocol fixture only"}]
    rows.append({"kind": "adapter", "adapter": adapter()})
    for case in range(2):
        rows.append({"kind": "case", **smoke.fixture(case), "submitted": (case + 1) * 3,
                     "completed": (case + 1) * 3, "created_resources": 17, "retired_resources": 17,
                     "subpixel_precision_bits": 8})
    status = "PASS" if validation == "required" else "NOT_RUN"
    rows.append({"kind": "complete", "status": status, "qualification": status, "execution_status": "PASS",
                 "cases": 2, "assignment_06": "NOT_RUN",
                 "diagnostics": {"warnings": 0, "errors": 0, "dropped": 0, "messages": []}})
    return rows


def result(rows: list[dict]) -> ProcessResult:
    return ProcessResult(["cpu-fixture"], 0, "\n".join(json.dumps(row) for row in rows), "", False)


class OffscreenEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name)
        for case in range(2):
            expected, reference = smoke.fixture(case), smoke.rasterize(case)
            (self.output / expected["color_file"]).write_bytes(reference.color)
            (self.output / expected["depth_file"]).write_bytes(struct.pack(f"<{len(reference.depth)}f", *reference.depth))

    def inspect(self, rows=None, *, mode="render", validation="required", process=None):
        return smoke.inspect_result(process or result(rows or records(mode, validation)), mode,
                                    UUID if mode == "render" else "auto", validation, LOCK, SOURCE, self.output)

    def assertRejects(self, rows):
        self.assertTrue(self.inspect(rows)[1])

    def test_complete_fixed_workload_and_noncontiguous_serials_accept(self):
        _, errors, comparisons = self.inspect()
        self.assertEqual(errors, [])
        self.assertEqual([row["status"] for row in comparisons], ["PASS", "PASS"])
        for row in comparisons:
            self.assertGreaterEqual(row["occlusion_pixels"], 50)
            self.assertEqual(row["max_color_error"], 0)
            self.assertLessEqual(row["max_depth_error"], smoke.DEPTH_TOLERANCE)

    def test_description_is_device_free_and_cannot_claim_qualification(self):
        self.assertFalse(self.inspect(mode="describe")[1])
        for field, value in (("device_accessed", True), ("qualification", "PASS"), ("cases", 1)):
            rows = records("describe")
            rows[1][field] = value
            self.assertTrue(self.inspect(rows, mode="describe")[1])

    def test_validation_off_is_diagnostic_only(self):
        self.assertFalse(self.inspect(validation="off")[1])
        rows = records(validation="off")
        rows[-1]["qualification"] = "PASS"
        self.assertTrue(self.inspect(rows, validation="off")[1])

    def test_stale_lock_source_selection_or_full_assignment_pass_reject(self):
        for field, value in (("dependency_lock_sha256", "0" * 64), ("source_sha256", "0" * 64),
                             ("selection", "0" * 32), ("assignment_06", "PASS"), ("schema_version", True)):
            rows = records()
            rows[0][field] = value
            self.assertRejects(rows)

    def test_wrong_gpu_or_missing_capability_reject(self):
        for mutation in (lambda row: row.update(uuid="0" * 32),
                         lambda row: row["features"].update(vulkan_1_3=False),
                         lambda row: row.update(kind="software")):
            rows = records()
            mutation(rows[1]["adapter"])
            self.assertRejects(rows)

    def test_missing_extra_reordered_or_nonobject_records_reject(self):
        original = records()
        for rows in (original[:-1], original + [original[-1]],
                     [original[0], original[2], original[1], *original[3:]],
                     [original[0], [], *original[2:]]):
            self.assertRejects(rows)

    def test_changed_fixed_geometry_cannot_self_define_oracle(self):
        for field, value in (("vertices", [[0, 0, .5, 0, 0]] * 48),
                             ("indices", list(reversed(smoke.fixture(0)["indices"]))),
                             ("transform", [1] * 16), ("texture_rgba8", [0] * 16),
                             ("extent", {"width": 128, "height": 128}), ("color_file", "../escape.rgba8")):
            rows = records()
            rows[2][field] = value
            self.assertRejects(rows)

    def test_float32_rounding_accepts_but_changed_pose_rejects(self):
        rows = records()
        rows[2]["vertices"][0][0] = struct.unpack("<f", struct.pack("<f", -.6))[0]
        self.assertFalse(self.inspect(rows)[1])
        rows[2]["vertices"][0][0] += .001
        self.assertRejects(rows)

    def test_integer_fields_reject_booleans_and_fractional_values(self):
        for field, value in (("submitted", True), ("completed", 3.0),
                             ("created_resources", 17.0), ("retired_resources", False)):
            rows = records()
            rows[2][field] = value
            self.assertRejects(rows)
        rows = records()
        rows[2]["indices"][0] = True
        self.assertRejects(rows)

    def test_submission_completion_and_retirement_failures_reject(self):
        for field, value in (("submitted", 0), ("completed", 2),
                             ("created_resources", 16), ("retired_resources", 16)):
            rows = records()
            rows[2][field] = value
            self.assertRejects(rows)
        rows = records()
        rows[3].update(submitted=3, completed=3)
        self.assertRejects(rows)

    def test_unknown_case_identity_completion_fields_reject(self):
        for index in (0, 2, -1):
            rows = records()
            rows[index]["surprise"] = 1
            self.assertRejects(rows)

    def test_diagnostics_never_silently_qualify(self):
        for key in ("warnings", "errors", "dropped"):
            rows = records()
            rows[-1]["diagnostics"][key] = 1
            self.assertRejects(rows)

    def test_process_failure_timeout_stderr_cleanup_reject(self):
        original = result(records())
        for process in (replace(original, returncode=1), replace(original, timed_out=True),
                        replace(original, stderr="validation error"), replace(original, cleanup_errors=("child survived",))):
            self.assertTrue(self.inspect(process=process)[1])

    def test_duplicate_keys_nonfinite_and_oversized_output_reject(self):
        original = result(records())
        for stdout in ('{"kind":"identity","kind":"identity"}', '{"value":NaN}', '{"value":1e999}', " " * (1024 * 1024 + 1)):
            self.assertTrue(self.inspect(process=replace(original, stdout=stdout))[1])

    def test_readback_missing_truncated_oversized_or_extra_file_reject(self):
        path = self.output / smoke.fixture(0)["color_file"]
        saved = path.read_bytes()
        for data in (saved[:-1], saved + b"\0"):
            path.write_bytes(data)
            self.assertTrue(self.inspect()[1])
        path.unlink()
        self.assertTrue(self.inspect()[1])
        path.write_bytes(saved)
        (self.output / "unexpected.txt").write_text("not part of the workload")
        self.assertTrue(self.inspect()[1])

    def test_blank_color_srgb_channel_swap_and_y_flip_reject(self):
        expected = smoke.fixture(0)
        path = self.output / expected["color_file"]
        original = path.read_bytes()
        width, height = smoke.EXTENTS[0]
        swapped = bytearray(original)
        for start in range(0, len(swapped), 4):
            swapped[start], swapped[start + 2] = swapped[start + 2], swapped[start]
        transformed = bytearray(original)
        for start in range(0, len(transformed), 4):
            for channel in range(3):
                value = transformed[start + channel] / 255
                transformed[start + channel] = round(255 * (12.92 * value if value <= .0031308 else 1.055 * value ** (1 / 2.4) - .055))
        flipped = b"".join(original[y * width * 4:(y + 1) * width * 4] for y in reversed(range(height)))
        for data in (bytes((0, 0, 0, 255)) * (width * height), swapped, transformed, flipped):
            path.write_bytes(data)
            self.assertTrue(self.inspect()[1])

    def test_depth_clear_zero_nan_and_wrong_compare_reject(self):
        path = self.output / smoke.fixture(0)["depth_file"]
        count = smoke.EXTENTS[0][0] * smoke.EXTENTS[0][1]
        for value in (0., 1., float("nan")):
            path.write_bytes(struct.pack(f"<{count}f", *([value] * count)))
            self.assertTrue(self.inspect()[1])

    def test_exact_tolerance_and_excluded_edge_are_bounded(self):
        reference = smoke.rasterize(0)
        checked = next(index for index, value in enumerate(reference.checked) if value)
        skipped = next(index for index, value in enumerate(reference.checked) if not value)
        color = bytearray(reference.color)
        depths = struct.pack(f"<{len(reference.depth)}f", *reference.depth)
        color[checked * 4] = 1  # This checked background channel was zero.
        color[skipped * 4] = 127
        self.assertEqual(smoke.compare_readback(color, depths, reference)["status"], "PASS")
        color[checked * 4] = 2
        self.assertEqual(smoke.compare_readback(color, depths, reference)["status"], "FAIL")

    def test_analytic_projection_and_barycentric_orientation(self):
        identity = [1., 0, 0, 0, 0, 1., 0, 0, 0, 0, 1., 0, 0, 0, 0, 1.]
        self.assertEqual(smoke.project([-.5, .5, .25, .2, .7], identity, 100, 80),
                         (25., 20., .25, 1., .2, .7))
        a, b, c, point = (0, 0), (0, 4), (4, 0), (1, 1)
        area = smoke.edge(a, b, c)
        self.assertEqual(area, -16)
        self.assertEqual([smoke.edge(b, c, point) / area, smoke.edge(c, a, point) / area,
                          smoke.edge(a, b, point) / area], [.5, .25, .25])

    def test_subpixel_quantization_precedes_depth_interpolation(self):
        vertices = ((.001, .001, .25), (.001, 4.001, .5), (4.001, .001, .75))
        snapped = [smoke.snap_vertex(vertex) for vertex in vertices]
        self.assertEqual(snapped, [(0., 0., .25), (0., 4., .5), (4., 0., .75)])

        def depth(points):
            a, b, c = points
            area = smoke.edge(a, b, c)
            weights = (smoke.edge(b, c, (1, 1)) / area, smoke.edge(c, a, (1, 1)) / area,
                       smoke.edge(a, b, (1, 1)) / area)
            return sum(weight * vertex[2] for weight, vertex in zip(weights, points, strict=True))

        self.assertEqual(depth(snapped), .4375)
        self.assertGreater(abs(depth(vertices) - .4375), smoke.DEPTH_TOLERANCE)

    def test_unsupported_subpixel_profile_is_not_run_without_autofit(self):
        rows = records()
        for row in rows[2:4]:
            row["subpixel_precision_bits"] = 9
        _, errors, comparisons = self.inspect(rows)
        self.assertFalse(errors)
        self.assertEqual([row["status"] for row in comparisons], ["NOT_RUN", "NOT_RUN"])
        for invalid in (True, 0, 3, 33, 8.0):
            rows = records()
            rows[2]["subpixel_precision_bits"] = invalid
            self.assertRejects(rows)

    def test_cube_face_normals_and_far_offset_independent_of_projection(self):
        data = smoke.fixture(0)
        for face in range(6):
            a, b, c = (data["vertices"][face * 4 + offset] for offset in (0, 1, 2))
            ab, ac = [b[i] - a[i] for i in range(3)], [c[i] - a[i] for i in range(3)]
            normal = [ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0]]
            self.assertGreater(sum(normal[i] * a[i] for i in range(3)), 0)
        for index in range(24):
            near = smoke.project(data["vertices"][index], data["transform"], 96, 80)
            far = smoke.project(data["vertices"][index + 24], data["transform"], 96, 80)
            self.assertAlmostEqual(far[0], 48 + (near[0] - 48) * .65)
            self.assertAlmostEqual(far[1], 40 + (near[1] - 40) * .65)
            self.assertAlmostEqual(far[2], .5 + (near[2] - .5) * .65 + .2)

    def test_png_encodes_exact_top_left_rgba_and_valid_crcs(self):
        pixels = bytes((255, 0, 0, 255, 0, 0, 255, 255))
        path = self.output / "test.png"
        smoke.write_png(path, 1, 2, pixels)
        data = path.read_bytes()
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        cursor, payloads = 8, {}
        while cursor < len(data):
            length = struct.unpack_from(">I", data, cursor)[0]
            kind, payload = data[cursor + 4:cursor + 8], data[cursor + 8:cursor + 8 + length]
            self.assertEqual(struct.unpack_from(">I", data, cursor + 8 + length)[0], zlib.crc32(kind + payload))
            payloads[kind] = payload
            cursor += length + 12
        self.assertEqual(zlib.decompress(payloads[b"IDAT"]), b"\0" + pixels[:4] + b"\0" + pixels[4:])

    def test_shader_generation_requires_current_recipe_lock_sources_and_builder(self):
        recipe = smoke.shader_pipeline.recipe(smoke.ROOT / "shaders/recipe.json")
        sources = smoke.shader_pipeline.source_closure(smoke.ROOT / "shaders", recipe)
        manifest = {"inputs": {"recipe": recipe,
                               "toolchain": {"lock_sha256": smoke.sha256(smoke.ROOT / "dependencies.lock.json")},
                               "pipeline_sha256": smoke.sha256(smoke.ROOT / "tools/shader_pipeline.py"),
                               "sources": {name: {"sha256": smoke.shader_pipeline.digest(data), "bytes": len(data)}
                                           for name, data in sources.items()}},
                    "shaders": [{"id": name, "file": name + ".spv"} for name in ("textured-cube-vs", "textured-cube-ps")]}
        with patch.object(smoke.shader_pipeline, "verify", return_value=manifest):
            self.assertEqual(smoke.shader_inputs(self.output)[0], manifest)
        for field in ("recipe", "toolchain", "pipeline_sha256", "sources"):
            changed = deepcopy(manifest)
            changed["inputs"][field] = {} if field != "pipeline_sha256" else "0" * 64
            if field == "toolchain":
                changed["inputs"][field] = {"lock_sha256": "0" * 64}
            with patch.object(smoke.shader_pipeline, "verify", return_value=changed), self.assertRaises(ValueError):
                smoke.shader_inputs(self.output)


if __name__ == "__main__":
    unittest.main()
