"""Check window/graphics options and reject invalid inputs before population."""

from pathlib import Path
import argparse
import os
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
                "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
                f"-DSHARK_ENTT_ARCHIVE={OPTIONS.entt_archive}",
                f"-DSHARK_GLM_ARCHIVE={OPTIONS.glm_archive}", *arguments,
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

    def test_headless_native_window_conflict_precedes_dependency_setup(self):
        self.configure_failure(
            ["-DSHARK_BUILD_CLIENT=OFF", "-DSHARK_BUILD_GRAPHICS_PROBE=OFF",
             "-DSHARK_NATIVE_WINDOW=ON", "-DSHARK_BUILD_TESTS=OFF"],
            "SHARK_NATIVE_WINDOW requires SHARK_BUILD_CLIENT=ON",
        )

    def test_headless_native_graphics_conflict_precedes_dependency_setup(self):
        self.configure_failure(
            ["-DSHARK_BUILD_CLIENT=OFF", "-DSHARK_BUILD_GRAPHICS_PROBE=OFF",
             "-DSHARK_NATIVE_GRAPHICS=ON", "-DSHARK_BUILD_TESTS=OFF"],
            "SHARK_NATIVE_GRAPHICS requires SHARK_BUILD_CLIENT=ON",
        )

    def test_native_graphics_requires_a_supported_native_window_configuration(self):
        self.configure_failure(
            ["-DSHARK_BUILD_GRAPHICS_PROBE=OFF", "-DSHARK_NATIVE_WINDOW=OFF",
             "-DSHARK_NATIVE_GRAPHICS=ON", "-DSHARK_BUILD_TESTS=OFF"],
            ("SHARK_NATIVE_GRAPHICS requires SHARK_NATIVE_WINDOW=ON"
             if os.name == "nt" else "SHARK_NATIVE_GRAPHICS is supported only on Windows"),
        )

    def test_native_graphics_rejects_overrides_or_unsupported_host_before_download(self):
        for dependency in ("VULKANHEADERS", "VULKANLOADER", "GLFW", "NVRHI"):
            with self.subTest(dependency=dependency):
                self.configure_failure(
                    ["-DSHARK_BUILD_GRAPHICS_PROBE=OFF", "-DSHARK_NATIVE_WINDOW=ON",
                     "-DSHARK_NATIVE_GRAPHICS=ON",
                     f"-DFETCHCONTENT_SOURCE_DIR_{dependency}={ROOT}"],
                    ("source overrides are disabled; use the locked archive/cache"
                     if os.name == "nt" else "SHARK_NATIVE_GRAPHICS is supported only on Windows"),
                )

    def test_native_window_rejects_override_or_unsupported_host_before_download(self):
        self.configure_failure(
            ["-DSHARK_BUILD_GRAPHICS_PROBE=OFF", "-DSHARK_NATIVE_WINDOW=ON",
             f"-DFETCHCONTENT_SOURCE_DIR_GLFW={ROOT}"],
            ("source overrides are disabled; use the locked archive/cache"
             if os.name == "nt" else "SHARK_NATIVE_WINDOW is supported only on Windows"),
        )

    def test_device_free_configurations_need_no_c_compiler_or_client_dependencies(self):
        for client in (False, True):
            with self.subTest(client=client), tempfile.TemporaryDirectory(
                prefix="shark-device-free-dependencies-"
            ) as temporary:
                build = Path(temporary) / "build"
                command = [
                    OPTIONS.cmake, "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
                    f"-DCMAKE_MAKE_PROGRAM={OPTIONS.ninja}",
                    f"-DCMAKE_CXX_COMPILER={OPTIONS.compiler}",
                    f"-DCMAKE_C_COMPILER={Path(temporary) / 'missing-c-compiler'}",
                    f"-DSHARK_BUILD_CLIENT={'ON' if client else 'OFF'}",
                    "-DSHARK_BUILD_EDITOR=OFF", "-DSHARK_BUILD_TESTS=OFF",
                    "-DSHARK_BUILD_GRAPHICS_PROBE=OFF",
                    "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
                    f"-DSHARK_ENTT_ARCHIVE={OPTIONS.entt_archive}",
                    f"-DSHARK_GLM_ARCHIVE={OPTIONS.glm_archive}",
                ]
                if client:
                    command.append("-DSHARK_NATIVE_WINDOW=OFF")
                result = subprocess.run(command, text=True, capture_output=True, timeout=30)
                output = result.stdout + result.stderr
                self.assertEqual(result.returncode, 0, output)
                self.assertEqual({path.name for path in (build / "_deps").iterdir()},
                                 {"entt-src", "entt-build", "entt-subbuild", "glm-src", "glm-build", "glm-subbuild"})
                cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
                self.assertIn("SHARK_NATIVE_WINDOW:BOOL=OFF", cache)
                self.assertIn("SHARK_NATIVE_GRAPHICS:BOOL=OFF", cache)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ninja", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--entt-archive", type=Path, required=True)
    parser.add_argument("--glm-archive", type=Path, required=True)
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
