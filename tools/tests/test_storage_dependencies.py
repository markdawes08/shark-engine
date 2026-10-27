"""SQLite is a pinned private Server input and absent from protected consumers."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from storage_dependency import DEFINITIONS, archive_files, entry, populate


class StorageDependencyTests(unittest.TestCase):
    def test_locked_source_header_and_notice_population_rejects_drift(self):
        with tempfile.TemporaryDirectory(prefix="shark-storage-integrity-") as temporary:
            build = Path(temporary)
            record = populate(build, OPTIONS.sqlite_archive)
            self.assertEqual(record, populate(build, OPTIONS.sqlite_archive))
            self.assertEqual(record["compiled_files"], ["sqlite3.c"])
            root = build / "_deps/sqlite-src"
            for name in ("sqlite3.c", "sqlite3.h", "LICENSE.txt"):
                target = root / name
                original = target.read_bytes()
                for mutation in ("missing", "modified"):
                    with self.subTest(name=name, mutation=mutation):
                        target.unlink() if mutation == "missing" else target.write_bytes(original + b"\nchanged")
                        with self.assertRaisesRegex(ValueError, "Populated SQLite source"):
                            populate(build, OPTIONS.sqlite_archive)
                        target.write_bytes(original)
            (root / "injected.c").write_bytes(b"int injected;")
            with self.assertRaisesRegex(ValueError, "Populated SQLite source differs"):
                populate(build, OPTIONS.sqlite_archive)

    def test_archive_and_reviewed_metadata_are_required(self):
        locked = entry()
        expected = archive_files(OPTIONS.sqlite_archive, locked)
        self.assertEqual(set(expected), {"sqlite3.c", "sqlite3.h", "sqlite3ext.h", "shell.c", "LICENSE.txt"})
        for field, value in (("sha256", "0" * 64), ("archive_bytes", 1), ("version", "other"),
                             ("source_id", "other"), ("notice_sha256", "0" * 64)):
            with self.subTest(field=field), self.assertRaises(ValueError):
                archive_files(OPTIONS.sqlite_archive, {**locked, field: value})

    def configure(self, extra=(), expected=None, *, server=True, client=False, authoring=False, drift=False):
        with tempfile.TemporaryDirectory(prefix="shark-storage-dependencies-") as temporary:
            build = Path(temporary) / "build"
            command = [OPTIONS.cmake, "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
                       f"-DCMAKE_MAKE_PROGRAM={OPTIONS.ninja}", f"-DCMAKE_CXX_COMPILER={OPTIONS.compiler}",
                       f"-DCMAKE_C_COMPILER={OPTIONS.c_compiler}", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                       f"-DSHARK_BUILD_SERVER={'ON' if server else 'OFF'}",
                       f"-DSHARK_BUILD_CLIENT={'ON' if client else 'OFF'}",
                       f"-DSHARK_BUILD_TOOLS={'ON' if authoring else 'OFF'}",
                       "-DSHARK_BUILD_EDITOR=OFF", "-DSHARK_BUILD_TESTS=OFF",
                       "-DSHARK_NATIVE_WINDOW=OFF", "-DSHARK_NATIVE_GRAPHICS=OFF",
                       "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
                       f"-DSHARK_ENTT_ARCHIVE={OPTIONS.entt_archive}", f"-DSHARK_GLM_ARCHIVE={OPTIONS.glm_archive}",
                       f"-DSHARK_MBEDTLS_ARCHIVE={OPTIONS.mbedtls_archive}",
                       f"-DSHARK_SQLITE_ARCHIVE={OPTIONS.sqlite_archive}",
                       *([f"-DSHARK_JSON_ARCHIVE={OPTIONS.json_archive}",
                          f"-DSHARK_CGLTF_ARCHIVE={OPTIONS.cgltf_archive}"] if authoring else []), *extra]
            result = subprocess.run(command, text=True, capture_output=True, timeout=120)
            output = result.stdout + result.stderr
            if expected:
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(expected, " ".join(output.split()))
                self.assertFalse((build / "_deps").exists(), output)
                return
            self.assertEqual(result.returncode, 0, output)
            populations = {"entt-src", "entt-build", "entt-subbuild", "glm-src", "glm-build", "glm-subbuild",
                           "mbedtls-src", "mbedtls-build"}
            if server:
                populations.add("sqlite-src")
            if authoring:
                populations.add("json-src")
                populations.add("cgltf-src")
            self.assertEqual({path.name for path in (build / "_deps").iterdir()}, populations)
            graph = next(build.glob("shark-target-graph-*.cmake"))
            result = subprocess.run([OPTIONS.cmake, "-DSHARK_GRAPH=" + str(graph), "-P",
                                     str(ROOT / "cmake/CheckRuntimeBoundary.cmake")],
                                    text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            commands = json.loads((build / "compile_commands.json").read_text())
            sqlite = []
            for item in commands:
                path = Path(item["file"])
                arguments = item["command"].replace("\\", "/")
                if path.name == "sqlite3.c":
                    sqlite.append(item)
                    for definition in DEFINITIONS:
                        self.assertIn(definition, arguments)
                    self.assertNotRegex(arguments, r"(?:^|\s)(?:/W4|/WX|-Werror|-Wconversion)(?:\s|$)")
                elif path.is_relative_to(ROOT / "server/storage/src"):
                    self.assertIn("sqlite-src", arguments)
                    self.assertNotIn("SQLITE_THREADSAFE", arguments)
                else:
                    self.assertNotIn("sqlite-src", arguments)
                    self.assertNotIn("SQLITE_THREADSAFE", arguments)
                self.assertNotEqual(path.name, "shell.c")
            self.assertEqual(len(sqlite), 1 if server else 0)
            if server:
                text = graph.read_text()
                self.assertIn("SHARK_DOMAIN_EngineStorage [==[storage]==]", text)
                self.assertIn("SHARK_DOMAIN_SharkSQLite [==[storage]==]", text)
            else:
                self.assertNotIn("SharkSQLite", graph.read_text())
            if drift:
                header = build / "_deps/sqlite-src/sqlite3.h"
                header.write_bytes(header.read_bytes() + b"\n/* changed */")
                result = subprocess.run(command, text=True, capture_output=True, timeout=120)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Populated SQLite source differs", result.stdout + result.stderr)

    def test_server_owns_static_sqlite_without_public_compile_leaks(self):
        self.configure(drift=True)

    def test_runtime_and_tools_ignore_unneeded_storage_inputs(self):
        extra = ["-DSHARK_SQLITE_ARCHIVE=missing-sqlite", f"-DFETCHCONTENT_SOURCE_DIR_SQLITE={ROOT}"]
        self.configure(extra, server=False, client=True)
        if OPTIONS.json_archive:
            self.configure(extra, server=False, authoring=True)

    def test_overrides_and_invalid_archives_reject_before_population(self):
        cases = [(["-DSHARK_SQLITE_ARCHIVE="], "Offline SQLite population requires SHARK_SQLITE_ARCHIVE"),
                 ([f"-DSHARK_SQLITE_ARCHIVE={ROOT}"], "must name a regular locked archive"),
                 ([f"-DSHARK_SQLITE_ARCHIVE={ROOT / 'dependencies.lock.json'}"], "SQLite archive SHA-256 differs")]
        for name in ("FETCHCONTENT_SOURCE_DIR_SQLITE", "SQLite3_DIR", "SQLite3_INCLUDE_DIR", "SQLite3_LIBRARY", "SQLITE_SOURCE_DIR"):
            cases.append(([f"-D{name}={ROOT}"], "Unverified SQLite source override"))
        for arguments, diagnostic in cases:
            with self.subTest(arguments=arguments):
                self.configure(arguments, diagnostic)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cmake", "ninja", "compiler", "c-compiler"):
        parser.add_argument("--" + name, required=True)
    for name in ("entt-archive", "glm-archive", "mbedtls-archive", "sqlite-archive"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--json-archive", type=Path)
    parser.add_argument("--cgltf-archive", type=Path)
    OPTIONS, remaining = parser.parse_known_args()
    if OPTIONS.json_archive and not OPTIONS.cgltf_archive:
        parser.error("--json-archive requires --cgltf-archive for the authoring fixture")
    unittest.main(argv=[__file__, *remaining])
