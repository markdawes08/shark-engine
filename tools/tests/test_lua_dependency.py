"""Verify the pinned Lua source and reject archive and population drift."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import lua_dependency as lua


class LuaDependencyTests(unittest.TestCase):
    def test_archive_and_populated_sources_reject_drift(self):
        archive = OPTIONS.archive
        with tempfile.TemporaryDirectory(prefix="shark-lua-source-") as temporary:
            root = Path(temporary)
            record = lua.populate(root / "build", archive)
            self.assertEqual(len(record["source_files"]), 75)
            self.assertEqual(record["compiled_files"], list(lua.SOURCES))
            self.assertEqual(record["license_files"], {"doc/readme.html": lua.entry()["source_sha256"]["doc/readme.html"]})
            header = root / "build/_deps/lua-src/src/lua.h"
            original = header.read_bytes()
            header.write_bytes(original + b"drift")
            with self.assertRaisesRegex(ValueError, "Populated Lua source differs"):
                lua.populate(root / "build", archive)
            self.assertEqual(header.read_bytes(), original + b"drift")
            damaged = root / "bad.tar.gz"
            data = archive.read_bytes()
            damaged.write_bytes(bytes([data[0] ^ 1]) + data[1:])
            with self.assertRaisesRegex(ValueError, "Lua archive SHA-256 differs"):
                lua.populate(root / "new-build", damaged)
            self.assertFalse((root / "new-build/_deps").exists())

    def test_changed_lock_metadata_is_rejected(self):
        locked = lua.entry()
        for field, replacement in (("version", "latest"), ("sources", ["lua.c"]),
                                   ("license_files", []), ("patches", ["unreviewed"])):
            with self.subTest(field=field), tempfile.TemporaryDirectory(prefix="shark-lua-lock-") as temporary:
                source = Path(temporary)
                (source / "dependencies.lock.json").write_text(
                    json.dumps({"dependencies": {"Lua": dict(locked, **{field: replacement})}}), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "integration review"):
                    lua.entry(source)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", type=Path, required=True)
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
