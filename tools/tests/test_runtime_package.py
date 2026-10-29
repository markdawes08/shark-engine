"""CPU-only Runtime deployment manifest, closure and completion rejection tests."""

from __future__ import annotations

import json
import math
import os
from pathlib import Path
import sys
import tempfile
import unittest
import copy
import hashlib
import struct
import uuid
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_runtime as package
import shader_pipeline as pipeline
from process_runner import ProcessResult


class RuntimePackageTests(unittest.TestCase):
    def test_runtime_crt_addition_keeps_the_diagnostic_probe_selection_unchanged(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            hashes = {}
            for name in package.RUNTIME_CRT_NAMES:
                binary = directory / name
                binary.write_bytes(b"synthetic CRT fixture " + name.encode("ascii"))
                hashes[name] = package.sha256(binary)
            notice = directory / "NOTICE.txt"
            notice.write_bytes(b"synthetic redistributable notice")
            original = {"version": "fixture", "architecture": "x86_64",
                        "files": {name: hashes[name] for name in package.CRT_NAMES},
                        "runtime_additional_files": {"msvcp140_2.dll": hashes["msvcp140_2.dll"]},
                        "license_files": [{"path": "NOTICE.txt", "destination": "NOTICE.txt",
                                           "sha256": package.sha256(notice)}]}
            snapshot = copy.deepcopy(original)
            merged = package.runtime_crt_manifest(original)
            self.assertEqual(original, snapshot)
            with patch("package_graphics_probe.ROOT", directory):
                probe_binaries, _, _ = package.crt_inputs(directory, original)
                self.assertEqual(set(probe_binaries), {"bin/" + name for name in package.CRT_NAMES})
                self.assertNotIn("bin/msvcp140_2.dll", probe_binaries)
                runtime_binaries, _, _ = package.crt_inputs(directory, merged, required_names=package.RUNTIME_CRT_NAMES)
                self.assertEqual(set(runtime_binaries), {"bin/" + name for name in package.RUNTIME_CRT_NAMES})
                with self.assertRaisesRegex(ValueError, "exact selected"):
                    package.crt_inputs(directory, merged)
                (directory / "msvcp140_2.dll").write_bytes(b"changed")
                with self.assertRaisesRegex(ValueError, "changed"):
                    package.crt_inputs(directory, merged, required_names=package.RUNTIME_CRT_NAMES)
                package.crt_inputs(directory, original)
            for additional in ({}, {"unreviewed.dll": "a" * 64}, None):
                with self.subTest(additional=additional), self.assertRaisesRegex(ValueError, "exact base"):
                    package.runtime_crt_manifest({**original, "runtime_additional_files": additional})

    def test_production_notices_include_core_headers_and_bundled_tls_from_verified_caches(self):
        lock = {"dependencies": {name: {"url": f"https://example.invalid/{name}.tar.gz",
                                       "sha256": name, "license": "MIT"}
                                  for name in ("EnTT", "GLM", "MbedTLS", "GLFW", "NVRHI", "VulkanHeaders", "VulkanLoader")}}
        tls_notices = {name: b"tls license " + name.encode("utf-8")
                       for name in package.required_core_notices()["MbedTLS"]}
        lock["dependencies"]["MbedTLS"]["license_files"] = list(tls_notices)
        local_archive = Path("local cache/entt.tar.gz")
        glm_archive = Path("local cache/glm.tar.gz")
        tls_archive = Path("local cache/mbedtls-4.1.1.tar.bz2")
        with patch.object(package, "cache_values", return_value={"SHARK_EnTT_ARCHIVE": str(local_archive),
                                                                "SHARK_GLM_ARCHIVE_INPUT": str(glm_archive),
                                                                "SHARK_MbedTLS_ARCHIVE_INPUT": str(tls_archive)}), \
                patch.object(package, "archive_notices", side_effect=lambda archive, entry: {
                    "copying.txt" if entry["sha256"] == "GLM" else "LICENSE": b"license"}) as notices, \
                patch.object(package, "core_archive_files", return_value={
                    **tls_notices, "tests/test-server.key": b"must not be packaged"}) as tls_files:
            payload, records = package.production_notices(Path("build"), lock)
        self.assertIn("licenses/EnTT/LICENSE", payload)
        self.assertEqual(records["EnTT"]["archive_sha256"], "EnTT")
        self.assertEqual(notices.call_args_list[0].args[0], local_archive)
        self.assertEqual(notices.call_args_list[1].args[0], glm_archive)
        self.assertIn("licenses/GLM/copying.txt", payload)
        self.assertEqual(records["GLM"]["archive_sha256"], "GLM")
        tls_files.assert_called_once_with(tls_archive, lock["dependencies"]["MbedTLS"], "MbedTLS")
        self.assertEqual({name.removeprefix("licenses/MbedTLS/"): data for name, data in payload.items()
                          if name.startswith("licenses/MbedTLS/")}, tls_notices)
        self.assertEqual(records["MbedTLS"]["license_files"],
                         {name: package.digest(data) for name, data in tls_notices.items()})
        self.assertNotIn("licenses/MbedTLS/tests/test-server.key", payload)

    def test_missing_manifested_core_dependency_license_is_rejected(self):
        for name, notices in package.required_core_notices().items():
            for notice in notices:
                with self.subTest(name=name, notice=notice):
                    self.missing_license(name, notice)

    def missing_license(self, name, notice):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "package"
            payload = self.payload()
            del payload[f"licenses/{name}/{notice}"]
            package.publish(directory, payload, {"schema_version": 1, "kind": "runtime-deployment-fixture",
                                                 "status": "PASS", "qualification": "NOT_RUN"})
            with self.assertRaisesRegex(ValueError, name + " license notices"):
                package.inspect_package(directory, package.sha256(directory / "package-manifest.json"))

    def test_build_freshness_allows_only_the_always_run_boundary_check(self):
        self.assertTrue(package.current_build("ninja: no work to do.\n"))
        self.assertTrue(package.current_build("ninja: Entering directory `build/runtime-only'\n"
                                              "[1/1] Checking client transitive dependency boundaries\n"))
        for text in ("", "ninja: Entering directory `build/runtime-only'\n",
                     "[1/2] Building CXX object engine/client.obj\n[2/2] Linking Runtime.exe\n",
                     "[1/1] Re-running CMake...\n", "[1/1] Unknown future build step\n"):
            with self.subTest(text=text):
                self.assertFalse(package.current_build(text))

    def cache(self, directory, **changes):
        values = {"CMAKE_BUILD_TYPE": "Release", "SHARK_SANITIZER": "none",
                  "SHARK_BUILD_CLIENT": "ON", "SHARK_BUILD_EDITOR": "OFF", "SHARK_BUILD_TOOLS": "OFF",
                  "SHARK_BUILD_TESTS": "OFF", "SHARK_BUILD_SERVER": "OFF",
                  "SHARK_BUILD_GRAPHICS_PROBE": "OFF", "SHARK_NATIVE_WINDOW": "ON",
                  "SHARK_NATIVE_GRAPHICS": "ON", "SHARK_INJECT_TEST_FAILURE": "OFF",
                  "CMAKE_HOME_DIRECTORY": str(package.ROOT)}
        values.update(changes)
        (directory / "CMakeCache.txt").write_text(
            "".join(f"{key}:STRING={value}\n" for key, value in values.items()), encoding="utf-8")

    def test_only_protected_runtime_release_cache_is_accepted(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.cache(directory)
            self.assertEqual(package.cache_values(directory)["SHARK_BUILD_EDITOR"], "OFF")
            for key, value in (("CMAKE_BUILD_TYPE", "Debug"), ("SHARK_SANITIZER", "address"),
                               ("SHARK_BUILD_EDITOR", "ON"), ("SHARK_BUILD_TOOLS", "ON"), ("SHARK_BUILD_TESTS", "ON"),
                               ("SHARK_BUILD_SERVER", "ON"), ("SHARK_BUILD_GRAPHICS_PROBE", "ON"),
                               ("SHARK_NATIVE_GRAPHICS", "OFF"), ("SHARK_INJECT_TEST_FAILURE", "ON")):
                with self.subTest(key=key):
                    self.cache(directory, **{key: value})
                    with self.assertRaisesRegex(ValueError, key):
                        package.cache_values(directory)

    def test_different_source_checkout_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.cache(directory, CMAKE_HOME_DIRECTORY=str(directory))
            with self.assertRaisesRegex(ValueError, "different source"):
                package.cache_values(directory)

    def test_storage_population_and_manifested_storage_payloads_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.cache(directory)
            (directory / "_deps/sqlite-src").mkdir(parents=True)
            with self.assertRaisesRegex(ValueError, "SQLite population"):
                package.cache_values(directory)
        for name in ("licenses/SQLite/LICENSE.txt", "licenses/sQLite/LICENSE.txt", "bin/sqlite3.dll", "data/world.db"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary) / "package"
                package.publish(directory, {**self.payload(), name: b"storage"},
                                {"schema_version": 1, "kind": "runtime-deployment-fixture", "status": "PASS", "qualification": "NOT_RUN"})
                with self.assertRaisesRegex(ValueError, "SQLite|unexpected|Unexpected"):
                    package.inspect_package(directory, package.sha256(directory / "package-manifest.json"))

    def payload(self):
        files = {name: b"synthetic CPU fixture" for name in package.BINARIES}
        for name, notices in package.required_core_notices().items():
            for notice in notices:
                files[f"licenses/{name}/{notice}"] = b"synthetic license fixture"
        for name in ("GLFW", "NVRHI", "VulkanHeaders", "VulkanLoader", "MicrosoftVC"):
            files[f"licenses/{name}/LICENSE.txt"] = b"synthetic license fixture"
        return files

    def test_manifested_tls_shared_libraries_and_test_credentials_are_rejected(self):
        for name in ("bin/mbedtls.dll", "bin/tfpsacrypto.dll", "tests/test-server.key",
                     "licenses/MbedTLS/test-server.key"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "package"
                files = self.payload()
                files[name] = b"not a permitted deployment payload"
                package.publish(output, files, {"schema_version": 1, "kind": "runtime-deployment-fixture",
                                               "status": "PASS", "qualification": "NOT_RUN"})
                with self.assertRaisesRegex(ValueError, "unexpected|Unexpected"):
                    package.inspect_package(output, package.sha256(output / "package-manifest.json"))

    def publish(self, output, **changes):
        manifest = {"schema_version": 1, "kind": "runtime-deployment-fixture",
                    "status": "PASS", "qualification": "NOT_RUN"}
        manifest.update(changes)
        package.publish(output, self.payload(), manifest)
        return package.sha256(output / "package-manifest.json")

    def test_manifest_and_payload_hashes_round_trip(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "package"
            expected = self.publish(output)
            self.assertEqual(package.inspect_package(output, expected)["status"], "PASS")
            (output / "bin/Runtime.exe").write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "changed"):
                package.inspect_package(output, expected)

    def test_manifest_tamper_cannot_change_trusted_file_hashes(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "package"
            expected = self.publish(output)
            manifest = json.loads((output / "package-manifest.json").read_text())
            manifest["files"]["bin/Runtime.exe"]["sha256"] = "0" * 64
            (output / "package-manifest.json").write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "changed"):
                package.inspect_package(output, expected)

    def test_extra_file_and_missing_license_are_rejected(self):
        for mutation in ("extra", "missing"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "package"
                expected = self.publish(output)
                if mutation == "extra":
                    (output / "bin/dxcompiler.dll").write_bytes(b"forbidden")
                else:
                    (output / "licenses/NVRHI/LICENSE.txt").unlink()
                with self.assertRaisesRegex(ValueError, "inventory"):
                    package.inspect_package(output, expected)

    def test_even_manifested_authoring_payload_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "package"
            files = self.payload()
            files["bin/Editor.exe"] = b"forbidden"
            package.publish(output, files, {"schema_version": 1, "kind": "runtime-deployment-fixture",
                                           "status": "PASS", "qualification": "NOT_RUN"})
            with self.assertRaisesRegex(ValueError, "unexpected"):
                package.inspect_package(output, package.sha256(output / "package-manifest.json"))

    def test_manifested_source_and_absent_notices_are_rejected(self):
        for mutation in ("source", "license"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "package"
                files = self.payload()
                if mutation == "source":
                    files["shaders/source.hlsl"] = b"uncompiled source"
                else:
                    del files["licenses/NVRHI/LICENSE.txt"]
                package.publish(output, files, {"schema_version": 1, "kind": "runtime-deployment-fixture",
                                               "status": "PASS", "qualification": "NOT_RUN"})
                with self.assertRaisesRegex(ValueError, "auxiliary|notices"):
                    package.inspect_package(output, package.sha256(output / "package-manifest.json"))

    def test_no_overwrite_or_unsafe_payload_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "package"
            expected = self.publish(output)
            with self.assertRaisesRegex(ValueError, "fresh"):
                self.publish(output)
            self.assertEqual(package.sha256(output / "package-manifest.json"), expected)
            for name in ("../escape", "bin/../../escape", "C:/escape", "bin\\escape"):
                with self.subTest(name=name), self.assertRaises(ValueError):
                    package.publish(Path(temporary) / "other", {name: b"bad"}, {})
            self.assertFalse((Path(temporary) / "other").exists())

    def completion(self, **changes):
        row = {"event": "graphics-sample-complete", "validation": "off", "adapter_uuid": "a" * 32,
               "submitted": 60, "presented": 60, "completed": 60, "generations": 1,
               "retired_generations": 1, "synchronization": "present_fence", "qualification": "NOT_RUN"}
        row.update(changes)
        return json.dumps(row)

    def test_completed_positive_sample_keeps_qualification_not_run(self):
        self.assertEqual(package.completion(self.completion(), "a" * 32)["qualification"], "NOT_RUN")
        package.completion(self.completion(synchronization="host_completion"), "a" * 32)

    def cube_payload(self):
        binaries = []
        for shader, stage, bindings in ((pipeline.BUNDLE_SHADERS[0], 0, (256,)),
                                        (pipeline.BUNDLE_SHADERS[1], 4, (0, 128))):
            name = shader["entry"].encode() + b"\0"
            name += b"\0" * (-len(name) % 4)
            args = [stage, 1, *struct.unpack(f"<{len(name) // 4}I", name)]
            words = [0x07230203, 0x10600, 0, 64, 0, ((len(args) + 1) << 16) | 15, *args]
            for index, binding in enumerate(bindings, 2):
                words.extend([(4 << 16) | 71, index, 33, binding, (4 << 16) | 71, index, 34, 0])
            binaries.append(struct.pack(f"<{len(words)}I", *words))
        vertex, pixel = binaries
        data = pipeline.BUNDLE_HEADER.pack(pipeline.BUNDLE_MAGIC, 1, len(vertex), len(pixel), 0,
                                            hashlib.sha256(vertex).digest(), hashlib.sha256(pixel).digest()) + vertex + pixel
        record = pipeline.inspect_bundle(data)
        inputs = {"toolchain": {"lock_sha256": "a" * 64}}
        generation = package.digest(pipeline.canonical(inputs))
        provenance = {"schema_version": 1, "generation": generation, "inputs": inputs,
                      "shaders": [{**row, "file": f"generations/{generation}/{row['id']}.spv"}
                                  for row in record["shaders"]]}
        source = pipeline.canonical(provenance)
        files = self.payload()
        files.update({package.SHADER_BUNDLE: data, package.SHADER_MANIFEST: source})
        manifest = {"schema_version": 1, "checkpoint": "06c", "kind": "runtime-deployment-fixture",
                    "status": "PASS", "qualification": "NOT_RUN", "dependency_lock_sha256": "a" * 64,
                    "shader_bundle": {**record, "generation": generation, "artifact_manifest_sha256": package.digest(source)}}
        return files, manifest

    def test_packaged_cube_bundle_and_dependency_provenance_round_trip(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "package"
            files, manifest = self.cube_payload()
            package.publish(output, files, manifest)
            actual = package.inspect_package(output, package.sha256(output / "package-manifest.json"))
            self.assertEqual(actual["shader_bundle"]["sha256"], manifest["shader_bundle"]["sha256"])

    def test_manifested_shader_corruption_incomplete_closure_and_provenance_are_rejected(self):
        for mutation in ("corrupt", "missing", "undeclared", "metadata", "generation", "provenance", "identity", "source"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "package"
                files, manifest = self.cube_payload()
                if mutation == "corrupt":
                    files[package.SHADER_BUNDLE] = files[package.SHADER_BUNDLE][:-1] + b"x"
                elif mutation == "missing":
                    del files[package.SHADER_BUNDLE]
                elif mutation == "undeclared":
                    del manifest["shader_bundle"]
                elif mutation == "metadata":
                    manifest["shader_bundle"]["sha256"] = "0" * 64
                elif mutation == "generation":
                    manifest["shader_bundle"]["generation"] = "0" * 64
                elif mutation in {"provenance", "identity"}:
                    provenance = json.loads(files[package.SHADER_MANIFEST])
                    if mutation == "provenance":
                        provenance["inputs"] = []
                    else:
                        provenance["shaders"][0]["sha256"] = "0" * 64
                    files[package.SHADER_MANIFEST] = pipeline.canonical(provenance)
                    manifest["shader_bundle"]["artifact_manifest_sha256"] = package.digest(files[package.SHADER_MANIFEST])
                else:
                    files["bin/assets/source.hlsl"] = b"source"
                package.publish(output, files, manifest)
                with self.assertRaises(ValueError):
                    package.inspect_package(output, package.sha256(output / "package-manifest.json"))

    def test_cube_completion_requires_exact_bundle_draw_and_retired_gpu_work(self):
        row = json.loads(self.completion())
        row.update(event="cube-sample-complete", draw="textured-cube", shader_bundle_sha256="f" * 64)
        self.assertEqual(package.completion(json.dumps(row), "a" * 32, "f" * 64)["draw"], "textured-cube")
        for changes in ({"event": "graphics-sample-complete"}, {"draw": "clear"},
                        {"shader_bundle_sha256": "e" * 64}, {"completed": 59}, {"retired_generations": 0}):
            changed = copy.deepcopy(row)
            changed.update(changes)
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                package.completion(json.dumps(changed), "a" * 32, "f" * 64)
        with self.assertRaises(ValueError):
            package.completion(json.dumps(row), "a" * 32)

    def test_completion_rejects_wrong_device_validation_and_incomplete_work(self):
        for changes in ({"adapter_uuid": "b" * 32}, {"validation": "required"}, {"qualification": "PASS"},
                        {"submitted": 0}, {"presented": True}, {"completed": 59},
                        {"retired_generations": 0}, {"presented": 61}, {"synchronization": "not_started"}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                package.completion(self.completion(**changes), "a" * 32)

    def test_completion_rejects_duplicate_or_missing_record(self):
        for text in ("", self.completion() + "\n" + self.completion(), "{invalid}",
                     self.completion() + "\nunexpected diagnostic", "[]", "null",
                     self.completion().replace('"submitted": 60', '"submitted": 60, "submitted": 60'),
                     self.completion().replace('"submitted": 60', '"submitted": NaN')):
            with self.subTest(text=text), self.assertRaises(ValueError):
                package.completion(text, "a" * 32)

    def test_module_audit_requires_app_local_loader_and_crt(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory, system = Path(temporary) / "package", Path(temporary) / "Windows"
            modules = [str(directory / name) for name in package.BINARIES]
            modules.append(str(system / "System32/kernel32.dll"))
            self.assertEqual(package.module_errors(modules, directory, system), [])
            modules = [name for name in modules if not name.endswith("vulkan-1.dll")]
            modules.append(str(system / "System32/vulkan-1.dll"))
            self.assertTrue(package.module_errors(modules, directory, system))

    def test_module_audit_rejects_development_and_forbidden_modules(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory, system = Path(temporary) / "package", Path(temporary) / "Windows"
            modules = [str(directory / name) for name in package.BINARIES]
            for bad in (Path(temporary) / "SDK/vulkan-1.dll", system / "System32/dxcompiler.dll"):
                with self.subTest(bad=bad):
                    self.assertTrue(package.module_errors([*modules, str(bad)], directory, system))

    def scene_completion(self):
        return {"event": "runtime-scene-complete", "adapter_uuid": "a" * 32, "validation": "off",
                "adapter_name": "Fixture GPU", "driver_name": "Fixture driver", "driver_info": "CPU fixture",
                "driver_version": 1, "width": 160, "height": 120, "rendered_frames": 2,
                "draw_count": 0, "mesh_instances": 3, "hidden": 1, "culled": 2,
                "scene_instance": 1, "scene_revision": 4, "warnings": 0, "errors": 0, "dropped": 0,
                "presentation": {"submitted": 2, "completed": 2, "presented": 2, "generations": 1,
                                 "retired_generations": 1, "in_flight": 0, "closed": True, "faulted": False},
                "qualification": "NOT_RUN", "capture": {"color": "color.rgba", "depth": "depth.f64",
                                                        "color_space": "srgb", "subpixel_precision_bits": 8}}

    def test_isolated_scene_completion_accepts_arbitrary_geometry_and_rejects_unfinished_work(self):
        baseline = self.scene_completion()
        self.assertEqual(package.scene_completion(json.dumps(baseline), "a" * 32), baseline)
        for changes in ({"validation": "required"}, {"adapter_uuid": "b" * 32}, {"width": 161},
                        {"rendered_frames": 1}, {"rendered_frames": True}, {"scene_instance": 0},
                        {"warnings": 1}, {"hidden": 2}, {"draw_count": -1}, {"qualification": "PASS"}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                package.scene_completion(json.dumps({**baseline, **changes}), "a" * 32)
        for changes in ({"closed": False}, {"faulted": True}, {"in_flight": 1}, {"completed": 1},
                        {"retired_generations": 0}, {"presented": 1}, {"generations": True}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                value = {**baseline, "presentation": {**baseline["presentation"], **changes}}
                package.scene_completion(json.dumps(value), "a" * 32)

    def write_scene_capture(self, directory, row):
        directory.mkdir()
        (directory / "report.json").write_text(json.dumps(row), encoding="utf-8")
        pixels = row["width"] * row["height"]
        (directory / "color.rgba").write_bytes(bytes((0, 0, 0, 255)) * pixels)
        (directory / "depth.f64").write_bytes(struct.pack("<d", 1.) * pixels)

    def test_isolated_scene_capture_requires_exact_saved_result_lengths_and_finite_depth(self):
        with tempfile.TemporaryDirectory() as temporary:
            baseline = self.scene_completion()
            directory = Path(temporary) / "capture"
            self.write_scene_capture(directory, baseline)
            row, evidence = package.inspect_scene_capture(directory, json.dumps(baseline), "a" * 32)
            self.assertEqual(row, baseline)
            self.assertEqual(set(evidence["files"]), {"report.json", "color.rgba", "depth.f64"})
            for value in (math.nan, math.inf, -0.01, 1.01):
                (directory / "depth.f64").write_bytes(struct.pack("<d", value) + struct.pack("<d", 1.) * (160 * 120 - 1))
                with self.subTest(value=value), self.assertRaisesRegex(ValueError, "depth"):
                    package.inspect_scene_capture(directory, json.dumps(baseline), "a" * 32)
            (directory / "depth.f64").write_bytes(struct.pack("<d", 1.) * (160 * 120))
            (directory / "color.rgba").write_bytes(b"truncated")
            with self.assertRaisesRegex(ValueError, "extent"):
                package.inspect_scene_capture(directory, json.dumps(baseline), "a" * 32)
            (directory / "color.rgba").write_bytes(bytes((0, 0, 0, 255)) * (160 * 120))
            (directory / "report.json").write_text(json.dumps({**baseline, "draw_count": 1}), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "completion"):
                package.inspect_scene_capture(directory, json.dumps(baseline), "a" * 32)
            (directory / "extra.txt").write_bytes(b"extra")
            with self.assertRaisesRegex(ValueError, "inventory"):
                package.inspect_scene_capture(directory, json.dumps(baseline), "a" * 32)

    def launch_report(self, directory, working, system, mode, stdout, device, capture=None):
        command = [str(directory / "bin/Runtime.exe")]
        command += (["--check-package", str(directory / package.STARTUP)] if mode == "inspect" else
                    ["--package", str(directory / package.STARTUP), "--extent", "160x120", "--frames", "2",
                     "--device", device, "--validation", "off", "--capture-directory", str(capture)])
        return {"mode": mode, "exit_code": 0, "timed_out": False, "elapsed_seconds": .05,
                "stdout": stdout, "stderr": "", "module_poll_errors": 0,
                "modules": [str(directory / name) for name in package.BINARIES],
                "working_directory": str(working), "command": command, "arguments": "quoted fixture arguments",
                "capture_directory": str(capture) if capture else None,
                "scene_window": {"window_observed": True, "window_process_id": 42, "process_id": 42,
                                 "dpi": 96, "client_width": 160, "client_height": 120,
                                 "per_monitor_v2": True} if mode == "scene" else None,
                "environment": {"PATH": str(system / "System32"), "SystemRoot": str(system),
                                "VK_LOADER_LAYERS_DISABLE": "~implicit~", "inherited_development_overrides": False}}

    def test_isolated_scene_uses_independent_144_dpi_window_extent_and_rejects_mismatches(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            directory, working, system, capture = root / "package", root / "cwd", root / "Windows", root / "capture"
            baseline = {**self.scene_completion(), "width": 240, "height": 180}
            report = self.launch_report(directory, working, system, "scene", json.dumps(baseline), "a" * 32, capture)
            report["scene_window"].update(dpi=144, client_width=240, client_height=180)
            extent = package.isolated_launch_report(report, "scene", directory, working, system, "a" * 32, capture)
            self.assertEqual(extent, (240, 180))
            self.write_scene_capture(capture, baseline)
            row, evidence = package.inspect_scene_capture(capture, report["stdout"], "a" * 32, extent)
            self.assertEqual(row, baseline)
            self.assertEqual(evidence["files"]["depth.f64"]["bytes"], 240 * 180 * 8)
            with self.assertRaisesRegex(ValueError, "dimensions"):
                package.scene_completion(json.dumps(self.scene_completion()), "a" * 32, extent)
            (capture / "color.rgba").write_bytes(bytes(160 * 120 * 4))
            with self.assertRaisesRegex(ValueError, "extent"):
                package.inspect_scene_capture(capture, report["stdout"], "a" * 32, extent)
            for changes in ({"dpi": 96}, {"client_width": 241}, {"window_process_id": 43},
                            {"per_monitor_v2": False}, {"window_observed": False}, {"dpi": True},
                            {"dpi": 1536, "client_width": 2560, "client_height": 1920}):
                value = {**report, "scene_window": {**report["scene_window"], **changes}}
                with self.subTest(changes=changes), self.assertRaises(ValueError):
                    package.isolated_launch_report(value, "scene", directory, working, system, "a" * 32, capture)

    def test_isolated_scene_report_rejects_environment_cwd_arguments_and_failure_flags(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            directory, working, system, capture = root / "package", root / "cwd", root / "Windows", root / "capture"
            working.mkdir()
            report = self.launch_report(directory, working, system, "scene", "{}", "a" * 32, capture)
            package.isolated_launch_report(report, "scene", directory, working, system, "a" * 32, capture)
            (working / "driver-created").mkdir()
            package.isolated_launch_report(report, "scene", directory, working, system, "a" * 32, capture)
            for mutation in ("environment", "environment-type", "cwd", "command", "capture", "mode", "timeout", "stderr", "exit", "elapsed"):
                value = copy.deepcopy(report)
                if mutation == "environment":
                    value["environment"]["inherited_development_overrides"] = True
                elif mutation == "environment-type":
                    value["environment"]["inherited_development_overrides"] = 0
                elif mutation == "cwd":
                    value["working_directory"] = str(directory)
                elif mutation == "command":
                    value["command"][1] = "--graphics"
                elif mutation == "capture":
                    value["capture_directory"] = str(directory)
                elif mutation == "mode":
                    value["mode"] = "graphics"
                elif mutation == "timeout":
                    value["timed_out"] = True
                elif mutation == "stderr":
                    value["stderr"] = "unexpected diagnostic"
                elif mutation == "exit":
                    value["exit_code"] = False
                else:
                    value["elapsed_seconds"] = math.nan
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    package.isolated_launch_report(value, "scene", directory, working, system, "a" * 32, capture)

    def test_scene_package_checker_connects_one_cpu_inspection_and_two_isolated_captures(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            directory, record, shaders, payload, manifest, _ = self.scene_inputs(root / "inputs")
            files, metadata = package.assemble_scene_package(payload, manifest, directory, record, shaders,
                                                               content_notices={"CC0-1.0": b"Fixture waiver"})
            relocated = root / "relocated package 世界"
            published = package.publish(relocated, files, metadata)
            expected_hash = package.sha256(relocated / "package-manifest.json")
            system = root / "Windows"
            calls = []

            def invoke(command, *, cwd, timeout):
                self.assertEqual(timeout, 30)
                working = Path(command[command.index("-WorkingDirectory") + 1])
                self.assertEqual(Path(cwd), working)
                self.assertEqual(list(working.iterdir()), [])
                device = command[command.index("-Device") + 1]
                mode = "inspect" if "-Inspect" in command else "scene"
                calls.append(mode)
                capture = None
                if mode == "inspect":
                    value = {"format": "shark.package-inspection", "schema_version": 1, "status": "PASS",
                             "renderer_initialized": False, "target": "windows-x86_64",
                             **{key: record[key] for key in ("project", "world", "scene", "camera")},
                             "startup_sha256": record["files"][package.STARTUP]["sha256"], "mesh_count": 1,
                             "qualification": "NOT_RUN"}
                else:
                    value = {**self.scene_completion(), "adapter_uuid": device}
                    capture = Path(command[command.index("-CaptureDirectory") + 1])
                    self.write_scene_capture(capture, value)
                    generated = working / "NVIDIA Corporation" / "umdlogs"
                    generated.mkdir(parents=True)
                    (generated / "fixture.log").write_bytes(b"driver output")
                report = self.launch_report(relocated, working, system, mode, json.dumps(value), device, capture)
                return ProcessResult(command, 0, json.dumps(report), "", False)

            with patch.object(package, "run_process", side_effect=invoke), \
                    patch.dict(os.environ, {"SystemRoot": str(system)}), \
                    patch("windows_unelevated.process_security", return_value={"fixture": True}), \
                    patch("windows_unelevated._assert_standard"):
                result = package.check_package(relocated, expected_hash, ["a" * 32, "b" * 32], root / "results")
            self.assertEqual(result["status"], "PASS", result["errors"])
            self.assertEqual(result["checkpoint"], "14c")
            self.assertEqual(result["qualification"], "NOT_RUN")
            self.assertEqual(calls, ["inspect", "scene", "scene"])
            self.assertEqual(result["inspection"]["content"]["camera"], record["camera"])
            self.assertTrue(result["inspection"]["working_directory_initially_empty"])
            self.assertEqual(result["inspection"]["working_directory_after"], [])
            self.assertEqual(len(result["runs"]), 2)
            self.assertNotEqual(result["runs"][0]["working_directory"], result["runs"][1]["working_directory"])
            for row in result["runs"]:
                self.assertTrue(row["working_directory_initially_empty"])
                self.assertEqual(row["working_directory_after"], [
                    {"path": "NVIDIA Corporation", "kind": "directory", "bytes": 0},
                    {"path": "NVIDIA Corporation/umdlogs", "kind": "directory", "bytes": 0},
                    {"path": "NVIDIA Corporation/umdlogs/fixture.log", "kind": "file", "bytes": 13}])
            self.assertFalse(Path(result["working_directory"]).exists())
            self.assertEqual(package.inspect_package(relocated, expected_hash), published)

            retained = root / "shark-runtime-working-failure"
            retained.mkdir()

            def fail(command, *, cwd, timeout):
                (Path(cwd) / "failure.log").write_bytes(b"inspect this failure")
                return ProcessResult(command, 1, "", "injected launcher failure", False)

            with patch.object(package, "run_process", side_effect=fail), \
                    patch.object(package.tempfile, "mkdtemp", return_value=str(retained)), \
                    patch.dict(os.environ, {"SystemRoot": str(system)}), \
                    patch("windows_unelevated.process_security", return_value={"fixture": True}), \
                    patch("windows_unelevated._assert_standard"):
                failed = package.check_package(relocated, expected_hash, ["a" * 32, "b" * 32], root / "failure-results")
            self.assertEqual(failed["status"], "FAIL")
            self.assertEqual(failed["retained_working_directory"], str(retained))
            self.assertEqual((retained / "inspection/failure.log").read_bytes(), b"inspect this failure")
            self.assertEqual(failed["inspection"]["working_directory_after"],
                             [{"path": "failure.log", "kind": "file", "bytes": 20}])

    def scene_inputs(self, root):
        """Independent byte fixture for package framing; no native/GPU claim."""
        root.mkdir()
        shaders = root / "shaders"
        shaders.mkdir()
        recipe = {"schema_version": 1, "convention": "shark-vulkan-1",
                  "shaders": [{**row, "source": "scene_unlit.hlsl"} for row in package.SCENE_SHADERS]}
        inputs = {"recipe": recipe, "sources": {"scene_unlit.hlsl": {"sha256": "1" * 64, "bytes": 20}},
                  "toolchain": {"lock_sha256": "a" * 64, "host": "windows-x86_64",
                                "compiler_archive_sha256": "2" * 64, "compiler_files": {"dxc.exe": "3" * 64},
                                "validator_sha256": "4" * 64, "validator_revision": "5" * 40},
                  "flags": pipeline.FLAGS, "pipeline_sha256": "6" * 64}
        generation = package.digest(pipeline.canonical(inputs))
        binaries, rows = [], []
        generation_dir = shaders / "generations" / generation
        generation_dir.mkdir(parents=True)
        for shader, stage, bindings in ((package.SCENE_SHADERS[0], 0, (256,)),
                                        (package.SCENE_SHADERS[1], 4, ())):
            name = shader["entry"].encode() + b"\0"
            name += b"\0" * (-len(name) % 4)
            args = [stage, 1, *struct.unpack(f"<{len(name) // 4}I", name)]
            words = [0x07230203, 0x10600, 0, 64, 0, ((len(args) + 1) << 16) | 15, *args]
            for index, binding in enumerate(bindings, 2):
                words.extend([(4 << 16) | 71, index, 33, binding, (4 << 16) | 71, index, 34, 0])
            binary = struct.pack(f"<{len(words)}I", *words)
            binaries.append(binary)
            (generation_dir / (shader["id"] + ".spv")).write_bytes(binary)
            rows.append({"id": shader["id"], "file": f"generations/{generation}/{shader['id']}.spv",
                         "sha256": package.digest(binary), "bytes": len(binary)})
        provenance = {"schema_version": 1, "convention": "shark-vulkan-1", "generation": generation,
                      "inputs": inputs, "shaders": rows}
        (shaders / "shader-manifest.json").write_bytes(pipeline.canonical(provenance))
        mesh = "40000000-0000-4000-8000-000000000001"
        record = {"format": "shark.client-export-content", "schema_version": 1,
                  "project": "10000000-0000-4000-8000-000000000001",
                  "world": "10000000-0000-4000-8000-000000000002",
                  "scene": "10000000-0000-4000-8000-000000000003",
                  "camera": "10000000-0000-4000-8000-000000000004",
                  "name": "Export 世界", "target": "test-client", "dirty": False,
                  "sources": [{"asset": mesh, "key": "b" * 64, "origin": "Original fixture", "license": "CC0-1.0"}]}
        content = {package.STARTUP_SCENE: b"synthetic prepared scene", package.SCENE_VERTEX: binaries[0],
                   package.SCENE_PIXEL: binaries[1], f"bin/content/{mesh}.sharkmesh": b"synthetic prepared mesh"}
        identities = {path: {"sha256": package.digest(data), "bytes": len(data)} for path, data in content.items()}
        body = b"".join(uuid.UUID(record[key]).bytes for key in ("project", "world", "scene", "camera"))
        for text in (record["name"], "windows-x86_64"):
            encoded = text.encode("utf-8")
            body += struct.pack("<I", len(encoded)) + encoded
        for path in (package.STARTUP_SCENE, package.SCENE_VERTEX, package.SCENE_PIXEL):
            body += struct.pack("<I", identities[path]["bytes"]) + bytes.fromhex(identities[path]["sha256"])
        body += struct.pack("<I", 1) + uuid.UUID(mesh).bytes
        mesh_identity = identities[f"bin/content/{mesh}.sharkmesh"]
        body += struct.pack("<I", mesh_identity["bytes"]) + bytes.fromhex(mesh_identity["sha256"])
        descriptor = b"SHKSTRT1" + struct.pack("<I", 1) + hashlib.sha256(body).digest() + body
        content[package.STARTUP] = descriptor
        record["files"] = {path: {"sha256": package.digest(data), "bytes": len(data)} for path, data in content.items()}
        directory = root / "content"
        directory.mkdir()
        for path, data in content.items():
            destination = directory / path
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        payload = {name: b"synthetic binary" for name in package.BINARIES}
        payload.update({name: b"synthetic notice" for name in package.scene_license_paths()
                        if name not in {package.CONTENT_ATTRIBUTION, package.CONTENT_NOTICES}})
        manifest = {"schema_version": 1, "kind": "runtime-deployment-fixture", "status": "PASS",
                    "qualification": "NOT_RUN", "platform": "windows-x86_64", "configuration": "Release",
                    "dependency_lock_sha256": "a" * 64}
        return directory, record, shaders, payload, manifest, provenance

    def test_scene_export_preserves_descriptor_closure_attribution_and_shader_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory, record, shaders, payload, manifest, provenance = self.scene_inputs(Path(temporary) / "inputs")
            vertex, pixel, observed = package.scene_shader_inputs(shaders)
            self.assertEqual(observed, provenance)
            self.assertEqual(vertex.name, "scene-unlit-vs.spv")
            self.assertEqual(pixel.name, "scene-unlit-ps.spv")
            files, metadata = package.assemble_scene_package(payload, manifest, directory, record, shaders,
                                                               provenance["generation"],
                                                               content_notices={"CC0-1.0": b"Fixture source notice\n"})
            output = Path(temporary) / "package"
            package.publish(output, files, metadata)
            actual = package.inspect_package(output, package.sha256(output / "package-manifest.json"))
            self.assertEqual(actual["kind"], "runtime-scene-export")
            self.assertEqual(actual["content"], record)
            self.assertEqual(json.loads(files[package.CONTENT_ATTRIBUTION])["sources"], record["sources"])
            self.assertEqual(json.loads(files[package.CONTENT_NOTICES])["entries"],
                             [{"license": "CC0-1.0", "sha256": package.digest(b"Fixture source notice\n"),
                               "bytes": 22, "text": "Fixture source notice\n"}])
            self.assertEqual(actual["qualification"], "NOT_RUN")
            self.assertNotIn("shader_bundle", actual)

    def test_scene_content_rejects_versions_paths_budgets_and_incomplete_attribution(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, source, _, _, _, _ = self.scene_inputs(Path(temporary) / "inputs")
            mutations = {
                "version": lambda row: row.update(schema_version=2),
                "boolean-version": lambda row: row.update(schema_version=True),
                "unknown-field": lambda row: row.update(source_path="private/source.gltf"),
                "bad-id": lambda row: row.update(camera="not-a-uuid"),
                "bad-target": lambda row: row.update(target="Server"),
                "control-name": lambda row: row.update(name="Name\u0080"),
                "absent-startup": lambda row: row["files"].pop(package.STARTUP),
                "escaping-file": lambda row: row["files"].update({"../private.bin": {"sha256": "a" * 64, "bytes": 1}}),
                "large-scene": lambda row: row["files"][package.STARTUP_SCENE].update(bytes=4 * 1024 * 1024 + 1),
                "invalid-shader": lambda row: row["files"][package.SCENE_VERTEX].update(bytes=21),
                "missing-attribution": lambda row: row.update(sources=[]),
                "duplicate-attribution": lambda row: row["sources"].append(copy.deepcopy(row["sources"][0])),
                "source-path": lambda row: row["sources"][0].update(path="secret.gltf"),
            }
            for name, mutate in mutations.items():
                with self.subTest(name=name), self.assertRaises(ValueError):
                    record = copy.deepcopy(source)
                    mutate(record)
                    package.validate_content_record(record)

    def test_scene_assembly_rejects_extra_files_and_changed_shader_generation(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory, record, shaders, payload, manifest, _ = self.scene_inputs(Path(temporary) / "inputs")
            with self.assertRaisesRegex(ValueError, "generation"):
                package.assemble_scene_package(payload, manifest, directory, record, shaders, "0" * 64)
            (directory / "source.gltf").write_bytes(b"private")
            with self.assertRaisesRegex(ValueError, "inventory"):
                package.assemble_scene_package(payload, manifest, directory, record, shaders)

    def test_scene_package_rejects_descriptor_content_attribution_and_provenance_disagreement(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory, record, shaders, payload, manifest, _ = self.scene_inputs(Path(temporary) / "inputs")
            original_files, original_manifest = package.assemble_scene_package(
                payload, manifest, directory, record, shaders,
                content_notices={"CC0-1.0": b"Fixture source notice\n"})
            for mutation in ("descriptor", "camera", "mesh", "attribution", "shader-profile", "shader-hash",
                             "platform", "private", "missing-shader", "extra-license", "notice"):
                with self.subTest(mutation=mutation):
                    files, metadata = copy.deepcopy(original_files), copy.deepcopy(original_manifest)
                    if mutation == "descriptor":
                        files[package.STARTUP] += b"trailing"
                        metadata["content"]["files"][package.STARTUP] = {
                            "sha256": package.digest(files[package.STARTUP]), "bytes": len(files[package.STARTUP])}
                    elif mutation == "camera":
                        metadata["content"]["camera"] = "10000000-0000-4000-8000-000000000005"
                    elif mutation == "mesh":
                        mesh_path = next(path for path in files if path.endswith(".sharkmesh"))
                        files[mesh_path] += b"changed"
                        metadata["content"]["files"][mesh_path] = {
                            "sha256": package.digest(files[mesh_path]), "bytes": len(files[mesh_path])}
                    elif mutation == "attribution":
                        files[package.CONTENT_ATTRIBUTION] = b"{}"
                    elif mutation == "notice":
                        notices = json.loads(files[package.CONTENT_NOTICES])
                        notices["entries"][0]["text"] = "Changed text, original digest"
                        files[package.CONTENT_NOTICES] = package.canonical(notices)
                    elif mutation == "shader-profile":
                        metadata["scene_shaders"]["profile"] = "textured-cube"
                    elif mutation == "shader-hash":
                        metadata["scene_shaders"]["artifact_manifest_sha256"] = "0" * 64
                    elif mutation == "platform":
                        metadata["platform"] = "linux-x86_64"
                    elif mutation == "private":
                        files["bin/content/source.gltf"] = b"private"
                    elif mutation == "extra-license":
                        files["licenses/Content/source.gltf"] = b"private"
                    else:
                        del files[package.SCENE_PIXEL]
                    output = Path(temporary) / mutation
                    package.publish(output, files, metadata)
                    with self.assertRaises(ValueError):
                        package.inspect_package(output, package.sha256(output / "package-manifest.json"))

    def test_content_notices_require_exact_bounded_text_for_every_used_license(self):
        record = {"sources": [{"license": "MIT"}, {"license": "MIT"}, {"license": "CC0-1.0"}]}
        notices = {"MIT": b"Copyright fixture\r\nPermission fixture\n", "CC0-1.0": b"Fixture waiver\n"}
        encoded = package.content_notice_payload(record, notices)
        package.inspect_content_notices(encoded, record)
        self.assertEqual([entry["license"] for entry in json.loads(encoded)["entries"]], ["CC0-1.0", "MIT"])
        for invalid in (None, {}, {"MIT": b"Missing second license"}, {**notices, "unused": b"extra"},
                        {**notices, "MIT": b""}, {**notices, "MIT": b"\xff"},
                        {**notices, "MIT": b"bad\0notice"}, {**notices, "MIT": b"a" * (64 * 1024 + 1)}):
            with self.subTest(invalid=invalid is None), self.assertRaises(ValueError):
                package.content_notice_payload(record, invalid)
        oversized = {f"license-{number}": b"a" * (64 * 1024) for number in range(17)}
        with self.assertRaisesRegex(ValueError, "combined byte budget"):
            package.content_notice_payload({"sources": [{"license": key} for key in oversized]}, oversized)
        with self.assertRaisesRegex(ValueError, "complete and sorted"):
            changed = json.loads(encoded)
            changed["entries"].reverse()
            package.inspect_content_notices(package.canonical(changed), record)
        package.inspect_content_notices(package.content_notice_payload({"sources": []}, None), {"sources": []})

    def test_final_publication_fence_runs_after_staging_and_failure_preserves_destination(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            output = parent / "package"
            calls = []

            def reject():
                calls.append("validate")
                self.assertFalse(output.exists())
                stages = list(parent.glob(".runtime-package-*"))
                self.assertEqual(len(stages), 1)
                self.assertEqual((stages[0] / "bin/Runtime.exe").read_bytes(), b"candidate")
                self.assertTrue((stages[0] / "package-manifest.json").is_file())
                raise ValueError("source changed before publication")

            with self.assertRaisesRegex(ValueError, "source changed"):
                package.publish(output, {"bin/Runtime.exe": b"candidate"}, {}, before_publish=reject)
            self.assertEqual(calls, ["validate"])
            self.assertFalse(output.exists())
            self.assertEqual(list(parent.iterdir()), [])
            package.publish(output, {"bin/Runtime.exe": b"candidate"}, {}, before_publish=lambda: calls.append("accept"))
            self.assertEqual(calls, ["validate", "accept"])
            self.assertEqual((output / "bin/Runtime.exe").read_bytes(), b"candidate")


if __name__ == "__main__":
    unittest.main()
