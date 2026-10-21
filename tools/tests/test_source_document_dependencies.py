"""Source JSON is pinned, private to Tools and absent from protected builds."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from source_document_dependency import populate


class SourceDocumentDependencyTests(unittest.TestCase):
    def test_population_rejects_missing_added_and_modified_inputs(self):
        with tempfile.TemporaryDirectory(prefix="shark-json-source-integrity-") as temporary:
            build = Path(temporary)
            original = populate(build, OPTIONS.json_archive)
            self.assertEqual(populate(build, OPTIONS.json_archive), original)
            source = build / "_deps/json-src"
            header = source / "include/nlohmann/json.hpp"
            content = header.read_bytes()
            for mutation in ("missing", "modified", "added"):
                with self.subTest(mutation=mutation):
                    if mutation == "missing":
                        header.unlink()
                    elif mutation == "modified":
                        header.write_bytes(content + b"// changed\n")
                    else:
                        (source / "extra.hpp").write_text("// injected\n", encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, "Populated JSON source"):
                        populate(build, OPTIONS.json_archive)
                    header.write_bytes(content)
                    (source / "extra.hpp").unlink(missing_ok=True)

    def configure(self, extra, expected=None, *, tools=True, client=False, editor=False, drift=False):
        with tempfile.TemporaryDirectory(prefix="shark-source-dependencies-") as temporary:
            build = Path(temporary) / "build"
            command = [OPTIONS.cmake, "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
                       f"-DCMAKE_MAKE_PROGRAM={OPTIONS.ninja}",
                       f"-DCMAKE_CXX_COMPILER={OPTIONS.compiler}",
                       f"-DCMAKE_C_COMPILER={OPTIONS.c_compiler}",
                       f"-DSHARK_BUILD_CLIENT={'ON' if client else 'OFF'}",
                       f"-DSHARK_BUILD_EDITOR={'ON' if editor else 'OFF'}",
                       f"-DSHARK_BUILD_TOOLS={'ON' if tools else 'OFF'}",
                       "-DSHARK_BUILD_SERVER=ON", "-DSHARK_BUILD_TESTS=OFF",
                       "-DSHARK_NATIVE_WINDOW=OFF", "-DSHARK_NATIVE_GRAPHICS=OFF",
                       "-DFETCHCONTENT_FULLY_DISCONNECTED=ON", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                       f"-DSHARK_ENTT_ARCHIVE={OPTIONS.entt_archive}",
                       f"-DSHARK_GLM_ARCHIVE={OPTIONS.glm_archive}",
                       f"-DSHARK_MBEDTLS_ARCHIVE={OPTIONS.mbedtls_archive}",
                       f"-DSHARK_JSON_ARCHIVE={OPTIONS.json_archive}", *extra]
            result = subprocess.run(command, text=True, capture_output=True, timeout=120)
            output = result.stdout + result.stderr
            if expected:
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(expected, " ".join(output.split()))
                self.assertFalse((build / "_deps").exists(), output)
                return
            self.assertEqual(result.returncode, 0, output)
            dependencies = {path.name for path in (build / "_deps").iterdir()}
            expected_dependencies = {"entt-src", "entt-build", "entt-subbuild", "glm-src", "glm-build",
                                     "glm-subbuild", "mbedtls-src", "mbedtls-build"}
            if tools:
                expected_dependencies.add("json-src")
            self.assertEqual(dependencies, expected_dependencies)
            cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
            self.assertNotIn("JSON_BuildTests:", cache)
            self.assertNotIn("JSON_Install:", cache)
            graph_path = next(build.glob("shark-target-graph-*.cmake"))
            graph = graph_path.read_text(encoding="utf-8")
            commands = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
            tool_commands = []
            for entry in commands:
                source = Path(entry["file"])
                command_text = entry["command"].replace("\\", "/")
                if source.is_relative_to(ROOT / "engine_tools"):
                    tool_commands.append(entry)
                    self.assertIn("/json-src/include", command_text)
                    self.assertIn("JSON_USE_IMPLICIT_CONVERSIONS=0", command_text)
                    system_flag = (r"(?:/|-)external:I" if OPTIONS.compiler.lower().endswith(("cl", "cl.exe"))
                                   else r"-isystem\s+")
                    self.assertRegex(command_text, system_flag + r'"?' +
                                     re.escape((build / "_deps/json-src/include").as_posix()))
                else:
                    self.assertNotIn("json-src", command_text)
                    self.assertNotIn("JSON_USE_IMPLICIT_CONVERSIONS", command_text)
            self.assertEqual(len(tool_commands), 2 if tools else 0)
            if tools:
                self.assertIn("SHARK_DOMAIN_nlohmann_json [==[authoring]==]", graph)
                self.assertIn("SHARK_DOMAIN_nlohmann_json::nlohmann_json [==[authoring]==]", graph)
            else:
                self.assertNotIn("nlohmann", graph)
                self.assertNotIn("SHARK_JSON_SOURCE_DIR:", cache)
            check = subprocess.run([OPTIONS.cmake, "-DSHARK_GRAPH=" + str(graph_path), "-P",
                                    str(ROOT / "cmake/CheckRuntimeBoundary.cmake")],
                                   text=True, capture_output=True, timeout=15)
            self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
            if drift:
                header = build / "_deps/json-src/include/nlohmann/json.hpp"
                header.write_bytes(header.read_bytes() + b"\n// unexpected modification\n")
                retry = subprocess.run(command, text=True, capture_output=True, timeout=120)
                self.assertNotEqual(retry.returncode, 0)
                self.assertIn("Populated JSON source differs", retry.stdout + retry.stderr)

    def test_cpu_authoring_without_editor_or_client_uses_verified_private_json(self):
        self.configure([], drift=True)

    def test_editor_consumer_does_not_inherit_json(self):
        self.configure([], client=True, editor=True)

    def test_core_server_and_runtime_ignore_unneeded_json_inputs(self):
        # One CPU graph contains Core, Server and Runtime; the existing separate
        # server-only dependency fixture also verifies its exact population.
        self.configure(["-DSHARK_JSON_ARCHIVE=missing-json-archive",
                        f"-DFETCHCONTENT_SOURCE_DIR_JSON={ROOT}"], tools=False, client=True)

    def test_editor_requires_tools_before_dependency_population(self):
        self.configure([], "SHARK_BUILD_EDITOR requires SHARK_BUILD_TOOLS=ON",
                       tools=False, client=True, editor=True)

    def test_source_overrides_and_invalid_archives_reject_before_population(self):
        cases = [(["-DSHARK_JSON_ARCHIVE="], "Offline JSON population requires SHARK_JSON_ARCHIVE"),
                 (["-DSHARK_JSON_ARCHIVE=missing-json-archive"], "Offline JSON population requires"),
                 ([f"-DSHARK_JSON_ARCHIVE={ROOT / 'dependencies.lock.json'}"], "JSON archive SHA-256 differs"),
                 ([f"-DSHARK_JSON_ARCHIVE={ROOT}"], "must name a regular locked archive")]
        for name in ("FETCHCONTENT_SOURCE_DIR_JSON", "FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON",
                     "nlohmann_json_DIR", "JSON_SOURCE_DIR"):
            cases.append(([f"-D{name}={ROOT}"], "Unverified JSON source override"))
        for arguments, diagnostic in cases:
            with self.subTest(arguments=arguments):
                self.configure(arguments, diagnostic)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("cmake", "ninja", "compiler", "c-compiler"):
        parser.add_argument("--" + option, required=True)
    for option in ("mbedtls-archive", "entt-archive", "glm-archive", "json-archive"):
        parser.add_argument("--" + option, type=Path, required=True)
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
