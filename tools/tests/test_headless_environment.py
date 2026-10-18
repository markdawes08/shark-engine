"""Reject accidental desktop packages and package drift in the minimal server gate."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from headless_ci import package_errors


class HeadlessEnvironmentTests(unittest.TestCase):
    def test_reviewed_non_graphical_hash_package_is_allowed(self):
        packages = "libc6:amd64\t2.39\tamd64\nlibxxhash0:amd64\t0.8.2\tamd64\n"
        self.assertEqual([], package_errors(packages, packages))

    def test_presentation_packages_fail_even_if_inventory_is_edited(self):
        for package in ("libx11-6", "libxcb1", "libwayland-client0", "libvulkan1",
                        "mesa-vulkan-drivers", "libegl1", "libasound2t64", "libpulse0",
                        "libglfw3", "libassimp5", "glslang-tools", "libshaderc1",
                        "spirv-tools", "libpng16-16t64", "libdrm2", "libopenal1"):
            with self.subTest(package=package):
                inventory = f"{package}:amd64\t1.0\tamd64\n"
                self.assertTrue(any("Presentation/import/shader" in item for item in
                                    package_errors(inventory, inventory)))

    def test_unreviewed_package_and_version_changes_fail(self):
        expected = "libc6:amd64\t2.39\tamd64\n"
        for actual in (expected + "unknown-sdk\t1\tamd64\n",
                       "libc6:amd64\t2.40\tamd64\n", ""):
            with self.subTest(actual=actual):
                self.assertTrue(any("lock mismatch" in item for item in package_errors(actual, expected)))

    def test_inventory_order_does_not_change_evidence(self):
        first = "base-files\t1\tamd64\napt\t2\tamd64\n"
        second = "apt\t2\tamd64\nbase-files\t1\tamd64\n"
        self.assertEqual([], package_errors(first, second))


if __name__ == "__main__":
    unittest.main()
