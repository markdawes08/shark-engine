"""Evidence preservation, corruption detection and archive path safety checks."""

import hashlib
import json
from pathlib import Path
import stat
import sys
import tempfile
import unittest
import warnings
import zipfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import archive_evidence as archive


class EvidenceArchiveTests(unittest.TestCase):
    def test_round_trip_keeps_original_paths_and_source_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "old checkpoint/caf\u00e9/output.bin"
            source.parent.mkdir(parents=True)
            payload = bytes(range(256)) * 8192
            source.write_bytes(payload)
            output = root / "evidence.zip"
            result = archive.create_archive(root, [source.relative_to(root).as_posix()], output)
            self.assertEqual(result["source_bytes"], len(payload))
            self.assertEqual(result, archive.verify_archive(output))
            self.assertEqual(source.read_bytes(), payload)
            with zipfile.ZipFile(output) as bundle:
                self.assertEqual(bundle.read(source.relative_to(root).as_posix()), payload)

    def test_existing_output_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "input").write_text("input")
            output = root / "existing.zip"
            output.write_bytes(b"existing evidence")
            with self.assertRaisesRegex(ValueError, "already exists"):
                archive.create_archive(root, ["input"], output)
            self.assertEqual(output.read_bytes(), b"existing evidence")

    def test_unsafe_and_duplicate_selection_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "input").write_text("input")
            for selection in (["../input"], ["/input"], ["C:/input"], ["a\\input"],
                              ["a/./input"], ["a//input"], ["input/"], ["input", "input"],
                              [archive.MANIFEST], []):
                with self.subTest(selection=selection), self.assertRaises(ValueError):
                    archive.create_archive(root, selection, root / "bad.zip")
                self.assertFalse((root / "bad.zip").exists())

    def test_source_symlink_is_rejected_when_supported(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "input").write_text("input")
            try:
                (root / "linked").symlink_to(root / "input")
            except OSError:
                self.skipTest("Host does not permit creating symlinks")
            with self.assertRaisesRegex(ValueError, "Linked/reparse"):
                archive.create_archive(root, ["linked"], root / "bad.zip")

    def test_failed_verification_removes_only_new_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "input"
            source.write_bytes(b"important evidence")
            output = root / "failed.zip"
            with patch.object(archive, "verify_archive", side_effect=ValueError("verification failed")):
                with self.assertRaisesRegex(ValueError, "verification failed"):
                    archive.create_archive(root, ["input"], output)
            self.assertFalse(output.exists())
            self.assertEqual(source.read_bytes(), b"important evidence")

    def write_bundle(self, path, name="evidence.txt", payload=b"original", record_payload=b"original",
                     extra=False, duplicate=False, mode=None):
        manifest = {"version": 1, "files": {name: {
            "bytes": len(record_payload), "sha256": hashlib.sha256(record_payload).hexdigest()}}}
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(path, "w") as bundle:
                item = zipfile.ZipInfo(name)
                if mode is not None:
                    item.external_attr = mode << 16
                bundle.writestr(item, payload)
                if duplicate:
                    bundle.writestr(name, payload)
                if extra:
                    bundle.writestr("unlisted", b"extra")
                bundle.writestr(archive.MANIFEST, json.dumps(manifest))

    def test_changed_content_size_and_extra_members_fail_verification(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bad.zip"
            for options in ({"payload": b"modified"}, {"payload": b"short"}, {"extra": True},
                            {"duplicate": True}):
                with self.subTest(options=options):
                    self.write_bundle(path, **options)
                    with self.assertRaises(ValueError):
                        archive.verify_archive(path)

    def test_link_and_escape_members_fail_without_extraction(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bad.zip"
            for options in ({"name": "../outside"}, {"name": "/outside"},
                            {"mode": stat.S_IFLNK | 0o777}):
                with self.subTest(options=options):
                    self.write_bundle(path, **options)
                    with self.assertRaises(ValueError):
                        archive.verify_archive(path)

    def test_duplicate_manifest_keys_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bad.zip"
            with zipfile.ZipFile(path, "w") as bundle:
                bundle.writestr("evidence.txt", "input")
                bundle.writestr(archive.MANIFEST, '{"version":1,"version":1,"files":{}}')
            with self.assertRaisesRegex(ValueError, "Duplicate JSON"):
                archive.verify_archive(path)


if __name__ == "__main__":
    unittest.main()
