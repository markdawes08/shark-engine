"""Check exact Jolt population, source drift and bounded archive rejection."""

from __future__ import annotations

import argparse
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import physics_dependency as physics


class PhysicsDependencyTests(unittest.TestCase):
    def test_exact_population_rejects_source_drift_without_repair(self):
        with tempfile.TemporaryDirectory(prefix="shark-jolt-source-") as temporary:
            build = Path(temporary)
            record = physics.populate(build, OPTIONS.archive)
            locked = physics.entry()
            self.assertEqual(record["commit"], locked["commit"])
            self.assertEqual(len(record["source_files"]), locked["source_file_count"])
            self.assertEqual(record["license_files"], {"LICENSE": locked["source_sha256"]["LICENSE"]})
            self.assertEqual(physics.verify_sources(ROOT, build, OPTIONS.archive), record)
            header = build / "_deps/jolt-src/Jolt/Jolt.h"
            original = header.read_bytes()
            for kind in ("modified", "missing", "added"):
                with self.subTest(kind=kind):
                    extra = header.parent / "unexpected.h"
                    if kind == "modified":
                        header.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
                    elif kind == "missing":
                        header.unlink()
                    else:
                        extra.write_bytes(b"injected")
                    with self.assertRaisesRegex(ValueError, "Populated Jolt source"):
                        physics.populate(build, OPTIONS.archive)
                    header.write_bytes(original)
                    extra.unlink(missing_ok=True)

    def test_archive_mutation_rejects_before_population(self):
        with tempfile.TemporaryDirectory(prefix="shark-jolt-archive-") as temporary:
            root = Path(temporary)
            original = OPTIONS.archive.read_bytes()
            for kind, data in (("truncated", original[:-1]),
                               ("modified", bytes([original[0] ^ 1]) + original[1:])):
                with self.subTest(kind=kind):
                    archive = root / (kind + ".tar.gz")
                    archive.write_bytes(data)
                    with self.assertRaisesRegex(ValueError, "Jolt archive"):
                        physics.populate(root / "build", archive)
                    self.assertFalse((root / "build/_deps").exists())

    def test_changed_reviewed_options_and_metadata_reject(self):
        locked = physics.entry()
        for field, replacement in (("version", "master"), ("license", "other"),
                                   ("license_files", []), ("patches", ["patch"]),
                                   ("transitive_dependencies", ["graphics"]),
                                   ("commit", "latest"), ("sha256", "unpinned"),
                                   ("options", dict(locked["options"], JPH_USE_VK=True)),
                                   ("integration", dict(locked["integration"], cpu_baseline="native"))):
            with self.subTest(field=field), tempfile.TemporaryDirectory(prefix="shark-jolt-lock-") as temporary:
                source = Path(temporary)
                (source / "dependencies.lock.json").write_text(
                    json.dumps({"dependencies": {"Jolt": dict(locked, **{field: replacement})}}),
                    encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "integration review"):
                    physics.entry(source)

    def synthetic(self, root, extra):
        locked = copy.deepcopy(physics.entry())
        files = {name: name.encode("utf-8") for name in physics.REVIEWED_SOURCES}
        archive = root / "synthetic.tar.gz"
        with tarfile.open(archive, "w:gz") as stream:
            for name, content in files.items():
                member = tarfile.TarInfo(locked["archive_prefix"] + "/" + name)
                member.size = len(content)
                stream.addfile(member, io.BytesIO(content))
            name, kind = extra
            member = tarfile.TarInfo(name if name.startswith("/") else locked["archive_prefix"] + "/" + name)
            if kind == "link":
                member.type = tarfile.SYMTYPE
                member.linkname = "../../outside"
            stream.addfile(member)
        locked["archive_bytes"] = archive.stat().st_size
        locked["sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
        locked["source_file_count"] = len(files) + 1
        locked["source_bytes"] = sum(map(len, files.values()))
        locked["source_sha256"] = {name: hashlib.sha256(content).hexdigest() for name, content in files.items()}
        return archive, locked

    def test_unsafe_paths_links_duplicates_and_resource_bounds_reject(self):
        with tempfile.TemporaryDirectory(prefix="shark-jolt-unsafe-") as temporary:
            root = Path(temporary)
            for name, kind, diagnostic in (("/outside", "file", "Unsafe"),
                                            ("../outside", "file", "Unsafe"),
                                            ("C:outside", "file", "Unsafe"),
                                            ("dir\\outside", "file", "Unsafe"),
                                            ("linked", "link", "Unsafe"),
                                            ("LICENSE", "file", "Duplicate")):
                with self.subTest(name=name):
                    archive, locked = self.synthetic(root, (name, kind))
                    with self.assertRaisesRegex(ValueError, diagnostic):
                        physics.archive_files(archive, locked)
            archive, locked = self.synthetic(root, ("extra", "file"))
            with patch.object(physics, "MAX_FILES", len(physics.REVIEWED_SOURCES)):
                with self.assertRaisesRegex(ValueError, "oversized"):
                    physics.archive_files(archive, locked)
            with patch.object(physics, "MAX_CONTENT", 1):
                with self.assertRaisesRegex(ValueError, "oversized"):
                    physics.archive_files(archive, locked)

    def test_download_is_bounded_and_validated_before_cache_publication(self):
        original = OPTIONS.archive.read_bytes()
        with tempfile.TemporaryDirectory(prefix="shark-jolt-download-") as temporary:
            root = Path(temporary)
            archive = root / "cache/input.tar.gz"
            for kind, data in (("truncated", original[:-1]), ("oversized", original + b"x")):
                with self.subTest(kind=kind):
                    with patch.object(physics.urllib.request, "urlopen", return_value=io.BytesIO(data)):
                        with self.assertRaisesRegex(ValueError, "locked size or SHA-256"):
                            physics.download_archive(archive)
                    self.assertFalse(archive.exists())
            with patch.object(physics.urllib.request, "urlopen") as fetch:
                physics.download_archive(OPTIONS.archive)
                fetch.assert_not_called()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    OPTIONS, remaining = parser.parse_known_args()
    OPTIONS.archive = OPTIONS.archive.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
