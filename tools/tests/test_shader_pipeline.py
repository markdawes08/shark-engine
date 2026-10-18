"""CPU behavior tests for offline shader recipes, generations and failures."""

from __future__ import annotations

from contextlib import redirect_stdout
import copy
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import shader_pipeline as pipeline
from process_runner import ProcessResult


def shader_binary(entry="main", stage=0, bindings=()):
    name = entry.encode() + b"\0"
    name += b"\0" * (-len(name) % 4)
    args = [stage, 1, *struct.unpack(f"<{len(name) // 4}I", name)]
    words = [0x07230203, 0x10600, 0, 64, 0, ((len(args) + 1) << 16) | 15, *args]
    for index, binding in enumerate(bindings, 2):
        words.extend([(4 << 16) | 71, index, 33, binding, (4 << 16) | 71, index, 34, 0])
    return struct.pack(f"<{len(words)}I", *words)


class ShaderPipelineTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="shark-shader-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.recipe = self.source / "recipe.json"
        self.output = self.root / "output"
        self.receipt = self.root / "validation.json"
        self.definition = {"schema_version": 1, "convention": pipeline.CONVENTION,
                           "shaders": [{"id": "fixture-vs", "source": "main.hlsl", "entry": "main",
                                        "stage": "vertex", "bindings": []}]}
        self.save_recipe()
        (self.source / "main.hlsl").write_text('#include "include/value.hlsli"\nfloat4 main() : SV_Position { return value; }\n')
        (self.source / "include").mkdir()
        (self.source / "include/value.hlsli").write_text("static const float4 value = 1;\n")
        self.identity = {"lock_sha256": "a" * 64, "host": "windows-x86_64",
                         "compiler_archive_sha256": "b" * 64, "compiler_files": {"bin/dxc.exe": "c" * 64},
                         "validator_sha256": "d" * 64, "validator_revision": "e" * 40}
        self.calls = []
        self.tools = patch.object(pipeline, "tool_inputs", return_value=(Path("dxc"), Path("spirv-val"), self.identity))
        self.tools.start()
        self.addCleanup(self.tools.stop)
        self.runner = patch.object(pipeline, "run_process", side_effect=self.run_tool)
        self.runner.start()
        self.addCleanup(self.runner.stop)

    def save_recipe(self):
        self.recipe.write_text(json.dumps(self.definition), encoding="utf-8")

    def run_tool(self, command, **kwargs):
        self.calls.append(command)
        if command[0] == "dxc":
            # Header generator word records source content to prove cache changes.
            binary = bytearray(shader_binary())
            content = (Path(kwargs["cwd"]) / "include/value.hlsli").read_bytes()
            struct.pack_into("<I", binary, 8, int(pipeline.digest(content)[:8], 16))
            Path(command[command.index("-Fo") + 1]).write_bytes(binary)
        else:
            self.assertEqual(Path(command[-1]).name, command[-1])
            self.assertTrue((Path(kwargs["cwd"]) / command[-1]).is_file())
        return ProcessResult(command, 0, "", "", False)

    def build(self):
        return pipeline.build(self.recipe, self.output, self.receipt)

    def test_build_then_cache_and_source_free_verify(self):
        first = self.build()
        self.assertEqual(first["disposition"], "BUILT")
        manifest = (self.output / "shader-manifest.json").read_bytes()
        calls = len(self.calls)
        self.assertEqual(self.build()["disposition"], "CACHED")
        self.assertEqual(len(self.calls), calls)
        self.assertEqual((self.output / "shader-manifest.json").read_bytes(), manifest)
        with patch.object(pipeline, "tool_inputs", side_effect=AssertionError("verify must not require tools")):
            self.recipe.unlink()
            self.assertEqual(pipeline.verify(self.output)["generation"], first["generation"])

    def test_include_change_rebuilds_and_unrelated_edit_does_not(self):
        first = self.build()
        (self.source / "unused.hlsl").write_text("not in dependency closure")
        self.assertEqual(self.build()["disposition"], "CACHED")
        (self.source / "include/value.hlsli").write_text("static const float4 value = 2;\n")
        second = self.build()
        self.assertEqual(second["disposition"], "BUILT")
        self.assertNotEqual(first["generation"], second["generation"])
        self.assertTrue((self.output / f"generations/{first['generation']}/fixture-vs.spv").is_file())

    def test_cold_outputs_are_identical(self):
        self.build()
        other = self.root / "second"
        pipeline.build(self.recipe, other, self.receipt)
        self.assertEqual((self.output / "shader-manifest.json").read_bytes(), (other / "shader-manifest.json").read_bytes())

    def test_missing_include_preserves_previous_generation(self):
        self.build()
        previous = (self.output / "shader-manifest.json").read_bytes()
        (self.source / "include/value.hlsli").unlink()
        with self.assertRaisesRegex(pipeline.ShaderError, "Missing shader"):
            self.build()
        self.assertEqual((self.output / "shader-manifest.json").read_bytes(), previous)
        pipeline.verify(self.output)

    def test_compile_validate_timeout_and_cleanup_failures_preserve_last_good(self):
        self.build()
        previous = (self.output / "shader-manifest.json").read_bytes()
        (self.source / "include/value.hlsli").write_text("changed")
        for mode in ("compile", "validate", "timeout", "cleanup"):
            def broken(command, **kwargs):
                result = self.run_tool(command, **kwargs)
                if (mode == "validate") == (command[0] == "spirv-val") or mode in ("timeout", "cleanup"):
                    return ProcessResult(command, 1 if mode in ("compile", "validate") else 0,
                                         "", "error: bad fixture", mode == "timeout",
                                         ("cleanup failed",) if mode == "cleanup" else ())
                return result
            with self.subTest(mode=mode), patch.object(pipeline, "run_process", side_effect=broken):
                with self.assertRaises(pipeline.ShaderError):
                    self.build()
            self.assertEqual((self.output / "shader-manifest.json").read_bytes(), previous)
            self.assertFalse((self.output / ".shader-build-lock").exists())
            self.assertFalse(list(self.output.glob(".shader-staging-*")))

    def test_missing_or_malformed_compiler_output_never_publishes(self):
        for content in (None, b"not SPIR-V", struct.pack("<6I", 0x07230203, 0x10600, 0, 64, 0, 0)):
            def broken(command, **kwargs):
                if command[0] == "dxc" and content is not None:
                    Path(command[-1]).write_bytes(content)
                return ProcessResult(command, 0, "", "", False)
            with self.subTest(content=content), patch.object(pipeline, "run_process", side_effect=broken):
                with self.assertRaises(pipeline.ShaderError):
                    self.build()
            self.assertFalse((self.output / "shader-manifest.json").exists())

    def test_changed_source_during_compile_never_publishes(self):
        def changed(command, **kwargs):
            result = self.run_tool(command, **kwargs)
            (self.source / "include/value.hlsli").write_text("changed while compiling")
            return result
        with patch.object(pipeline, "run_process", side_effect=changed):
            with self.assertRaisesRegex(pipeline.ShaderError, "changed during build"):
                self.build()
        self.assertFalse((self.output / "shader-manifest.json").exists())

    def test_source_change_during_cache_validation_is_rejected(self):
        self.build()
        original_verify = pipeline.verify
        def changed(output):
            result = original_verify(output)
            (self.source / "include/value.hlsli").write_text("changed while verifying cache")
            return result
        with patch.object(pipeline, "verify", side_effect=changed):
            with self.assertRaisesRegex(pipeline.ShaderError, "changed during build"):
                self.build()

    def test_tool_change_during_compile_never_publishes(self):
        changed = {**self.identity, "validator_sha256": "f" * 64}
        with patch.object(pipeline, "tool_inputs", side_effect=[
                (Path("dxc"), Path("spirv-val"), self.identity),
                (Path("dxc"), Path("spirv-val"), changed)]):
            with self.assertRaisesRegex(pipeline.ShaderError, "changed during build"):
                self.build()
        self.assertFalse((self.output / "shader-manifest.json").exists())

    def test_tampered_or_missing_artifact_is_not_silently_repaired(self):
        self.build()
        manifest = pipeline.verify(self.output)
        artifact = self.output / manifest["shaders"][0]["file"]
        original = artifact.read_bytes()
        for changed in (b"tampered", None):
            artifact.write_bytes(original)
            if changed is None:
                artifact.unlink()
            else:
                artifact.write_bytes(changed)
            with self.subTest(changed=changed):
                with self.assertRaises(pipeline.ShaderError):
                    self.build()
                with self.assertRaises(pipeline.ShaderError):
                    pipeline.verify(self.output)

    def test_extra_artifact_generation_files_rejected(self):
        self.build()
        manifest = pipeline.verify(self.output)
        (self.output / "generations" / manifest["generation"] / "dxc.exe").write_bytes(b"forbidden")
        with self.assertRaisesRegex(pipeline.ShaderError, "inventory"):
            pipeline.verify(self.output)

    def test_recipe_version_unknown_fields_duplicates_and_ranges_rejected(self):
        mutations = [lambda row: row.update(schema_version=True), lambda row: row.update(schema_version=2),
                     lambda row: row.update(unknown=True), lambda row: row["shaders"].append(copy.deepcopy(row["shaders"][0])),
                     lambda row: row["shaders"][0].update(stage="geometry"),
                     lambda row: row["shaders"][0].update(bindings=[{"kind": "texture", "slot": 128}]),
                     lambda row: row["shaders"][0].update(bindings=[{"kind": "texture", "slot": True}])]
        for change in mutations:
            candidate = copy.deepcopy(self.definition)
            change(candidate)
            with self.subTest(candidate=candidate), self.assertRaises(pipeline.ShaderError):
                pipeline.validate_recipe_value(candidate)
        self.recipe.write_text('{"schema_version":1,"schema_version":1}')
        with self.assertRaisesRegex(pipeline.ShaderError, "Duplicate"):
            pipeline.recipe(self.recipe)

    def test_paths_reject_escape_ambiguous_devices_and_links(self):
        for path in ("../outside", "/root", "C:/drive", "a\\b", "./same", "a//b", "NUL.hlsl", "a./b", "a:b"):
            with self.subTest(path=path), self.assertRaises(pipeline.ShaderError):
                pipeline.relative(path)
        with patch.object(Path, "is_symlink", return_value=True):
            with self.assertRaises(pipeline.ShaderError):
                pipeline.contained(self.source, "main.hlsl")

    def test_source_preprocessor_subset_cycles_and_budgets(self):
        for directive in ('#include "../escape"', '#include <system>', '#include MACRO', '#define X 1', '#if X'):
            (self.source / "include/value.hlsli").write_text(directive)
            with self.subTest(directive=directive), self.assertRaises(pipeline.ShaderError):
                pipeline.source_closure(self.source, self.definition)
        (self.source / "include/value.hlsli").write_text('#include "value.hlsli"')
        with self.assertRaisesRegex(pipeline.ShaderError, "cycle"):
            pipeline.source_closure(self.source, self.definition)
        (self.source / "include/value.hlsli").write_text("hello")
        with patch.object(pipeline, "MAX_SOURCE", 4):
            with self.assertRaises(pipeline.ShaderError):
                pipeline.source_closure(self.source, self.definition)

    def test_hidden_directives_and_nondeterministic_builtins_rejected(self):
        for source in ('\ufeff#include "absent.hlsli"', '#inc\\\nlude "absent.hlsli"',
                       '#inc\\\r\nlude "absent.hlsli"', 'const char* now = __TIME__;',
                       '__DATE__', '__TIMESTAMP__', '__FILE__', '\0'):
            (self.source / "include/value.hlsli").write_text(source, encoding="utf-8")
            with self.subTest(source=source), self.assertRaises(pipeline.ShaderError):
                pipeline.source_closure(self.source, self.definition)

    def test_shader_source_argument_cannot_be_interpreted_as_a_flag(self):
        (self.source / "main.hlsl").rename(self.source / "-option.hlsl")
        self.definition["shaders"][0]["source"] = "-option.hlsl"
        self.save_recipe()
        self.build()
        self.assertIn("./-option.hlsl", self.calls[0])

    def test_commented_includes_do_not_add_dependencies(self):
        source = '// #include "absent"\n/*\n#include "also-absent"\n*/\n#include "real.hlsli" // comment\n'
        self.assertEqual(pipeline.directives(source, "fixture"), ["real.hlsli"])

    def test_layout_and_stage_mismatches_rejected(self):
        shader = self.definition["shaders"][0]
        pipeline.inspect_spirv(shader_binary(), shader)
        for data in (shader_binary(stage=4), shader_binary(entry="different"), shader_binary(bindings=(0,))):
            with self.assertRaisesRegex(pipeline.ShaderError, "differs|differ"):
                pipeline.inspect_spirv(data, shader)
        with_binding = {**shader, "bindings": [{"kind": "constant_buffer", "slot": 0}]}
        pipeline.inspect_spirv(shader_binary(bindings=(256,)), with_binding)
        with self.assertRaises(pipeline.ShaderError):
            pipeline.inspect_spirv(shader_binary(bindings=(0,)), with_binding)

    def test_manifest_strict_schema_digest_and_inventory(self):
        self.build()
        path = self.output / "shader-manifest.json"
        original = json.loads(path.read_bytes())
        for mutate in (lambda row: row.update(schema_version=True), lambda row: row.update(unknown=True),
                       lambda row: row.update(generation="f" * 64),
                       lambda row: row["inputs"]["toolchain"].update(extra="bad"),
                       lambda row: row["shaders"][0].update(file="../escape")):
            modified = copy.deepcopy(original)
            mutate(modified)
            path.write_bytes(pipeline.canonical(modified))
            with self.assertRaises(pipeline.ShaderError):
                pipeline.verify(self.output)

    def test_output_owner_conflicts_and_source_overlap_rejected(self):
        self.output.mkdir()
        (self.output / ".shader-build-lock").mkdir()
        with self.assertRaisesRegex(pipeline.ShaderError, "Another shader build"):
            self.build()
        with self.assertRaisesRegex(pipeline.ShaderError, "separate"):
            pipeline.build(self.recipe, self.source / "output", self.receipt)

    def test_cli_failure_is_machine_readable_nonzero(self):
        stream = io.StringIO()
        with redirect_stdout(stream):
            self.assertEqual(pipeline.main(["verify", "--output", str(self.output)]), 1)
        result = json.loads(stream.getvalue())
        self.assertEqual(result["schema_version"], 1)
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("code", result["error"])


class ShaderToolInputTests(unittest.TestCase):
    def test_compiler_only_receipt_and_explicit_offline_preparation(self):
        with tempfile.TemporaryDirectory(prefix="shark-shader-tool-input-") as temporary:
            root = Path(temporary)
            validator = root / "spirv-val"
            validator.write_bytes(b"validator")
            lock = {"graphics_validation": {"dependencies": {"SPIRVTools": {"commit": "e" * 40}}},
                    "graphics_tools": {"DXC": {"artifacts": {"windows-x86_64": {}}}}}
            (root / "dependencies.lock.json").write_bytes(pipeline.canonical(lock))
            receipt = root / "receipt.json"
            record = {"schema_version": 1, "status": "PASS",
                      "dependency_lock_sha256": pipeline.sha256(root / "dependencies.lock.json"),
                      "tools": {"spirv_val": {"path": str(validator), "sha256": pipeline.sha256(validator),
                                                "version": "SPIRV-Tools " + "e" * 40}}}
            receipt.write_bytes(pipeline.canonical(record))
            with patch.object(pipeline, "ROOT", root), patch.object(pipeline, "native_host", return_value="windows-x86_64"), \
                    patch.object(pipeline, "prepare", return_value=(root / "dxc", {
                        "archive_sha256": "a" * 64, "files": {"dxc": "b" * 64}})) as prepare:
                _, actual, _ = pipeline.tool_inputs(receipt)
                self.assertEqual(actual, validator)
                self.assertTrue(prepare.call_args.kwargs["offline"])
                validator.write_bytes(b"tampered")
                with self.assertRaises(pipeline.ShaderError):
                    pipeline.tool_inputs(receipt)
                validator.write_bytes(b"validator")
                record["dependency_lock_sha256"] = "f" * 64
                receipt.write_bytes(pipeline.canonical(record))
                with self.assertRaisesRegex(pipeline.ShaderError, "passing/current"):
                    pipeline.tool_inputs(receipt)


if __name__ == "__main__":
    unittest.main()
