"""CPU-only rejection and atomicity checks for the Windows graphics package."""

from __future__ import annotations

from argparse import Namespace
from contextlib import redirect_stdout
import io
import json
from pathlib import Path, PureWindowsPath
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_graphics_probe as package
from process_runner import ProcessResult


def pe_fixture() -> bytes:
    data = bytearray(128)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3c, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<H", data, 68, 0x8664)
    return bytes(data)


class GraphicsPackageTests(unittest.TestCase):
    def cache(self, directory: Path, **changes) -> Path:
        values = {"CMAKE_BUILD_TYPE": "Release", "SHARK_SANITIZER": "none",
                  "SHARK_BUILD_GRAPHICS_PROBE": "ON", "CMAKE_HOME_DIRECTORY": str(package.ROOT)}
        values.update(changes)
        directory.mkdir(exist_ok=True)
        (directory / "CMakeCache.txt").write_text("".join(f"{key}:STRING={value}\n" for key, value in values.items()), encoding="utf-8")
        executable = directory / "bin/GraphicsProbe.exe"
        executable.parent.mkdir(exist_ok=True)
        executable.write_bytes(pe_fixture())
        return executable

    def test_release_source_and_exact_executable_required(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable = self.cache(directory)
            self.assertEqual(package.cache_values(directory, executable)["CMAKE_BUILD_TYPE"], "Release")
            with self.assertRaisesRegex(ValueError, "selected Release"):
                package.cache_values(directory, directory / "other.exe")
            for changes, message in (({"CMAKE_BUILD_TYPE": "Debug"}, "Release"),
                                     ({"CMAKE_BUILD_TYPE": "RelWithDebInfo"}, "Release"),
                                     ({"SHARK_SANITIZER": "address"}, "sanitizers"),
                                     ({"CMAKE_HOME_DIRECTORY": str(directory / "other-source")}, "different source")):
                with self.subTest(changes=changes):
                    self.cache(directory, **changes)
                    with self.assertRaisesRegex(ValueError, message):
                        package.cache_values(directory, executable)

    def inspected(self, temporary: str, dependencies: dict[str, list[str]]):
        directory = Path(temporary)
        inspector = directory / "dumpbin.exe"
        inspector.write_bytes(b"bounded mock inspector")
        binaries = {}
        for name in dependencies:
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(pe_fixture())
            binaries[name] = path

        def invoke(command, **kwargs):
            name = Path(command[-1]).relative_to(directory).as_posix()
            text = (f"Dump of file {command[-1]}\n\n  Image has the following dependencies:\n\n"
                    + "".join(f"    {item}\n" for item in dependencies[name]) + "\n  Summary\n")
            return ProcessResult(command, 0, text, "", False)

        return binaries, inspector, invoke

    def test_recursive_import_closure_includes_dynamic_validation_layer(self):
        with tempfile.TemporaryDirectory() as temporary:
            binaries, inspector, invoke = self.inspected(temporary, {
                "bin/GraphicsProbe.exe": ["vulkan-1.dll", "KERNEL32.dll"],
                "bin/vulkan-1.dll": ["KERNEL32.dll"],
                "validation/VkLayer_khronos_validation.dll": ["KERNEL32.dll", "api-ms-win-crt-string-l1-1-0.dll"],
            })
            with patch.object(package, "run_process", side_effect=invoke) as run:
                report = package.inspect_closure(binaries, inspector)
            self.assertEqual(report["status"], "PASS")
            self.assertEqual(run.call_count, 3)

    def test_debug_crt_unknown_external_and_path_imports_rejected(self):
        for bad, message in (("MSVCP140D.dll", "Debug CRT"), ("VCRUNTIME140_1D.dll", "Debug CRT"),
                             ("ucrtbased.dll", "Debug CRT"), ("dxcompiler.dll", "Unpackaged"),
                             ("api-ms-win-shark.dll", "Unpackaged"), ("C:\\SDK\\custom.dll", "Unexpected path")):
            with self.subTest(imported=bad), tempfile.TemporaryDirectory() as temporary:
                binaries, inspector, invoke = self.inspected(temporary, {"bin/GraphicsProbe.exe": [bad]})
                with patch.object(package, "run_process", side_effect=invoke), self.assertRaisesRegex(ValueError, message):
                    package.inspect_closure(binaries, inspector)

    def test_layer_dependency_cannot_resolve_from_an_unsearched_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            binaries, inspector, invoke = self.inspected(temporary, {
                "bin/GraphicsProbe.exe": ["hidden.dll"], "validation/hidden.dll": ["KERNEL32.dll"]})
            with patch.object(package, "run_process", side_effect=invoke), self.assertRaisesRegex(ValueError, "Unpackaged"):
                package.inspect_closure(binaries, inspector)

    def test_inspector_failure_or_timeout_cannot_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            binaries, inspector, _ = self.inspected(temporary, {"bin/GraphicsProbe.exe": ["KERNEL32.dll"]})
            for result in (ProcessResult([], 1, "", "error", False), ProcessResult([], 0, "", "", True),
                           ProcessResult([], 0, "", "", False, ("cleanup failed",))):
                with patch.object(package, "run_process", return_value=result), self.assertRaisesRegex(ValueError, "inspection failed"):
                    package.inspect_closure(binaries, inspector)

    def test_wrong_architecture_and_malformed_pe_rejected(self):
        good = bytearray(pe_fixture())
        struct.pack_into("<H", good, 68, 0xaa64)
        for invalid in (b"not a binary", bytes(good), b"MZ" + b"\xff" * 100):
            with self.subTest(binary=invalid[:8]), self.assertRaises(ValueError):
                package.pe_x64(invalid, "input.dll")

    def crt(self, directory: Path) -> dict:
        for name in package.CRT_NAMES:
            (directory / name).write_bytes(pe_fixture())
        (directory / "NOTICE.txt").write_text("test license", encoding="utf-8")
        return {"version": "test-14", "architecture": "x86_64",
                "files": {name: package.sha256(directory / name) for name in package.CRT_NAMES},
                "license_files": [{"path": "NOTICE.txt", "sha256": package.sha256(directory / "NOTICE.txt"), "destination": "NOTICE.txt"}]}

    def test_crt_uses_exact_locked_selection_and_notice_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            manifest = self.crt(directory)
            with patch.object(package, "ROOT", directory):
                binaries, notices, _ = package.crt_inputs(directory, manifest)
                self.assertEqual(set(binaries), {"bin/" + name for name in package.CRT_NAMES})
                self.assertEqual(notices["licenses/MicrosoftVC/NOTICE.txt"], b"test license")
                (directory / "msvcp140.dll").write_bytes(b"changed")
                with self.assertRaisesRegex(ValueError, "changed"):
                    package.crt_inputs(directory, manifest)

    def test_crt_extra_missing_unpinned_and_escaping_inputs_rejected(self):
        for change in ("extra", "missing", "license_hash", "license_escape"):
            with self.subTest(change=change), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                manifest = self.crt(directory)
                if change == "extra":
                    manifest["files"]["extra.dll"] = "a" * 64
                elif change == "missing":
                    (directory / "vcruntime140.dll").unlink()
                elif change == "license_hash":
                    manifest["license_files"][0]["sha256"] = "a" * 64
                else:
                    manifest["license_files"][0]["path"] = "../outside.txt"
                with patch.object(package, "ROOT", directory), self.assertRaises(ValueError):
                    package.crt_inputs(directory, manifest)

    def test_missing_shader_fails_before_binary_inspection_or_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable = self.cache(directory / "build")
            validation_path = directory / "validation.json"
            validation_path.write_text("{}", encoding="utf-8")
            shaders = directory / "shaders"
            shaders.mkdir()
            args = Namespace(executable=executable, build_dir=executable.parent.parent,
                             validation_manifest=validation_path, shader_dir=shaders,
                             crt_dir=directory / "crt")
            with patch.object(package, "validation_tools", return_value={}), patch.object(package, "inspect_closure") as inspect:
                with self.assertRaisesRegex(ValueError, "Missing"):
                    package.prepare_payload(args)
            inspect.assert_not_called()

    def test_layer_library_resolves_relative_to_manifest_with_windows_loader_separator(self):
        source = {"file_format_version": "1.2.0", "layer": {
            "name": "VK_LAYER_KHRONOS_validation", "api_version": "1.4.352",
            "library_path": "C:\\developer-sdk\\VkLayer_khronos_validation.dll"}}
        document = json.loads(package.package_layer_manifest(source))
        library = document["layer"]["library_path"]
        # Regression: the locked loader does not classify './name.dll' as
        # manifest-relative on Windows, even though Win32 accepts '/' elsewhere.
        self.assertIn("\\", library)
        self.assertFalse(PureWindowsPath(library).is_absolute())
        with tempfile.TemporaryDirectory() as temporary:
            manifest_directory = Path(temporary) / "relocated package/validation"
            manifest_directory.mkdir(parents=True)
            expected = manifest_directory / "VkLayer_khronos_validation.dll"
            expected.write_bytes(pe_fixture())
            resolved = manifest_directory.joinpath(*PureWindowsPath(library).parts).resolve()
            self.assertEqual(resolved, expected.resolve())
            self.assertTrue(resolved.is_file())
        self.assertEqual(document["layer"]["api_version"], source["layer"]["api_version"])
        self.assertTrue(PureWindowsPath(source["layer"]["library_path"]).is_absolute())

    def test_publication_hashes_files_and_retains_unqualified_status(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "package"
            manifest = package.publish(output, {"bin/GraphicsProbe.exe": pe_fixture(), "README.txt": b"fixture"},
                                       {"schema_version": 1, "status": "PASS", "qualification": "NOT_RUN"})
            self.assertEqual(json.loads((output / "package-manifest.json").read_text()), manifest)
            self.assertEqual(manifest["qualification"], "NOT_RUN")
            self.assertEqual(manifest["files"]["README.txt"], {"sha256": package.digest(b"fixture"), "bytes": 7})
            self.assertTrue((output / "validation/implicit").is_dir())
            self.assertFalse(list(output.parent.glob(".graphics-package-*")))

    def test_existing_output_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "package"
            output.mkdir()
            (output / "preserve.txt").write_text("user output", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "preserved"):
                package.publish(output, {"README.txt": b"replacement"}, {})
            self.assertEqual((output / "preserve.txt").read_text(), "user output")

    def test_publication_failure_cleans_only_owned_staging(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "package"
            sentinel = output.parent / "preserve.txt"
            sentinel.write_text("user output", encoding="utf-8")
            with patch.object(package, "regular_file", side_effect=ValueError("copy verification failed")):
                with self.assertRaisesRegex(ValueError, "verification"):
                    package.publish(output, {"README.txt": b"payload"}, {})
            self.assertFalse(output.exists())
            self.assertEqual(sentinel.read_text(), "user output")
            self.assertFalse(list(output.parent.glob(".graphics-package-*")))

    def test_escaping_case_collision_and_reserved_payloads_rejected(self):
        for payload in ({"../escape": b"x"}, {"/absolute": b"x"}, {"C:/drive": b"x"},
                        {"bin\\bad.dll": b"x"}, {"Name": b"x", "name": b"y"}, {"package-manifest.json": b"{}"}):
            with self.subTest(paths=list(payload)), tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "package"
                with self.assertRaises(ValueError):
                    package.publish(output, payload, {})
                self.assertFalse(output.exists())

    def test_linked_input_and_output_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            data = directory / "data"
            data.write_bytes(b"data")
            linked = directory / "linked"
            try:
                linked.symlink_to(data)
            except OSError:
                # Exercise the same link guard without requiring Windows
                # Developer Mode or symlink privileges on hosted CPU runners.
                with patch.object(Path, "is_symlink", return_value=True), self.assertRaisesRegex(ValueError, "link"):
                    package.regular_file(data)
            else:
                with self.assertRaisesRegex(ValueError, "link"):
                    package.regular_file(linked)
                with self.assertRaisesRegex(ValueError, "link"):
                    package.publish(linked, {"README.txt": b"x"}, {})

    def test_cli_preserves_existing_output_without_preparation(self):
        with tempfile.TemporaryDirectory() as temporary:
            args = ["package_graphics_probe.py"]
            for name in ("executable", "build-dir", "shader-dir", "validation-manifest", "crt-dir", "output"):
                args.extend(["--" + name, temporary])
            stream = io.StringIO()
            with patch.object(sys, "argv", args), patch.object(package, "prepare_payload") as prepare, redirect_stdout(stream):
                self.assertEqual(package.main(), 1)
            prepare.assert_not_called()
            report = json.loads(stream.getvalue())
            self.assertEqual(report["status"], "FAIL")
            self.assertEqual(report["qualification"], "NOT_RUN")


if __name__ == "__main__":
    unittest.main()
