"""Core EnTT/GLM population stays pinned, CPU-only and disconnected-capable."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class SceneDependencyTests(unittest.TestCase):
    def configure(self, extra, expected=None, *, header_checks=False, reject_tls_drift=False):
        with tempfile.TemporaryDirectory(prefix="shark-scene-dependencies-") as temporary:
            build = Path(temporary) / "build"
            command = [OPTIONS.cmake, "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
                       f"-DCMAKE_MAKE_PROGRAM={OPTIONS.ninja}",
                       f"-DCMAKE_CXX_COMPILER={OPTIONS.compiler}",
                       f"-DCMAKE_C_COMPILER={OPTIONS.c_compiler}",
                       "-DSHARK_BUILD_CLIENT=OFF", f"-DSHARK_BUILD_SERVER={'ON' if OPTIONS.sqlite_archive else 'OFF'}",
                       "-DSHARK_BUILD_EDITOR=OFF", "-DSHARK_BUILD_TESTS=OFF",
                       "-DFETCHCONTENT_FULLY_DISCONNECTED=ON", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                       f"-DSHARK_ENTT_ARCHIVE={OPTIONS.entt_archive}",
                       f"-DSHARK_GLM_ARCHIVE={OPTIONS.glm_archive}",
                       f"-DSHARK_MBEDTLS_ARCHIVE={OPTIONS.mbedtls_archive}",
                       f"-DSHARK_JOLT_ARCHIVE={OPTIONS.jolt_archive}",
                       *([f"-DSHARK_SQLITE_ARCHIVE={OPTIONS.sqlite_archive}"] if OPTIONS.sqlite_archive else []), *extra]
            result = subprocess.run(command, text=True, capture_output=True, timeout=120)
            text = result.stdout + result.stderr
            if expected:
                self.assertNotEqual(result.returncode, 0, text)
                self.assertIn(expected, " ".join(text.split()))
                self.assertFalse((build / "_deps").exists(), text)
            else:
                self.assertEqual(result.returncode, 0, text)
                self.assertEqual({p.name for p in (build / "_deps").iterdir()},
                                 {"entt-src", "entt-build", "entt-subbuild", "glm-src", "glm-build", "glm-subbuild", "mbedtls-src", "mbedtls-build", "jolt-src", "jolt-build"} |
                                 ({"sqlite-src"} if OPTIONS.sqlite_archive else set()))
                cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
                self.assertIn("FETCHCONTENT_FULLY_DISCONNECTED:BOOL=ON", cache)
                self.assertNotIn("ENTT_BUILD_TESTING:BOOL", cache)
                self.assertNotIn("GLM_BUILD_LIBRARY:BOOL", cache)
                graph = next(build.glob("shark-target-graph-*.cmake")).read_text(encoding="utf-8")
                self.assertIn("SHARK_DOMAIN_EnTT [==[core]==]", graph)
                self.assertIn("SHARK_DOMAIN_EnTT::EnTT [==[core]==]", graph)
                self.assertIn("SHARK_DOMAIN_SharkMath [==[core]==]", graph)
                self.assertIn("SHARK_DOMAIN_SharkTLS_mbedtls [==[core]==]", graph)
                self.assertIn("SHARK_DOMAIN_Jolt [==[core]==]", graph)
                self.assertIn("GEN_FILES:BOOL=OFF", cache)
                self.assertIn("USE_SHARED_MBEDTLS_LIBRARY:BOOL=OFF", cache)
                commands = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
                for item in commands:
                    if Path(item["file"]).suffix == ".c" or Path(item["file"]).is_relative_to(build / "_deps/jolt-src"):
                        continue
                    for definition in ("GLM_FORCE_RADIANS", "GLM_FORCE_DEPTH_ZERO_TO_ONE",
                                       "GLM_FORCE_EXPLICIT_CTOR", "GLM_FORCE_CTOR_INIT", "GLM_FORCE_PURE"):
                        self.assertIn(definition, item["command"])
                    if Path(item["file"]).name == "main.cpp":
                        self.assertNotIn("entt-src", item["command"])
                        self.assertIn("glm-src", item["command"])
                        self.assertNotIn("mbedtls-src", item["command"])
                        self.assertNotIn("jolt-src", item["command"])
                        self.assertNotIn("MBEDTLS_USER_CONFIG_FILE", item["command"])
                check = subprocess.run([OPTIONS.cmake, "-DSHARK_GRAPH=" + str(next(build.glob("shark-target-graph-*.cmake"))),
                                        "-DSHARK_BOUNDARY_MODE=server", "-P", str(ROOT / "cmake/CheckRuntimeBoundary.cmake")],
                                       text=True, capture_output=True, timeout=15)
                self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
                if header_checks:
                    self.compile_math_guards(build)
                if reject_tls_drift:
                    header = build / "_deps/mbedtls-src/include/mbedtls/ssl.h"
                    header.write_bytes(header.read_bytes() + b"\n// unexpected modification\n")
                    retry = subprocess.run(command, text=True, capture_output=True, timeout=120)
                    self.assertNotEqual(retry.returncode, 0)
                    self.assertIn("Populated MbedTLS source differs", retry.stdout + retry.stderr)

    def compile_math_guards(self, build):
        source = build / "math-consumer.cpp"
        output = build / ("math-consumer.obj" if os.name == "nt" else "math-consumer.o")
        definitions = ("GLM_FORCE_RADIANS", "GLM_FORCE_DEPTH_ZERO_TO_ONE", "GLM_FORCE_EXPLICIT_CTOR",
                       "GLM_FORCE_CTOR_INIT", "GLM_FORCE_PURE")
        if os.name == "nt":
            arguments = [OPTIONS.compiler, "/nologo", "/std:c++20", "/EHsc", "/permissive-", "/Zc:__cplusplus",
                         "/utf-8", "/W4", "/WX", "/external:W0", "/I" + str(ROOT / "engine/core/include"),
                         "/external:I" + str(build / "_deps/glm-src"), "/c", str(source), "/Fo" + str(output)]
            define = "/D"
        else:
            arguments = [OPTIONS.compiler, "-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                         "-I", str(ROOT / "engine/core/include"), "-isystem", str(build / "_deps/glm-src"),
                         "-c", str(source), "-o", str(output)]
            define = "-D"
        body = '#include <shark/math.hpp>\nint main() { shark::math::Vec3 value{1.0, 2.0, 3.0}; return value.x == 1.0 ? 0 : 1; }\n'
        conflicts = ("GLM_FORCE_LEFT_HANDED", "GLM_FORCE_ALIGNED", "GLM_FORCE_ALIGNED_GENTYPES",
                     "GLM_FORCE_DEFAULT_ALIGNED_GENTYPES", "GLM_FORCE_QUAT_DATA_WXYZ", "GLM_FORCE_QUAT_DATA_XYZW",
                     "GLM_FORCE_SIZE_T_LENGTH", "GLM_FORCE_SINGLE_ONLY", "GLM_FORCE_SWIZZLE", "GLM_FORCE_XYZW_ONLY",
                     "GLM_FORCE_INTRINSICS", "GLM_FORCE_SSE", "GLM_FORCE_SSE2", "GLM_FORCE_SSE3", "GLM_FORCE_SSSE3",
                     "GLM_FORCE_SSE41", "GLM_FORCE_SSE42", "GLM_FORCE_AVX", "GLM_FORCE_AVX2", "GLM_FORCE_NEON")
        cases = [("central-definitions", "", definitions, None), ("wrapper-defaults", "", (), None),
                 ("prior-include", "#include <glm/glm.hpp>\n", definitions,
                  "Include shark/math.hpp before any GLM header")]
        cases.extend((name, "#define " + name + "\n", definitions,
                      "GLM configuration conflicts with Shark's packed right-handed scalar math contract")
                     for name in conflicts)
        for name, prefix, values, diagnostic in cases:
            with self.subTest(configuration=name):
                source.write_text(prefix + body, encoding="utf-8")
                result = subprocess.run(arguments + [define + value for value in values],
                                        text=True, capture_output=True, timeout=20)
                message = result.stdout + result.stderr
                if diagnostic is None:
                    self.assertEqual(result.returncode, 0, message)
                    self.assertTrue(output.is_file())
                else:
                    self.assertNotEqual(result.returncode, 0, message)
                    self.assertIn(diagnostic, message)

    def test_public_math_consumer_rejects_conflicting_or_prior_glm_configuration(self):
        self.configure([], header_checks=True)

    def test_verified_archive_populates_offline_with_private_tls_and_no_header_upstream_configuration(self):
        self.configure([f"-DSHARK_ENTT_ARCHIVE={OPTIONS.entt_archive}"], reject_tls_drift=True)

    def test_tls_rejects_unverified_or_missing_inputs_before_population(self):
        cases = [(["-DSHARK_MBEDTLS_ARCHIVE="], "Offline MbedTLS population requires SHARK_MBEDTLS_ARCHIVE"),
                 ([f"-DSHARK_MBEDTLS_ARCHIVE={ROOT / 'dependencies.lock.json'}"], "MbedTLS archive SHA-256 differs"),
                 ([f"-DSHARK_MBEDTLS_ARCHIVE={ROOT}"], "must name a regular locked archive"),
                 ([f"-DFETCHCONTENT_SOURCE_DIR_MBEDTLS={ROOT}"], "Unverified network source override"),
                 ([f"-DMBEDTLS_CONFIG_FILE={ROOT / 'dependencies.lock.json'}"], "Unverified network source override"),
                 ([f"-DTF_PSA_CRYPTO_USER_CONFIG_FILE={ROOT / 'dependencies.lock.json'}"], "Unverified network source override")]
        for arguments, diagnostic in cases:
            with self.subTest(arguments=arguments):
                self.configure(arguments, diagnostic)

    def test_source_override_is_rejected_before_population(self):
        self.configure([f"-DFETCHCONTENT_SOURCE_DIR_ENTT={ROOT}"], "Unverified EnTT source overrides are disabled")

    def test_disconnected_requires_explicit_locked_archive(self):
        self.configure(["-DSHARK_ENTT_ARCHIVE="], "Offline EnTT population requires SHARK_ENTT_ARCHIVE")

    def test_glm_requires_verified_archive_and_rejects_source_override(self):
        cases = [(["-DSHARK_GLM_ARCHIVE="], "Offline GLM population requires SHARK_GLM_ARCHIVE"),
                 ([f"-DSHARK_GLM_ARCHIVE={ROOT / 'dependencies.lock.json'}"], "SHARK_GLM_ARCHIVE SHA-256 differs"),
                 ([f"-DSHARK_GLM_ARCHIVE={ROOT}"], "must name a regular locked archive"),
                 ([f"-DFETCHCONTENT_SOURCE_DIR_GLM={ROOT}"], "Unverified GLM source overrides are disabled")]
        for arguments, diagnostic in cases:
            with self.subTest(arguments=arguments):
                self.configure(arguments, diagnostic)

    def test_wrong_archive_is_rejected_before_population(self):
        self.configure([f"-DSHARK_ENTT_ARCHIVE={ROOT / 'dependencies.lock.json'}"],
                       "SHARK_ENTT_ARCHIVE SHA-256 differs")

    def test_missing_or_directory_archive_is_rejected_before_population(self):
        for value in (ROOT, ROOT / "not-an-entt-archive.tar.gz"):
            with self.subTest(value=value):
                self.configure([f"-DSHARK_ENTT_ARCHIVE={value}"], "must name a regular locked archive")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ninja", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--c-compiler", required=True)
    parser.add_argument("--mbedtls-archive", type=Path, required=True)
    parser.add_argument("--jolt-archive", type=Path, required=True)
    parser.add_argument("--entt-archive", type=Path, required=True)
    parser.add_argument("--glm-archive", type=Path, required=True)
    parser.add_argument("--sqlite-archive", type=Path)
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
