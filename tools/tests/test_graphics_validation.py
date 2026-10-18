"""Offline validation-tool cache and manifest failure-path checks."""

from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import prepare_graphics_validation as setup


class GraphicsValidationTests(unittest.TestCase):
    def archive(self, members=None):
        members = members or [("project/CMakeLists.txt", b"project(fixture)"),
                              ("project/LICENSE", b"license")]
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w:gz") as archive:
            for name, data in members:
                info = tarfile.TarInfo(name)
                if data is None:
                    info.type = tarfile.SYMTYPE
                    info.linkname = "outside"
                    archive.addfile(info)
                else:
                    info.size = len(data)
                    archive.addfile(info, io.BytesIO(data))
        data = stream.getvalue()
        return data, {"sha256": setup.digest(data), "license_files": ["LICENSE"],
                      "url": "https://github.com/KhronosGroup/fixture/archive/immutable.tar.gz"}

    def test_archive_selects_exact_root_and_notices(self):
        data, entry = self.archive()
        self.assertEqual(setup.source_files(data, entry),
                         {"CMakeLists.txt": b"project(fixture)", "LICENSE": b"license"})

    def test_changed_archive_rejected(self):
        data, entry = self.archive()
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            setup.source_files(data + b"changed", entry)

    def test_escaping_and_link_members_rejected_before_extraction(self):
        for name, content in (("project/../outside", b"data"), ("/outside", b"data"),
                              ("C:/outside", b"data"), ("project/linked", None),
                              ("project\\..\\outside", b"data")):
            with self.subTest(name=name):
                data, entry = self.archive([("project/CMakeLists.txt", b"x"),
                                            ("project/LICENSE", b"l"), (name, content)])
                with self.assertRaisesRegex(ValueError, "Unsafe"):
                    setup.source_files(data, entry)

    def test_duplicate_members_and_multiple_roots_rejected(self):
        for extra in ("project/LICENSE", "other/LICENSE"):
            data, entry = self.archive([("project/CMakeLists.txt", b"x"),
                                        ("project/LICENSE", b"l"), (extra, b"l")])
            with self.assertRaises(ValueError):
                setup.source_files(data, entry)

    def test_license_and_archive_bounds_enforced(self):
        data, entry = self.archive()
        for constant in ("MAX_ARCHIVE", "MAX_FILE", "MAX_EXPANDED", "MAX_MEMBERS"):
            with self.subTest(constant=constant), patch.object(setup, constant, 1):
                with self.assertRaises(ValueError):
                    setup.source_files(data, entry)
        entry["license_files"].append("missing-notice")
        with self.assertRaisesRegex(ValueError, "license"):
            setup.source_files(data, entry)

    def test_offline_missing_archive_never_downloads(self):
        _, entry = self.archive()
        with tempfile.TemporaryDirectory() as temporary, patch.object(setup.urllib.request, "urlopen") as download:
            with self.assertRaisesRegex(ValueError, "not cached"):
                setup.prepare_source("Fixture", entry, Path(temporary), offline=True)
            download.assert_not_called()

    def test_offline_cache_reuse_checks_source_bytes_and_inventory(self):
        data, entry = self.archive()
        for modification in ("changed", "extra", "missing"):
            with self.subTest(modification=modification), tempfile.TemporaryDirectory() as temporary:
                cache = Path(temporary)
                (cache / (entry["sha256"] + ".tar.gz")).write_bytes(data)
                destination, receipt = setup.prepare_source("Fixture", entry, cache, offline=True)
                self.assertEqual(receipt["files_verified"], 2)
                setup.prepare_source("Fixture", entry, cache, offline=True)
                if modification == "changed":
                    (destination / "LICENSE").write_bytes(b"changed")
                elif modification == "extra":
                    (destination / "unexpected").write_bytes(b"extra")
                else:
                    (destination / "LICENSE").unlink()
                with self.assertRaisesRegex(ValueError, "changed"):
                    setup.prepare_source("Fixture", entry, cache, offline=True)

    def test_corrupt_cached_archive_is_preserved_and_never_redownloaded(self):
        _, entry = self.archive()
        with tempfile.TemporaryDirectory() as temporary, patch.object(setup.urllib.request, "urlopen") as download:
            cache = Path(temporary)
            archive = cache / (entry["sha256"] + ".tar.gz")
            archive.write_bytes(b"invalid")
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                setup.prepare_source("Fixture", entry, cache, offline=False)
            self.assertEqual(archive.read_bytes(), b"invalid")
            download.assert_not_called()

    def test_cmake_preflight_verifies_sources_without_repairing_them(self):
        data, entry = self.archive()
        lock = {"dependencies": {"VulkanHeaders": entry}, "graphics_validation": {"dependencies": {}}}
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary)
            archive = cache / (entry["sha256"] + ".tar.gz")
            with self.assertRaisesRegex(ValueError, "Missing"):
                setup.verify_cache(cache, lock)
            self.assertEqual(list(cache.iterdir()), [])
            archive.write_bytes(data)
            source, _ = setup.prepare_source("VulkanHeaders", entry, cache, offline=True)
            setup.verify_cache(cache, lock)
            (source / "CMakeLists.txt").write_text("unverified")
            with self.assertRaisesRegex(ValueError, "changed"):
                setup.verify_cache(cache, lock)
            self.assertEqual((source / "CMakeLists.txt").read_text(), "unverified")

    def test_generated_newline_exception_is_exact_and_only_for_named_windows_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            expected = {"settings.h": b"first\nsecond\n"}
            candidate = source / "settings.h"
            candidate.write_bytes(b"first\r\nsecond\r\n")
            with self.assertRaisesRegex(ValueError, "changed"):
                setup.verify_sources(source, expected)
            if os.name == "nt":
                setup.verify_sources(source, expected, ("settings.h",))
            else:
                with self.assertRaisesRegex(ValueError, "changed"):
                    setup.verify_sources(source, expected, ("settings.h",))
            candidate.write_bytes(b"first\r\nmodified\r\n")
            with self.assertRaisesRegex(ValueError, "changed"):
                setup.verify_sources(source, expected, ("settings.h",))

    def test_nonofficial_source_url_rejected_without_network(self):
        _, entry = self.archive()
        entry["url"] = "https://example.invalid/source.tar.gz"
        with tempfile.TemporaryDirectory() as temporary, patch.object(setup.urllib.request, "urlopen") as download:
            with self.assertRaisesRegex(ValueError, "official Khronos"):
                setup.prepare_source("Fixture", entry, Path(temporary), offline=False)
            download.assert_not_called()

    def test_layer_manifest_cannot_select_unrelated_library(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            library = directory / "validation.dll"
            library.write_bytes(b"library")
            manifest = directory / "validation.json"
            layer = {"name": "VK_LAYER_KHRONOS_validation", "api_version": "1.4.352",
                     "library_path": "validation.dll"}
            manifest.write_text(json.dumps({"layer": layer}))
            products = {"validation_library": str(library), "validation_manifest": str(manifest)}
            result = setup.validation_identity(products)
            self.assertEqual(result["library_sha256"], setup.digest(b"library"))
            for key, value in (("library_path", "../external.dll"), ("name", "OtherLayer")):
                mutated = {**layer, key: value}
                manifest.write_text(json.dumps({"layer": mutated}))
                with self.assertRaisesRegex(ValueError, "built local library"):
                    setup.validation_identity(products)

    def test_cleanup_refuses_parent_and_outside_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / "cache"
            cache.mkdir()
            for target in (cache, cache.parent):
                with self.assertRaisesRegex(ValueError, "Refusing cleanup"):
                    setup.remove_staging(target, cache)
                self.assertTrue(target.is_dir())

    def test_existing_evidence_is_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary)
            (evidence / "summary.json").write_text("existing")
            with patch.object(sys, "argv", ["prepare", "--output", str(evidence)]), redirect_stdout(io.StringIO()):
                self.assertEqual(setup.main(), 2)
            self.assertEqual((evidence / "summary.json").read_text(), "existing")

    def test_unsupported_native_host_reports_not_run_without_fetching(self):
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "new"
            with (patch.object(setup, "native_host", return_value="darwin-arm64"),
                  patch.object(setup, "prepare_source") as prepare,
                  patch.object(sys, "argv", ["prepare", "--output", str(evidence)]),
                  redirect_stdout(io.StringIO())):
                self.assertEqual(setup.main(), 3)
            prepare.assert_not_called()
            report = json.loads((evidence / "summary.json").read_text())
            self.assertEqual(report["status"], "NOT_RUN")
            self.assertNotIn("validation_layer", report)


if __name__ == "__main__":
    unittest.main()
