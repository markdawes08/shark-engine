"""Check the exact Editor dependency and its protected-build exclusion."""

from __future__ import annotations

import argparse
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import editor_dependency


class EditorDependencyTests(unittest.TestCase):
    def test_population_reuse_and_source_drift(self):
        with tempfile.TemporaryDirectory(prefix="shark-imgui-source-") as temporary:
            build = Path(temporary)
            record = editor_dependency.populate(build, OPTIONS.archive)
            self.assertEqual(editor_dependency.populate(build, OPTIONS.archive), record)
            self.assertEqual(len(record["source_files"]), 243)
            self.assertEqual(record["commit"], "f5befd2d29e66809cd1110a152e375a7f1981f06")
            self.assertEqual(set(record["license_sha256"]), set(editor_dependency.LICENSES))
            source = build / "_deps/imgui-src"
            header = source / "imgui.h"
            original = header.read_bytes()
            for mutation in ("modified", "missing", "added"):
                with self.subTest(mutation=mutation):
                    if mutation == "modified":
                        header.write_bytes(original + b"\n// changed\n")
                    elif mutation == "missing":
                        header.unlink()
                    else:
                        (source / "unexpected.h").write_bytes(b"injected")
                    with self.assertRaisesRegex(ValueError, "Populated imgui source"):
                        editor_dependency.populate(build, OPTIONS.archive)
                    header.write_bytes(original)
                    (source / "unexpected.h").unlink(missing_ok=True)

    def test_archive_and_bounded_download_reject_before_publication(self):
        original = OPTIONS.archive.read_bytes()

        class BoundedInput(io.BytesIO):
            def read(stream, size=-1):
                self.assertEqual(size, len(original) + 1)
                return super().read(size)

        with tempfile.TemporaryDirectory(prefix="shark-imgui-download-") as temporary:
            root = Path(temporary)
            archive = root / "cache/input.tar.gz"
            with patch.object(editor_dependency.urllib.request, "urlopen", return_value=BoundedInput(original)):
                editor_dependency.download_archive(archive)
            self.assertEqual(archive.read_bytes(), original)
            for name, content in (("truncated", original[:-1]), ("oversized", original + b"x"),
                                  ("modified", bytes([original[0] ^ 1]) + original[1:])):
                with self.subTest(name=name):
                    rejected = root / name / "input.tar.gz"
                    with patch.object(editor_dependency.urllib.request, "urlopen", return_value=BoundedInput(content)):
                        with self.assertRaisesRegex(ValueError, "locked size or SHA-256"):
                            editor_dependency.download_archive(rejected)
                    self.assertFalse(rejected.exists())
                    archive.write_bytes(content)
                    with self.assertRaisesRegex(ValueError, "archive"):
                        editor_dependency.populate(root / "build", archive)
                    self.assertFalse((root / "build/_deps").exists())

    def configure_gate(self, editor, graphics, *, override="", collision=False, expected=None):
        with tempfile.TemporaryDirectory(prefix="shark-imgui-gate-") as temporary:
            source = Path(temporary) / "source"
            build = Path(temporary) / "build"
            source.mkdir()
            body = f'''cmake_minimum_required(VERSION 3.28)
project(ImGuiGateFixture LANGUAGES NONE)
set(PROJECT_SOURCE_DIR "{ROOT.as_posix()}")
include("{ROOT.as_posix()}/cmake/EditorDependencies.cmake")
'''
            if collision:
                body += "add_library(SharkImGui INTERFACE IMPORTED)\n"
            body += '''shark_validate_editor_inputs()
shark_setup_editor_dependencies()
if(TARGET SharkImGui OR TARGET Shark::ImGui OR EXISTS "${CMAKE_BINARY_DIR}/_deps")
  message(FATAL_ERROR "Protected build populated ImGui")
endif()
'''
            (source / "CMakeLists.txt").write_text(body, encoding="utf-8")
            command = [OPTIONS.cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
                       "-DCMAKE_MAKE_PROGRAM=" + OPTIONS.ninja,
                       "-DSHARK_BUILD_EDITOR=" + editor, "-DSHARK_NATIVE_GRAPHICS=" + graphics,
                       "-DFETCHCONTENT_FULLY_DISCONNECTED=ON", "-DSHARK_IMGUI_ARCHIVE=" + str(OPTIONS.archive)]
            if override:
                command.append(override)
            result = subprocess.run(command, text=True, capture_output=True, timeout=30)
            output = " ".join((result.stdout + result.stderr).split())
            if expected:
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(expected, output)
            else:
                self.assertEqual(result.returncode, 0, output)
                self.assertNotIn("SHARK_IMGUI_SOURCE_DIR:", (build / "CMakeCache.txt").read_text(encoding="utf-8"))
            self.assertFalse((build / "_deps").exists(), output)

    def test_noneditor_and_cpu_editor_ignore_gui_inputs(self):
        for editor, graphics in (("OFF", "ON"), ("OFF", "OFF"), ("ON", "OFF")):
            with self.subTest(editor=editor, graphics=graphics):
                self.configure_gate(editor, graphics, override="-DFETCHCONTENT_SOURCE_DIR_IMGUI=untrusted")

    def test_native_editor_rejects_source_override_missing_archive_and_target_collision(self):
        self.configure_gate("ON", "ON", override="-DFETCHCONTENT_SOURCE_DIR_IMGUI=untrusted",
                            expected="Unverified ImGui source override")
        self.configure_gate("ON", "ON", override="-DSHARK_IMGUI_ARCHIVE=missing-input",
                            expected="Offline ImGui population requires")
        self.configure_gate("ON", "ON", collision=True, expected="target already exists")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--ninja", required=True)
    OPTIONS, remaining = parser.parse_known_args()
    OPTIONS.archive = OPTIONS.archive.resolve()
    unittest.main(argv=[__file__, *remaining])
