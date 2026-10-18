"""Evidence acceptance tests; these need no graphics libraries or device."""

from __future__ import annotations

import json
from contextlib import redirect_stderr, redirect_stdout
import io
from pathlib import Path
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import graphics_probe as probe
from graphics_probe import inspect_result
from process_runner import ProcessResult, run_process


class ProbeEvidenceTests(unittest.TestCase):
    def result(self, *, exit_code=0, status="PASS", **changes):
        report = {"schema_version": 2, "mode": "smoke", "qualification": "NOT_RUN",
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
                       {"mode": "describe"}, {"schema_version": True}, {"schema_version": 1},
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
                     '{"value":NaN}', '{"value":1e999}', "{}{}", ""):
            with self.subTest(data=data):
                self.assertTrue(self.inspect(ProcessResult(["probe"], 0, data, "", False))[1])

    def case_result(self, directory, *, validation="required"):
        report = json.loads(self.result().stdout)
        report["mode"] = "cases"
        report["vulkan_validation_enabled"] = validation == "required"
        report["validation_configuration"] = {"core": validation == "required", "synchronization": validation == "required",
                                                "gpu_assisted": False, "disabled_checks": []}
        report["stages"][-1:-1] = [{"name": name, "status": "PASS"} for name in ("readback_cases", "lifecycle")]
        report["lifecycle"] = {"requested_resize_cycles": 2, "completed_resize_cycles": 2,
                               "requested_device_cycles": 1, "completed_device_cycles": 1,
                               "zero_extent_policy_checks": 2, "observed_zero_framebuffer_extents": 0,
                               "minimize_restore_cycles": 2, "presented_frames": 5,
                               "requested_present_seconds": 0, "present_elapsed_seconds": 0,
                               "fully_qualified": False}
        for case in report["cases"]:
            case["execution_status"] = "NOT_RUN"
            if case["id"] in ("P02-01", "P02-02", "P02-03", "P02-04", "P02-06"):
                case["status"] = "PASS" if validation == "required" else "NOT_RUN"
                case["execution_status"] = "PASS"
            if case["id"] in ("P02-02", "P02-03", "P02-04"):
                case["checks"] = [{"name": name, "status": "PASS", "compared_values": minimum,
                                   "max_absolute_error": 0, "absolute_tolerance": absolute,
                                   "relative_tolerance": relative, "failed_values": 0, "max_tolerance_ratio": 0}
                                  for name, (minimum, absolute, relative) in probe.REQUIRED_CHECKS[case["id"]].items()]
                case["artifacts"] = []
                for name, (format, width, height, layers) in probe.REQUIRED_ARTIFACTS[case["id"]].items():
                    size = width * height * layers * (8 if format == "rgba16f-le" else 4)
                    (directory / name).write_bytes(b"\0" * size)
                    case["artifacts"].append({"path": name, "format": format, "width": width,
                                               "height": height, "layers": layers, "bytes": size})
        return report

    def inspect_cases(self, report, directory, *, validation="required"):
        result = ProcessResult(["probe"], 0, json.dumps(report), "", False)
        return inspect_result(result, "cases", "a" * 64, "b" * 64, "c" * 32,
                              validation, artifacts_dir=directory)

    def test_validated_and_diagnostic_cases_preserve_distinct_qualification(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for validation in ("required", "off"):
                report, errors = self.inspect_cases(self.case_result(directory, validation=validation),
                                                    directory, validation=validation)
                self.assertFalse(errors)
                self.assertEqual(report["qualification"], "NOT_RUN")
                self.assertEqual(report["cases"][4]["status"], "NOT_RUN")
                self.assertEqual(len(report["cases"][1]["verified_artifacts"]), 2)
            forged = self.case_result(directory, validation="off")
            forged["cases"][1]["status"] = "PASS"
            self.assertTrue(self.inspect_cases(forged, directory, validation="off")[1])

    def test_case_pass_requires_measurements_actual_artifacts_and_fixed_tolerances(self):
        changes = ({"compared_values": 0}, {"compared_values": 1}, {"compared_values": 12543},
                   {"compared_values": True}, {"status": "FAIL"},
                   {"max_absolute_error": 1}, {"absolute_tolerance": 0.1}, {"relative_tolerance": 0.1},
                   {"failed_values": 1}, {"max_tolerance_ratio": 1.1})
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for change in changes:
                report = self.case_result(directory)
                report["cases"][1]["checks"][0].update(change)
                with self.subTest(change=change):
                    self.assertTrue(self.inspect_cases(report, directory)[1])
            report = self.case_result(directory)
            (directory / "graphics.rgba8").unlink()
            self.assertTrue(self.inspect_cases(report, directory)[1])

    def test_artifact_escape_duplicate_and_size_mismatch_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for change in ({"path": "../outside"}, {"path": "C:/outside"}, {"path": "..\\outside"},
                           {"bytes": 100}, {"width": True}, {"format": "unrecognized"}):
                report = self.case_result(directory)
                report["cases"][1]["artifacts"][0].update(change)
                with self.subTest(change=change):
                    self.assertTrue(self.inspect_cases(report, directory)[1])
            report = self.case_result(directory)
            report["cases"][1]["artifacts"] *= 2
            self.assertTrue(self.inspect_cases(report, directory)[1])
            report = self.case_result(directory)
            report["cases"][1]["artifacts"][0].update(width=1, height=1, layers=1, bytes=4)
            (directory / "graphics.rgba8").write_bytes(b"\0" * 4)
            self.assertTrue(self.inspect_cases(report, directory)[1])

    def test_depth_and_integer_checks_cannot_use_the_looser_color_tolerance(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for case_index, check_index in ((1, 1), (2, 0)):
                report = self.case_result(directory)
                report["cases"][case_index]["checks"][check_index]["absolute_tolerance"] = 0.0001
                self.assertTrue(self.inspect_cases(report, directory)[1])

    def test_partial_lifecycle_and_single_run_cannot_close_full_gate(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for index in (4, 6, 7):
                report = self.case_result(directory)
                report["cases"][index]["status"] = "PASS"
                self.assertTrue(self.inspect_cases(report, directory)[1])
            for field, value in (("fully_qualified", True), ("completed_resize_cycles", 1),
                                 ("present_elapsed_seconds", -1), ("completed_device_cycles", True),
                                 ("minimize_restore_cycles", 0), ("minimize_restore_cycles", 1),
                                 ("minimize_restore_cycles", 3), ("observed_zero_framebuffer_extents", 3),
                                 ("presented_frames", 0), ("presented_frames", 4)):
                report = self.case_result(directory)
                report["lifecycle"][field] = value
                self.assertTrue(self.inspect_cases(report, directory)[1])

    def test_deadline_terminates_a_real_nonresponsive_process(self):
        started = time.monotonic()
        result = run_process([sys.executable, "-c", "import time; time.sleep(60)"], timeout=0.1)
        self.assertTrue(result.timed_out)
        self.assertEqual(result.returncode, 124)
        self.assertFalse(result.cleanup_errors)
        self.assertLess(time.monotonic() - started, 8)

    def test_timed_presentation_requires_an_observed_extra_frame(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            report = self.case_result(directory)
            report["lifecycle"].update(requested_present_seconds=1, present_elapsed_seconds=1)
            for frames, valid in ((5, False), (6, True)):
                report["lifecycle"]["presented_frames"] = frames
                result = ProcessResult(["probe"], 0, json.dumps(report), "", False)
                errors = inspect_result(result, "cases", "a" * 64, "b" * 64, "c" * 32,
                                        artifacts_dir=directory, present_seconds=1)[1]
                self.assertEqual(not errors, valid)

    def test_layer_environment_removes_ambient_validation_overrides(self):
        with patch.dict(probe.os.environ, {"VK_LAYER_PATH": "untrusted", "VK_LAYER_DISABLES": "all",
                                          "VK_LOADER_LAYERS_ENABLE": "all", "VK_LAYER_MESSAGE_ID_FILTER": "hide-errors",
                                          "VK_KHRONOS_VALIDATION_VALIDATE_SYNC": "false",
                                          "VK_IMPLICIT_LAYER_PATH": "ambient", "VK_ADD_IMPLICIT_LAYER_PATH": "overlay"}):
            environment, evidence = probe.layer_environment({"validation_layer": {"manifest_path": "/pinned/layer.json"}},
                                                             Path("/owned/vk_layer_settings.txt"), Path("/owned/empty-layers"))
        self.assertNotIn("VK_LAYER_DISABLES", environment)
        self.assertNotIn("VK_LOADER_LAYERS_ENABLE", environment)
        self.assertNotIn("VK_LAYER_MESSAGE_ID_FILTER", environment)
        self.assertNotIn("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", environment)
        self.assertNotIn("VK_ADD_IMPLICIT_LAYER_PATH", environment)
        self.assertEqual(environment["VK_LAYER_SETTINGS_PATH"], str(Path("/owned/vk_layer_settings.txt")))
        self.assertEqual(environment["VK_LAYER_PATH"], str(Path("/pinned")))
        self.assertNotIn("VK_LOADER_LAYERS_DISABLE", environment)
        self.assertEqual(environment["VK_IMPLICIT_LAYER_PATH"], str(Path("/owned/empty-layers")))
        self.assertIn("VK_LAYER_PATH", evidence["removed_ambient_keys"])

    def validation_fixture(self, directory):
        validator = directory / "spirv-val"
        validator.write_bytes(b"fixture validator")
        library = directory / "layer.dll"
        library.write_bytes(b"fixture layer")
        declaration = directory / "layer.json"
        declaration.write_text(json.dumps({"layer": {"name": "VK_LAYER_KHRONOS_validation",
                                                       "api_version": "1.4.352", "library_path": "layer.dll"}}))
        manifest = {"schema_version": 1, "status": "PASS", "dependency_lock_sha256": "a" * 64,
                    "tools": {"spirv_val": {"path": str(validator), "sha256": probe.sha256(validator)}},
                    "validation_layer": {"name": "VK_LAYER_KHRONOS_validation", "api_version": "1.4.352",
                                         "manifest_path": str(declaration), "manifest_sha256": probe.sha256(declaration),
                                         "library_path": str(library), "library_sha256": probe.sha256(library)}}
        receipt = directory / "receipt.json"
        receipt.write_text(json.dumps(manifest))
        return receipt, manifest

    def test_validation_receipt_binds_actual_tools_layer_declaration_and_current_lock(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            receipt, manifest = self.validation_fixture(directory)
            self.assertEqual(probe.validation_tools(receipt, "a" * 64), manifest)
            with self.assertRaisesRegex(ValueError, "current"):
                probe.validation_tools(receipt, "b" * 64)
            (directory / "layer.dll").write_bytes(b"changed layer")
            with self.assertRaisesRegex(ValueError, "changed"):
                probe.validation_tools(receipt, "a" * 64)

    def test_validation_receipt_cannot_redirect_loader_to_unrecorded_library(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            receipt, manifest = self.validation_fixture(directory)
            declaration = directory / "layer.json"
            data = json.loads(declaration.read_text())
            data["layer"]["library_path"] = "other.dll"
            declaration.write_text(json.dumps(data))
            manifest["validation_layer"]["manifest_sha256"] = probe.sha256(declaration)
            receipt.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "disagrees"):
                probe.validation_tools(receipt, "a" * 64)

    def shader_fixture(self, directory):
        shaders = directory / "prepared"
        shaders.mkdir()
        source = directory / "probes/graphics/shaders/cases.hlsl"
        source.parent.mkdir(parents=True)
        source.write_text("fixture source")
        tool = directory / "tool"
        tool.write_bytes(b"fixture executable")
        records = []
        for name, (entry, profile) in probe.SHADERS.items():
            binary = shaders / name
            binary.write_bytes(b"x" * 20)
            records.append({"file": name, "sha256": probe.sha256(binary), "bytes": 20,
                            "source": "probes/graphics/shaders/cases.hlsl", "source_sha256": probe.sha256(source),
                            "entry": entry, "profile": profile, "compile_status": "PASS", "validation_status": "PASS"})
        manifest = {"schema_version": 1, "status": "PASS", "dependency_lock_sha256": "a" * 64,
                    "invalid_shader_rejected": True, "invalid_spirv_rejected": True, "shaders": records,
                    "compiler": {"path": str(tool), "sha256": probe.sha256(tool)},
                    "validator": {"path": str(tool), "sha256": probe.sha256(tool)}}
        (shaders / "shader-manifest.json").write_text(json.dumps(manifest))
        return shaders, source, manifest

    def test_shader_manifest_rejects_changed_spirv_sources_tools_and_stale_lock(self):
        for changed in ("spirv", "source", "tool", "lock"):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                shaders, source, manifest = self.shader_fixture(directory)
                with patch.object(probe, "ROOT", directory):
                    self.assertEqual(len(probe.shader_inputs(shaders, "a" * 64)["shaders"]), 4)
                    if changed == "spirv":
                        (shaders / "fixture_vs.spv").write_bytes(b"changed")
                    elif changed == "source":
                        source.write_text("changed")
                    elif changed == "tool":
                        (directory / "tool").write_bytes(b"changed")
                    with self.assertRaises(ValueError):
                        probe.shader_inputs(shaders, ("b" if changed == "lock" else "a") * 64)

    def test_shader_manifest_requires_all_independently_validated_fixtures(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            shaders, _, manifest = self.shader_fixture(directory)
            manifest["shaders"][0]["validation_status"] = "NOT_RUN"
            (shaders / "shader-manifest.json").write_text(json.dumps(manifest))
            with patch.object(probe, "ROOT", directory), self.assertRaisesRegex(ValueError, "validation evidence"):
                probe.shader_inputs(shaders, "a" * 64)

    def test_required_cases_without_pinned_layer_are_unavailable_before_launch(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            shaders, _, manifest = self.shader_fixture(directory)
            lock = directory / "dependencies.lock.json"
            lock.write_text("{}")
            manifest["dependency_lock_sha256"] = probe.sha256(lock)
            (shaders / "shader-manifest.json").write_text(json.dumps(manifest))
            output = directory / "evidence"
            command = ["graphics_probe.py", "--executable", str(directory / "tool"), "--mode", "cases",
                       "--device", "c" * 32, "--shader-dir", str(shaders), "--output", str(output)]
            with patch.object(probe, "ROOT", directory), patch.object(sys, "argv", command), \
                    patch.object(probe, "source_state", return_value={}), \
                    patch.object(probe, "run_process", side_effect=AssertionError("No GPU process allowed")), \
                    redirect_stdout(io.StringIO()):
                self.assertEqual(probe.main(), 3)
            summary = json.loads((output / "summary.json").read_text())
            self.assertEqual(summary["status"], "NOT_RUN")
            self.assertNotIn("process_exit_code", summary)

    def test_existing_runner_evidence_is_preserved_before_any_process_launch(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable = directory / "probe"
            executable.write_bytes(b"fixture executable")
            previous = directory / "summary.json"
            previous.write_text("preserve previous evidence")
            command = ["graphics_probe.py", "--executable", str(executable), "--mode", "describe", "--output", str(directory)]
            with patch.object(sys, "argv", command), \
                    patch.object(probe, "run_process", side_effect=AssertionError("No process allowed")), \
                    redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as result:
                probe.main()
            self.assertEqual(result.exception.code, 2)
            self.assertEqual(previous.read_text(), "preserve previous evidence")

    def test_high_integrity_validation_is_unavailable_with_standard_user_guidance(self):
        for integrity, available in ((0x2000, True), (0x3000, False), (0x4000, False)):
            security = {"integrity_rid": integrity, "elevated": not available}
            helper = SimpleNamespace(process_security=lambda: security)
            with patch.object(probe.sys, "platform", "win32"), patch.dict(sys.modules, {"windows_unelevated": helper}):
                if available:
                    self.assertEqual(probe.check_layer_process(), security)
                else:
                    with self.assertRaisesRegex(probe.Unavailable, "normal terminal.*unelevated wrapper"):
                        probe.check_layer_process()


if __name__ == "__main__":
    unittest.main()
