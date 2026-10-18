"""CPU-only Runtime deployment manifest, closure and completion rejection tests."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest
import copy
import hashlib
import struct
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_runtime as package
import shader_pipeline as pipeline


class RuntimePackageTests(unittest.TestCase):
    def test_production_notices_include_private_core_entt_from_verified_cache(self):
        lock = {"dependencies": {name: {"url": f"https://example.invalid/{name}.tar.gz",
                                       "sha256": name, "license": "MIT"}
                                  for name in ("EnTT", "GLFW", "NVRHI", "VulkanHeaders", "VulkanLoader")}}
        local_archive = Path("local cache/entt.tar.gz")
        with patch.object(package, "cache_values", return_value={"SHARK_EnTT_ARCHIVE": str(local_archive)}), \
                patch.object(package, "archive_notices", return_value={"LICENSE": b"license"}) as notices:
            payload, records = package.production_notices(Path("build"), lock)
        self.assertIn("licenses/EnTT/LICENSE", payload)
        self.assertEqual(records["EnTT"]["archive_sha256"], "EnTT")
        self.assertEqual(notices.call_args_list[0].args[0], local_archive)

    def test_missing_manifested_entt_license_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "package"
            payload = self.payload()
            del payload["licenses/EnTT/LICENSE.txt"]
            package.publish(directory, payload, {"schema_version": 1, "kind": "runtime-deployment-fixture",
                                                 "status": "PASS", "qualification": "NOT_RUN"})
            with self.assertRaisesRegex(ValueError, "EnTT license notices"):
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
                  "SHARK_BUILD_CLIENT": "ON", "SHARK_BUILD_EDITOR": "OFF",
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
                               ("SHARK_BUILD_EDITOR", "ON"), ("SHARK_BUILD_TESTS", "ON"),
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

    def payload(self):
        files = {name: b"synthetic CPU fixture" for name in package.BINARIES}
        for name in ("EnTT", "GLFW", "NVRHI", "VulkanHeaders", "VulkanLoader", "MicrosoftVC"):
            files[f"licenses/{name}/LICENSE.txt"] = b"synthetic license fixture"
        return files

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


if __name__ == "__main__":
    unittest.main()
