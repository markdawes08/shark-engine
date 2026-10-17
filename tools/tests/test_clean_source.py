"""Source snapshots must not follow links into ignored local inputs."""

from pathlib import Path
import os
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_clean_source import source_file


class SourcePathTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="shark-source-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / ".env").write_text("fixture-only private value", encoding="utf-8")
        (self.root / "source.cpp").write_text("int main() {}", encoding="utf-8")

    def link(self, target: Path, link: Path, *, directory=False):
        try:
            os.symlink(target, link, target_is_directory=directory)
        except OSError as error:
            if os.name == "nt" and getattr(error, "winerror", None) == 1314:
                self.skipTest("Windows account cannot create symlinks; Unix CI must exercise this case")
            raise
        self.addCleanup(link.unlink)

    def test_accepts_regular_source(self):
        self.assertEqual(source_file(self.root, "source.cpp"), self.root / "source.cpp")

    def test_rejects_traversal_and_absolute_paths(self):
        for path in ("../outside.cpp", str(self.root / "source.cpp")):
            with self.assertRaises(ValueError):
                source_file(self.root, path)

    def test_rejects_link_to_local_ignored_file(self):
        self.link(self.root / ".env", self.root / "visible.json")
        with self.assertRaisesRegex(ValueError, "Linked source"):
            source_file(self.root, "visible.json")

    def test_rejects_linked_parent_directory(self):
        private = self.root / "local"
        private.mkdir()
        (private / "settings.json").write_text("fixture", encoding="utf-8")
        self.link(private, self.root / "linked", directory=True)
        with self.assertRaisesRegex(ValueError, "Linked source"):
            source_file(self.root, "linked/settings.json")


if __name__ == "__main__":
    unittest.main()
