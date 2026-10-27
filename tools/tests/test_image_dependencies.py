"""Check exact stb_image population, bounded archives and the Tools-only CMake gate."""

from __future__ import annotations

import argparse
import copy
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import image_dependency


class ImageDependencyTests(unittest.TestCase):
    def test_exact_population_and_repeated_verification(self):
        with tempfile.TemporaryDirectory(prefix="shark-stb_image-source-") as temporary:
            build = Path(temporary)
            record = image_dependency.populate(build, OPTIONS.archive)
            self.assertEqual(image_dependency.populate(build, OPTIONS.archive), record)
            locked = image_dependency.specification()
            self.assertEqual(record["commit"], locked["commit"])
            self.assertEqual(record["license_sha256"], locked["source_sha256"]["LICENSE"])
            self.assertEqual(record["source_files"]["stb_image.h"], locked["source_sha256"]["stb_image.h"])
            self.assertEqual(len(record["source_files"]), 431)

    def test_missing_added_and_modified_populated_inputs_reject(self):
        with tempfile.TemporaryDirectory(prefix="shark-stb_image-drift-") as temporary:
            build = Path(temporary)
            image_dependency.populate(build, OPTIONS.archive)
            source = build / "_deps/stb_image-src"
            header = source / "stb_image.h"
            content = header.read_bytes()
            for mutation in ("missing", "modified", "added"):
                with self.subTest(mutation=mutation):
                    if mutation == "missing":
                        header.unlink()
                    elif mutation == "modified":
                        header.write_bytes(content + b"\n// changed\n")
                    else:
                        (source / "unexpected.h").write_bytes(b"injected")
                    with self.assertRaisesRegex(ValueError, "Populated stb_image source"):
                        image_dependency.populate(build, OPTIONS.archive)
                    header.write_bytes(content)
                    (source / "unexpected.h").unlink(missing_ok=True)

    def test_missing_directory_truncated_and_same_size_modified_archives_reject(self):
        with tempfile.TemporaryDirectory(prefix="shark-stb_image-archive-") as temporary:
            root = Path(temporary)
            for archive in (root / "missing.tar.gz", root):
                with self.subTest(archive=archive):
                    with self.assertRaisesRegex(ValueError, "stb_image archive"):
                        image_dependency.populate(root / "build", archive)
            archive = root / "input.tar.gz"
            original = OPTIONS.archive.read_bytes()
            archive.write_bytes(original[:-1])
            with self.assertRaisesRegex(ValueError, "size-mismatched"):
                image_dependency.populate(root / "build", archive)
            archive.write_bytes(bytes([original[0] ^ 1]) + original[1:])
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                image_dependency.populate(root / "build", archive)
            self.assertFalse((root / "build/_deps").exists())

    def test_bounded_download_verifies_before_cache_publication(self):
        original = OPTIONS.archive.read_bytes()

        class BoundedInput(io.BytesIO):
            def read(stream, size=-1):
                self.assertEqual(size, len(original) + 1)
                return super().read(size)

        with tempfile.TemporaryDirectory(prefix="shark-stb_image-download-") as temporary:
            root = Path(temporary)
            archive = root / "cache/input.tar.gz"
            with patch.object(image_dependency.urllib.request, "urlopen", return_value=BoundedInput(original)) as fetch:
                image_dependency.download_archive(archive)
                fetch.assert_called_once_with(image_dependency.specification()["url"], timeout=60)
            self.assertEqual(archive.read_bytes(), original)
            with patch.object(image_dependency.urllib.request, "urlopen") as fetch:
                image_dependency.download_archive(archive)
                fetch.assert_not_called()
            for name, content in (("truncated", original[:-1]), ("oversized", original + b"x" * 100),
                                  ("changed", bytes([original[0] ^ 1]) + original[1:])):
                with self.subTest(name=name):
                    rejected = root / name / "input.tar.gz"
                    with patch.object(image_dependency.urllib.request, "urlopen", return_value=io.BytesIO(content)):
                        with self.assertRaisesRegex(ValueError, "locked size or SHA-256"):
                            image_dependency.download_archive(rejected)
                    self.assertFalse(rejected.exists())
            self.assertEqual([p.name for p in archive.parent.iterdir()], [archive.name])

    def test_unreviewed_metadata_rejects_before_population(self):
        locked = image_dependency.specification()
        changes = {"license": "BSD-2-Clause", "license_files": [], "patches": ["patch"],
                   "transitive_dependencies": ["codec"], "commit": "master", "sha256": "floating",
                   "archive_prefix": "escape", "source_sha256": {"stb_image.h": "wrong"},
                   "url": "https://unreviewed.invalid/archive", "archive_bytes": 0, "options": {}}
        with tempfile.TemporaryDirectory(prefix="shark-stb_image-metadata-") as temporary:
            source = Path(temporary)
            for field, replacement in changes.items():
                with self.subTest(field=field):
                    candidate = dict(locked, **{field: replacement})
                    (source / "dependencies.lock.json").write_text(
                        json.dumps({"dependencies": {"stb_image": candidate}}), encoding="utf-8")
                    with patch.object(image_dependency, "ROOT", source):
                        with self.assertRaisesRegex(ValueError, "integration review"):
                            image_dependency.populate(source / "build", OPTIONS.archive)
                    self.assertFalse((source / "build/_deps").exists())

    def synthetic_archive(self, folder, members):
        locked = copy.deepcopy(image_dependency.specification())
        prefix = locked["archive_prefix"]
        archive = folder / "synthetic.tar.gz"
        with tarfile.open(archive, "w:gz") as output:
            for relative, content, kind in members:
                info = tarfile.TarInfo(relative if relative.startswith("/") else prefix + "/" + relative)
                if kind == "regular":
                    info.size = len(content)
                    output.addfile(info, io.BytesIO(content))
                elif kind == "directory":
                    info.type = tarfile.DIRTYPE
                    output.addfile(info)
                else:
                    info.type = tarfile.SYMTYPE
                    info.linkname = "../../outside"
                    output.addfile(info)
        locked["archive_bytes"] = archive.stat().st_size
        locked["sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
        locked["source_sha256"] = {
            name: hashlib.sha256(body).hexdigest() for name, body in
            (("stb_image.h", b"header"), ("LICENSE", b"license"))}
        return archive, locked

    def test_archive_paths_links_duplicates_and_resource_bounds_reject(self):
        base = [("stb_image.h", b"header", "regular"), ("LICENSE", b"license", "regular")]
        cases = [
            ("absolute", [("/outside", b"", "regular")], "Unsafe"),
            ("parent", [("../outside", b"", "regular")], "Unsafe"),
            ("windows", [("dir\\escape", b"", "regular")], "Unsafe"),
            ("drive", [("C:escape", b"", "regular")], "Unsafe"),
            ("link", [("linked", b"", "link")], "Unsafe"),
            ("duplicate", [("stb_image.h", b"header", "regular")], "Duplicate"),
            ("count", [(str(n), b"", "regular") for n in range(image_dependency.MAX_FILES - 1)], "oversized"),
            ("members", [(str(n), b"", "directory") for n in range(image_dependency.MAX_MEMBERS - 1)], "Oversized"),
            ("bytes", [("large", b"a" * image_dependency.MAX_CONTENT, "regular")], "oversized"),
        ]
        with tempfile.TemporaryDirectory(prefix="shark-stb_image-unsafe-") as temporary:
            root = Path(temporary)
            for name, extra, diagnostic in cases:
                with self.subTest(name=name):
                    folder = root / name
                    folder.mkdir()
                    archive, locked = self.synthetic_archive(folder, base + extra)
                    with self.assertRaisesRegex(ValueError, diagnostic):
                        image_dependency.archive_files(archive, locked)

    def test_reviewed_header_and_license_hashes_are_independent_of_archive_pin(self):
        with tempfile.TemporaryDirectory(prefix="shark-stb_image-review-") as temporary:
            archive, locked = self.synthetic_archive(
                Path(temporary), [("stb_image.h", b"header", "regular"), ("LICENSE", b"license", "regular")])
            self.assertEqual(set(image_dependency.archive_files(archive, locked)), {"stb_image.h", "LICENSE"})
            for field in ("stb_image.h", "LICENSE"):
                changed = copy.deepcopy(locked)
                changed["source_sha256"][field] = "0" * 64
                with self.subTest(field=field):
                    with self.assertRaisesRegex(ValueError, "reviewed source hash"):
                        image_dependency.archive_files(archive, changed)

    def test_full_archive_inventory_rejects_unreviewed_members(self):
        with tempfile.TemporaryDirectory(prefix="shark-image-inventory-") as temporary:
            archive, locked = self.synthetic_archive(
                Path(temporary), [("stb_image.h", b"header", "regular"), ("LICENSE", b"license", "regular"),
                                  ("unexpected.h", b"extra", "regular")])
            with self.assertRaisesRegex(ValueError, "archive inventory differs"):
                image_dependency.archive_files(archive, locked)

    def configure(self, *, tools=True, extra=(), expected=None, collision=""):
        with tempfile.TemporaryDirectory(prefix="shark-stb_image-cmake-") as temporary:
            source = Path(temporary) / "source"
            build = Path(temporary) / "build"
            source.mkdir()
            body = '''cmake_minimum_required(VERSION 3.28)
project(StbImageBoundaryFixture LANGUAGES NONE)
set(PROJECT_SOURCE_DIR "@ROOT@")
include("@ROOT@/cmake/SharkBoundaries.cmake")
include("@ROOT@/cmake/ImageDependencies.cmake")
@COLLISION@
shark_validate_image_inputs()
shark_setup_image_dependencies()
if(SHARK_BUILD_TOOLS)
  if(NOT TARGET SharkStbImage OR NOT TARGET Shark::StbImage)
    message(FATAL_ERROR "Missing authoring target")
  endif()
  get_target_property(includes SharkStbImage INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
  if(NOT includes STREQUAL "${CMAKE_BINARY_DIR}/_deps/stb_image-src")
    message(FATAL_ERROR "stb_image include is not private system input")
  endif()
  get_property(domain GLOBAL PROPERTY SHARK_DOMAIN_SharkStbImage)
  if(NOT domain STREQUAL "authoring")
    message(FATAL_ERROR "stb_image authoring domain missing")
  endif()
else()
  if(TARGET SharkStbImage OR TARGET Shark::StbImage OR EXISTS "${CMAKE_BINARY_DIR}/_deps")
    message(FATAL_ERROR "Protected build populated stb_image")
  endif()
endif()
'''.replace("@ROOT@", ROOT.as_posix()).replace("@COLLISION@", collision)
            (source / "CMakeLists.txt").write_text(body, encoding="utf-8")
            command = [OPTIONS.cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
                       "-DCMAKE_MAKE_PROGRAM=" + OPTIONS.ninja,
                       "-DPython3_EXECUTABLE=" + sys.executable,
                       "-DSHARK_BUILD_TOOLS=" + ("ON" if tools else "OFF"),
                       "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
                       "-DSHARK_IMAGE_ARCHIVE=" + str(OPTIONS.archive), *extra]
            result = subprocess.run(command, text=True, capture_output=True, timeout=30)
            output = result.stdout + result.stderr
            if expected:
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(expected, " ".join(output.split()))
                self.assertFalse((build / "_deps").exists(), output)
            else:
                self.assertEqual(result.returncode, 0, output)
                if tools:
                    self.assertEqual({p.name for p in (build / "_deps").iterdir()}, {"stb_image-src"})
                    self.assertFalse((build / "_deps/stb_image-build").exists())
                else:
                    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
                    self.assertNotIn("SHARK_IMAGE_SOURCE_DIR:", cache)

    def test_cmake_tools_population_is_private_without_upstream_build(self):
        self.configure()

    def test_cmake_protected_build_ignores_unneeded_invalid_inputs(self):
        self.configure(tools=False, extra=("-DSHARK_IMAGE_ARCHIVE=missing-input",
                                          "-DFETCHCONTENT_SOURCE_DIR_STB_IMAGE=untrusted"))

    def test_cmake_invalid_archives_and_source_overrides_reject_before_population(self):
        cases = [("-DSHARK_IMAGE_ARCHIVE=", "Offline stb_image population requires"),
                 ("-DSHARK_IMAGE_ARCHIVE=missing-input", "Offline stb_image population requires"),
                 ("-DSHARK_IMAGE_ARCHIVE=" + str(ROOT), "must name a regular locked archive"),
                 ("-DSHARK_IMAGE_ARCHIVE=" + str(ROOT / "dependencies.lock.json"), "archive size differs")]
        cases += [("-D" + name + "=untrusted", "Unverified stb_image source override") for name in
                  ("FETCHCONTENT_SOURCE_DIR_STB_IMAGE", "FETCHCONTENT_SOURCE_DIR_STB",
                   "stb_image_DIR", "STB_IMAGE_SOURCE_DIR")]
        for argument, diagnostic in cases:
            with self.subTest(argument=argument):
                self.configure(extra=(argument,), expected=diagnostic)

    def test_cmake_existing_target_cannot_bypass_reviewed_population(self):
        for name in ("stb_image", "SharkStbImage", "Shark::StbImage"):
            with self.subTest(name=name):
                self.configure(collision="add_library(" + name + " INTERFACE IMPORTED)",
                               expected="target already exists")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ninja", required=True)
    OPTIONS, remaining = parser.parse_known_args()
    OPTIONS.archive = OPTIONS.archive.resolve()
    unittest.main(argv=[__file__, *remaining])
