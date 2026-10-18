"""07a Core dependency population stays pinned, private and disconnected-capable."""

import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class SceneDependencyTests(unittest.TestCase):
    def configure(self, extra, expected=None):
        with tempfile.TemporaryDirectory(prefix="shark-scene-dependencies-") as temporary:
            build = Path(temporary) / "build"
            command = [OPTIONS.cmake, "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
                       f"-DCMAKE_MAKE_PROGRAM={OPTIONS.ninja}",
                       f"-DCMAKE_CXX_COMPILER={OPTIONS.compiler}",
                       f"-DCMAKE_C_COMPILER={Path(temporary) / 'missing-c-compiler'}",
                       "-DSHARK_BUILD_CLIENT=OFF", "-DSHARK_BUILD_SERVER=ON",
                       "-DSHARK_BUILD_EDITOR=OFF", "-DSHARK_BUILD_TESTS=OFF",
                       "-DFETCHCONTENT_FULLY_DISCONNECTED=ON", *extra]
            result = subprocess.run(command, text=True, capture_output=True, timeout=30)
            text = result.stdout + result.stderr
            if expected:
                self.assertNotEqual(result.returncode, 0, text)
                self.assertIn(expected, " ".join(text.split()))
                self.assertFalse((build / "_deps").exists(), text)
            else:
                self.assertEqual(result.returncode, 0, text)
                self.assertEqual({p.name for p in (build / "_deps").iterdir()},
                                 {"entt-src", "entt-build", "entt-subbuild"})
                cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
                self.assertIn("FETCHCONTENT_FULLY_DISCONNECTED:BOOL=ON", cache)
                self.assertNotIn("CMAKE_C_COMPILER_ID", cache)
                self.assertNotIn("ENTT_BUILD_TESTING:BOOL", cache)
                graph = next(build.glob("shark-target-graph-*.cmake")).read_text(encoding="utf-8")
                self.assertIn("SHARK_DOMAIN_EnTT [==[core]==]", graph)
                self.assertIn("SHARK_DOMAIN_EnTT::EnTT [==[core]==]", graph)
                check = subprocess.run([OPTIONS.cmake, "-DSHARK_GRAPH=" + str(next(build.glob("shark-target-graph-*.cmake"))),
                                        "-DSHARK_BOUNDARY_MODE=server", "-P", str(ROOT / "cmake/CheckRuntimeBoundary.cmake")],
                                       text=True, capture_output=True, timeout=15)
                self.assertEqual(check.returncode, 0, check.stdout + check.stderr)

    def test_verified_archive_populates_offline_without_c_or_upstream_configuration(self):
        self.configure([f"-DSHARK_ENTT_ARCHIVE={OPTIONS.entt_archive}"])

    def test_source_override_is_rejected_before_population(self):
        self.configure([f"-DFETCHCONTENT_SOURCE_DIR_ENTT={ROOT}"], "Unverified EnTT source overrides are disabled")

    def test_disconnected_requires_explicit_locked_archive(self):
        self.configure([], "Offline EnTT population requires SHARK_ENTT_ARCHIVE")

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
    parser.add_argument("--entt-archive", type=Path, required=True)
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
