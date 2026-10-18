"""Reject invalid graphics configurations before any dependency population."""

from pathlib import Path
import argparse
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class GraphicsDependencyTests(unittest.TestCase):
    def configure_failure(self, arguments, diagnostic):
        with tempfile.TemporaryDirectory(prefix="shark-graphics-dependencies-") as temporary:
            build = Path(temporary) / "build"
            command = [
                OPTIONS.cmake, "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
                f"-DCMAKE_MAKE_PROGRAM={OPTIONS.ninja}",
                f"-DCMAKE_CXX_COMPILER={OPTIONS.compiler}",
                "-DSHARK_BUILD_EDITOR=OFF", "-DSHARK_BUILD_GRAPHICS_PROBE=ON",
                # A missed early rejection must fail offline, never fetch on a test host.
                "-DFETCHCONTENT_FULLY_DISCONNECTED=ON", *arguments,
            ]
            result = subprocess.run(command, text=True, capture_output=True, timeout=30)
            output = result.stdout + result.stderr
            self.assertNotEqual(result.returncode, 0, output)
            self.assertIn(diagnostic, " ".join(output.split()))
            # Even a failed FetchContent population creates this directory. Its
            # absence proves the rejection preceded every dependency path.
            self.assertFalse((build / "_deps").exists(), output)

    def test_headless_probe_conflict_precedes_dependency_setup(self):
        self.configure_failure(
            ["-DSHARK_BUILD_CLIENT=OFF", "-DSHARK_BUILD_TESTS=OFF"],
            "SHARK_BUILD_GRAPHICS_PROBE requires SHARK_BUILD_CLIENT=ON",
        )

    def test_all_source_overrides_are_rejected_before_any_download(self):
        for dependency in ("VULKANHEADERS", "VULKANLOADER", "GLFW", "NVRHI"):
            with self.subTest(dependency=dependency):
                self.configure_failure(
                    [f"-DFETCHCONTENT_SOURCE_DIR_{dependency}={ROOT}"],
                    "source overrides are disabled; use the locked archive/cache",
                )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ninja", required=True)
    parser.add_argument("--compiler", required=True)
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
