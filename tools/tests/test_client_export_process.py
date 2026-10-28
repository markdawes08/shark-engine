"""Actual CPU export ownership, publication fences and package consumption."""
from __future__ import annotations

import argparse
from dataclasses import asdict
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import export_runtime
import package_runtime as package
from process_runner import run_process

ROOT = Path(__file__).resolve().parents[2]
ARGS = None


class ClientExportProcessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workspace = tempfile.TemporaryDirectory(prefix="shark-export-process-")
        cls.base = Path(cls.workspace.name)
        cls.prepared = cls.base / "prepared"
        # ProjectFiles locks a writable project root. Keep the checkout usable
        # read-only in WSL and qualify the copied source through the real owner.
        sources = cls.base / "sources"
        shutil.copytree(ROOT / "assets/FeatureWorld", sources / "assets/FeatureWorld",
                        ignore=shutil.ignore_patterns(".shark-write.lock"))
        gltf = sources / "tests/fixtures/gltf_source"
        gltf.mkdir(parents=True)
        for name in ("embedded.gltf", "LICENSE"):
            shutil.copyfile(ROOT / "tests/fixtures/gltf_source" / name, gltf / name)
        result = run_process([str(ARGS.prepare), "--source-root", str(sources),
                              "--output", str(cls.prepared)], cwd=cls.base, timeout=15)
        if result.returncode or result.timed_out or result.cleanup_errors:
            raise AssertionError(asdict(result))

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="case-", dir=self.base)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.project = self.root / "caf\u00e9 \u4e16\u754c"
        shutil.copytree(ROOT / "assets/FeatureWorld", self.project,
                        ignore=shutil.ignore_patterns(".shark-write.lock"))
        self.project_file = self.project / "FeatureWorld.sharkproject"
        self.scene = self.project / "scenes/FeatureWorld.sharkscene"
        self.camera = "20000000-0000-4000-8000-000000000004"
        self.sidecars = []
        # Reuse executable, independently validated scene shader fixtures. No compiler/GPU.
        text = (ROOT / "tests/gpu_shader_fixture.hpp").read_text(encoding="utf-8")
        self.vertex, self.pixel = self.root / "vertex.spv", self.root / "pixel.spv"
        for kind, path in (("vs", self.vertex), ("ps", self.pixel)):
            body = re.search(r"scene_" + kind + r"_words\{(.*?)\};", text, re.S).group(1)
            words = [int(word, 16) for word in re.findall(r"0x([0-9a-fA-F]+)U", body)]
            path.write_bytes(struct.pack(f"<{len(words)}I", *words))

    def command(self, output="content", camera=None):
        args = [str(ARGS.exporter), "--project", str(self.project_file),
                "--camera", camera or self.camera, "--content-target", "windows-x64-export-test",
                "--vertex-shader", str(self.vertex), "--pixel-shader", str(self.pixel),
                "--output", str(self.root / output)]
        for sidecar in self.sidecars:
            args.extend(["--sidecar", sidecar])
        return args

    def session(self, output="content"):
        return export_runtime.ExportSession(self.command(output), cwd=self.root, timeout=15)

    def imported(self):
        self.project = self.root / "imported project"
        shutil.copytree(self.prepared / "imported", self.project,
                        ignore=shutil.ignore_patterns(".shark-write.lock"))
        self.project_file = self.project / "project.sharkproject"
        self.scene = self.project / "scene.sharkscene"
        self.camera = "13e00000-0000-4000-8000-000000000004"
        self.sidecars = ["embedded.gltf.sharkasset"]

    def test_saved_project_lock_content_and_atomic_publication(self):
        original = {path: path.read_bytes() for path in (self.project_file, self.scene)}
        with self.session() as session:
            row = session.content
            self.assertEqual(row["project"], "20000000-0000-4000-8000-000000000001")
            self.assertEqual(row["scene"], "20000000-0000-4000-8000-000000000003")
            self.assertEqual(row["camera"], self.camera)
            self.assertEqual(row["sources"], [])
            self.assertFalse(row["dirty"])
            self.assertEqual(set(row["files"]), {"bin/startup.sharkstart", "bin/content/startup.sharkscene",
                                                 "bin/assets/scene-vs.spv", "bin/assets/scene-ps.spv"})
            competing = run_process(self.command("competing"), cwd=self.root, timeout=5)
            self.assertEqual(competing.returncode, 2, asdict(competing))
            self.assertEqual(json.loads(competing.stdout)["error"]["code"], "conflict")
            payload = {name: (self.root / "content" / name).read_bytes() for name in row["files"]}
            for name, record in row["files"].items():
                self.assertEqual(record, {"bytes": len(payload[name]),
                                          "sha256": hashlib.sha256(payload[name]).hexdigest()})
            package.publish(self.root / "published", payload, {}, before_publish=session.validate)
            session.release()
        self.assertEqual(original, {path: path.read_bytes() for path in original})
        with self.session("repeat") as repeated:
            self.assertEqual(row, repeated.content)
            repeated.validate()
            repeated.release()
        if ARGS.runtime:
            result = run_process([str(ARGS.runtime), "--check-package",
                                  str(self.root / "published/bin/startup.sharkstart")],
                                 cwd=self.root, timeout=5)
            self.assertEqual(result.returncode, 0, asdict(result))
            inspected = json.loads(result.stdout)
            self.assertEqual(inspected["status"], "PASS")
            self.assertIs(inspected["renderer_initialized"], False)

    def test_external_project_scene_and_shader_edits_block_publication(self):
        for index, path in enumerate((self.project_file, self.scene, self.vertex)):
            with self.subTest(path=path.name):
                before = path.read_bytes()
                with self.session(f"stale-{index}") as session:
                    path.write_bytes(before + (b" " if path != self.vertex else b"\0\0\0\0"))
                    output = self.root / f"stale-package-{index}"
                    with self.assertRaises(export_runtime.ExportError):
                        package.publish(output, {"sentinel": b"candidate"}, {},
                                        before_publish=session.validate)
                    self.assertFalse(output.exists())
                    self.assertFalse(list(self.root.glob(".runtime-package-*")))
                self.assertEqual(path.read_bytes(), before + (b" " if path != self.vertex else b"\0\0\0\0"))
                path.write_bytes(before)

    def test_imported_cached_mesh_and_attribution_use_actual_project(self):
        self.imported()
        with self.session() as session:
            row = session.content
            self.assertIn("bin/content/13e00000-0000-4000-8000-00000000000a.sharkmesh", row["files"])
            self.assertEqual([entry["asset"] for entry in row["sources"]],
                             ["13e00000-0000-4000-8000-00000000000a"])
            self.assertTrue(row["sources"][0]["license"])
            session.validate()
            session.release()
        with self.session("stale") as session:
            source = self.project / "embedded.gltf"
            source.write_bytes(source.read_bytes() + b" ")
            with self.assertRaises(export_runtime.ExportError):
                session.validate()

    def test_wrong_camera_and_missing_sidecar_are_structured_errors(self):
        result = run_process(self.command(camera="20000000-0000-4000-8000-000000000005"),
                             cwd=self.root, timeout=5)
        self.assertEqual(result.returncode, 2, asdict(result))
        row = json.loads(result.stdout)
        self.assertEqual(row["status"], "ERROR")
        self.assertIs(row["renderer_initialized"], False)
        self.assertFalse((self.root / "content").exists())
        self.imported()
        self.sidecars = []
        with self.assertRaises(export_runtime.ExportError):
            with self.session("missing-catalog"):
                self.fail("Unlisted external asset was accepted")

    def test_deadline_cleanup_releases_native_project_owner(self):
        with self.session() as session:
            session.deadline = time.monotonic() - 1
            with self.assertRaisesRegex(export_runtime.ExportError, "deadline"):
                session.validate()
            child = session.process
        self.assertIsNotNone(child.poll())
        with self.session("after-timeout") as session:
            session.validate()
            session.release()

    def test_public_command_reports_missing_inputs_and_unsupported_target(self):
        command = [sys.executable, str(ROOT / "tools/package_runtime.py"), "export"]
        result = run_process(command, cwd=self.root, timeout=5)
        self.assertEqual(result.returncode, 2, asdict(result))
        row = json.loads(result.stdout)
        self.assertEqual(row["format"], "shark.client-export")
        self.assertIs(row["published"], False)
        for name in ("build-dir", "crt-dir", "output", "shaders", "exporter", "project"):
            command.extend(["--" + name, str(self.root / name)])
        command += ["--camera", self.camera, "--content-target", "test", "--target", "unsupported"]
        result = run_process(command, cwd=self.root, timeout=5)
        self.assertEqual(result.returncode, 2, asdict(result))
        row = json.loads(result.stdout)
        self.assertEqual(row["error"]["code"], "capability_unavailable")
        self.assertIs(row["published"], False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exporter", type=Path, required=True)
    parser.add_argument("--prepare", type=Path, required=True)
    parser.add_argument("--runtime", type=Path)
    ARGS, remaining = parser.parse_known_args()
    ARGS.exporter, ARGS.prepare = ARGS.exporter.resolve(), ARGS.prepare.resolve()
    if ARGS.runtime:
        ARGS.runtime = ARGS.runtime.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
