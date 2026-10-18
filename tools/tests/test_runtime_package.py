"""CPU-only Runtime deployment manifest, closure and completion rejection tests."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_runtime as package


class RuntimePackageTests(unittest.TestCase):
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
        for name in ("GLFW", "NVRHI", "VulkanHeaders", "VulkanLoader", "MicrosoftVC"):
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
