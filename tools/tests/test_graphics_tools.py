"""Offline regression checks for locked shader tool preparation boundaries."""

from pathlib import Path
from contextlib import redirect_stdout
import io
import json
import stat
import struct
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import warnings
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import prepare_graphics_tools as setup
from process_runner import ProcessResult


class GraphicsToolsTests(unittest.TestCase):
    def archive(self, members, *, format="zip"):
        stream = io.BytesIO()
        if format == "zip":
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                with zipfile.ZipFile(stream, "w") as archive:
                    for name, data in members:
                        member = zipfile.ZipInfo(name)
                        member.filename = name  # Preserve deliberately malformed separators.
                        archive.writestr(member, data)
        else:
            with tarfile.open(fileobj=stream, mode="w:gz") as archive:
                for name, data in members:
                    member = tarfile.TarInfo(name)
                    member.size = len(data)
                    archive.addfile(member, io.BytesIO(data))
        data = stream.getvalue()
        artifact = {"sha256": setup.digest(data), "archive_format": format,
                    "files": ["bin/dxc", "LICENSE.txt"], "executable": "bin/dxc",
                    "url": "https://github.com/microsoft/DirectXShaderCompiler/releases/download/test/test.zip"}
        return data, artifact

    def fixture(self, **kwargs):
        return self.archive([("bin/dxc", b"compiler"), ("LICENSE.txt", b"license"),
                             ("unselected.h", b"header")], **kwargs)

    def test_zip_and_tar_extract_only_locked_regular_files(self):
        for format in ("zip", "tar.gz"):
            with self.subTest(format=format):
                data, artifact = self.fixture(format=format)
                self.assertEqual(setup.selected_files(data, artifact),
                                 {"bin/dxc": b"compiler", "LICENSE.txt": b"license"})

    def test_changed_archive_digest_rejected_before_extraction(self):
        data, artifact = self.fixture()
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            setup.selected_files(data + b"tampered", artifact)

    def test_path_escape_rejected_even_for_unselected_members(self):
        for name in ("../outside", "/outside", "C:/outside", "..\\outside"):
            for format in ("zip", "tar.gz"):
                with self.subTest(name=name, format=format):
                    data, artifact = self.archive([("bin/dxc", b"c"), ("LICENSE.txt", b"l"),
                                                   (name, b"bad")], format=format)
                    with self.assertRaisesRegex(ValueError, "Unsafe"):
                        setup.selected_files(data, artifact)

    def test_duplicate_members_rejected(self):
        for format in ("zip", "tar.gz"):
            data, artifact = self.archive([("bin/dxc", b"one"), ("bin/dxc", b"two"),
                                           ("LICENSE.txt", b"l")], format=format)
            with self.assertRaisesRegex(ValueError, "Duplicate"):
                setup.selected_files(data, artifact)

    def test_links_rejected(self):
        for format in ("zip", "tar.gz"):
            stream = io.BytesIO()
            if format == "zip":
                with zipfile.ZipFile(stream, "w") as archive:
                    member = zipfile.ZipInfo("bin/dxc")
                    member.create_system = 3
                    member.external_attr = (stat.S_IFLNK | 0o777) << 16
                    archive.writestr(member, "outside")
            else:
                with tarfile.open(fileobj=stream, mode="w:gz") as archive:
                    member = tarfile.TarInfo("bin/dxc")
                    member.type = tarfile.SYMTYPE
                    member.linkname = "outside"
                    archive.addfile(member)
            data = stream.getvalue()
            artifact = {"sha256": setup.digest(data), "archive_format": format,
                        "files": ["bin/dxc"]}
            with self.assertRaisesRegex(ValueError, "nonregular"):
                setup.selected_files(data, artifact)

    def test_missing_license_rejected(self):
        data, artifact = self.archive([("bin/dxc", b"compiler")])
        with self.assertRaisesRegex(ValueError, "missing required"):
            setup.selected_files(data, artifact)

    def test_extraction_budgets_enforced(self):
        data, artifact = self.fixture()
        for key, limit in (("MAX_MEMBER", 2), ("MAX_EXTRACTED", 10), ("MAX_MEMBERS", 1)):
            with self.subTest(limit=key), patch.object(setup, key, limit):
                with self.assertRaisesRegex(ValueError, "budget|count"):
                    setup.selected_files(data, artifact)

    def test_cached_tools_are_reverified_and_modification_fails(self):
        data, artifact = self.fixture()
        with tempfile.TemporaryDirectory(prefix="shark-shader-tool-test-") as temporary:
            cache = Path(temporary)
            (cache / (artifact["sha256"] + ".archive")).write_bytes(data)
            tool = {"version": "test", "artifacts": {"fixture": artifact}}
            with patch.object(setup.urllib.request, "urlopen", side_effect=AssertionError("No download allowed")), \
                    patch.object(setup.tempfile, "mkdtemp", side_effect=AssertionError("Persistent cache must inherit its parent ACL")):
                compiler, evidence = setup.prepare(tool, "fixture", cache, offline=True)
                self.assertEqual(evidence["files"]["bin/dxc"], setup.digest(b"compiler"))
                setup.prepare(tool, "fixture", cache, offline=True)
                compiler.write_bytes(b"modified")
                with self.assertRaisesRegex(ValueError, "hash mismatch"):
                    setup.prepare(tool, "fixture", cache, offline=True)

    def test_unexpected_cached_library_fails(self):
        data, artifact = self.fixture()
        with tempfile.TemporaryDirectory(prefix="shark-shader-tool-test-") as temporary:
            cache = Path(temporary)
            (cache / (artifact["sha256"] + ".archive")).write_bytes(data)
            tool = {"version": "test", "artifacts": {"fixture": artifact}}
            compiler, _ = setup.prepare(tool, "fixture", cache, offline=True)
            (compiler.parent / "unexpected.dll").write_bytes(b"unlocked")
            with self.assertRaisesRegex(ValueError, "inventory"):
                setup.prepare(tool, "fixture", cache, offline=True)

    def test_offline_cache_miss_never_downloads(self):
        _, artifact = self.fixture()
        with tempfile.TemporaryDirectory(prefix="shark-shader-tool-test-") as temporary:
            with patch.object(setup.urllib.request, "urlopen", side_effect=AssertionError("No download allowed")):
                with self.assertRaisesRegex(ValueError, "not cached"):
                    setup.prepare({"version": "test", "artifacts": {"fixture": artifact}},
                                  "fixture", Path(temporary), offline=True)

    def test_unavailable_native_macos_tool_reports_not_run_without_download(self):
        with tempfile.TemporaryDirectory(prefix="shark-shader-tool-test-") as temporary:
            output = Path(temporary) / "evidence"
            with patch.object(setup, "native_host", return_value="darwin-arm64"), \
                    patch.object(sys, "argv", ["prepare_graphics_tools.py", "--output", str(output)]), \
                    patch.object(setup.urllib.request, "urlopen", side_effect=AssertionError("No download allowed")), \
                    redirect_stdout(io.StringIO()):
                self.assertEqual(setup.main(), 3)
            report = json.loads((output / "summary.json").read_text())
            self.assertEqual(report["status"], "NOT_RUN")
            self.assertNotIn("compile", report)

    def test_existing_evidence_is_preserved_and_no_tools_run(self):
        with tempfile.TemporaryDirectory(prefix="shark-shader-tool-test-") as temporary:
            output = Path(temporary)
            previous = output / "summary.json"
            previous.write_text('{"status":"PASS"}\n')
            with patch.object(sys, "argv", ["prepare_graphics_tools.py", "--output", temporary]), \
                    patch.object(setup, "prepare", side_effect=AssertionError("No tool preparation allowed")), \
                    redirect_stdout(io.StringIO()):
                self.assertEqual(setup.main(), 2)
            self.assertEqual(previous.read_text(), '{"status":"PASS"}\n')

    def test_cleanup_refuses_cache_root_and_outside_directory(self):
        with tempfile.TemporaryDirectory(prefix="shark-shader-tool-test-") as temporary:
            root = Path(temporary)
            cache = root / "cache"
            cache.mkdir()
            outside = root / "outside"
            outside.mkdir()
            (outside / "keep").write_bytes(b"preserve")
            for invalid in (cache, outside):
                with self.assertRaisesRegex(ValueError, "Refusing recursive cleanup"):
                    setup.remove_staging(invalid, cache)
            self.assertEqual((outside / "keep").read_bytes(), b"preserve")

    def test_crashed_compiler_is_not_invalid_shader_rejection(self):
        def compiler(command, **unused):
            if "--version" in command:
                return ProcessResult(command, 0, "fixture version", "", False)
            if any(str(argument).endswith("invalid.hlsl") for argument in command):
                return ProcessResult(command, -11, "", "error: crash", False)
            Path(command[-1]).write_bytes(struct.pack("<5I", 0x07230203, 0x10600, 0, 1, 0))
            return ProcessResult(command, 0, "", "", False)

        with tempfile.TemporaryDirectory(prefix="shark-shader-tool-test-") as temporary:
            with patch.object(setup, "run_process", side_effect=compiler):
                evidence = {}
                with self.assertRaisesRegex(ValueError, "reject the invalid shader"):
                    setup.compile_fixture(Path("fixture-dxc"), Path(temporary), evidence)
                self.assertEqual(evidence["status"], "FAIL")
                self.assertEqual(evidence["commands"][-1]["returncode"], -11)

    def case_compiler(self, command, **unused):
        if command[0].endswith("spirv-val"):
            if command[-1].endswith("invalid-case-binary.spv"):
                return ProcessResult(command, 1, "", "error: zero instruction word count", False)
            return ProcessResult(command, 0, "", "", False)
        if any(str(argument).endswith("invalid-case.hlsl") for argument in command):
            code = 0x80004005 if setup.os.name == "nt" else 5
            return ProcessResult(command, code, "", "error: invalid token", False)
        Path(command[-1]).write_bytes(struct.pack("<5I", 0x07230203, 0x10600, 0, 1, 0))
        return ProcessResult(command, 0, "", "", False)

    def case_setup(self, directory):
        executable = directory / "dxc"
        executable.write_bytes(b"fixture compiler")
        validator = directory / "spirv-val"
        validator.write_bytes(b"fixture validator")
        source = directory / "probes/graphics/shaders/cases.hlsl"
        source.parent.mkdir(parents=True)
        source.write_text("fixture source")
        manifest = directory / "validation.json"
        manifest.write_text("{}")
        tools = {"tools": {"spirv_val": {"path": str(validator)}}}
        output = directory / "output"
        output.mkdir()
        return executable, manifest, tools, output

    def test_case_preparation_publishes_only_validated_repeatable_shaders(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable, receipt, tools, output = self.case_setup(directory)
            with patch.object(setup, "ROOT", directory), patch.object(setup, "validation_tools", return_value=tools), \
                    patch.object(setup, "run_process", side_effect=self.case_compiler):
                result = setup.compile_cases(executable, output, receipt, "a" * 64)
            manifest = json.loads((output / "shaders/shader-manifest.json").read_text())
            self.assertEqual(result["status"], "PASS")
            self.assertEqual(len(manifest["shaders"]), 4)
            self.assertTrue(manifest["invalid_shader_rejected"])
            self.assertTrue(manifest["invalid_spirv_rejected"])
            self.assertTrue(all(shader["validation_status"] == "PASS" for shader in manifest["shaders"]))

    def test_independent_validator_failure_never_publishes_passing_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable, receipt, tools, output = self.case_setup(directory)

            def invalid(command, **kwargs):
                if command[0].endswith("spirv-val"):
                    return ProcessResult(command, 1, "", "error: invalid shader", False)
                return self.case_compiler(command, **kwargs)

            with patch.object(setup, "ROOT", directory), patch.object(setup, "validation_tools", return_value=tools), \
                    patch.object(setup, "run_process", side_effect=invalid):
                with self.assertRaisesRegex(ValueError, "Independent SPIR-V validation failed"):
                    setup.compile_cases(executable, output, receipt, "a" * 64)
            self.assertFalse((output / "shaders/shader-manifest.json").exists())

    def test_validator_crash_is_not_successful_corrupt_shader_rejection(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            executable, receipt, tools, output = self.case_setup(directory)

            def crashed(command, **kwargs):
                if command[-1].endswith("invalid-case-binary.spv"):
                    return ProcessResult(command, -11, "", "error: crash", False)
                return self.case_compiler(command, **kwargs)

            with patch.object(setup, "ROOT", directory), patch.object(setup, "validation_tools", return_value=tools), \
                    patch.object(setup, "run_process", side_effect=crashed):
                with self.assertRaisesRegex(ValueError, "did not reject the corrupt binary normally"):
                    setup.compile_cases(executable, output, receipt, "a" * 64)
            self.assertFalse((output / "shaders/shader-manifest.json").exists())

    def test_compiler_nonrepeatability_and_timeout_fail_before_publishing(self):
        for problem in ("changed", "timeout", "cleanup"):
            with self.subTest(problem=problem), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                executable, receipt, tools, output = self.case_setup(directory)

                def broken(command, **kwargs):
                    result = self.case_compiler(command, **kwargs)
                    if command[-1].endswith(".repeat"):
                        if problem == "changed":
                            Path(command[-1]).write_bytes(b"changed")
                        elif problem == "timeout":
                            return ProcessResult(command, 124, "", "", True)
                        else:
                            return ProcessResult(command, 0, "", "", False, ("cleanup failed",))
                    return result

                with patch.object(setup, "ROOT", directory), patch.object(setup, "validation_tools", return_value=tools), \
                        patch.object(setup, "run_process", side_effect=broken):
                    with self.assertRaises(ValueError):
                        setup.compile_cases(executable, output, receipt, "a" * 64)
                self.assertFalse((output / "shaders/shader-manifest.json").exists())


if __name__ == "__main__":
    unittest.main()
